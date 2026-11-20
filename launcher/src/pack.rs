//! Builder and verifier for the loader's read-only `assets.nxpack` pair.
//! The format is byte-for-byte compatible with source/asset_pack.c.

use std::fs::{self, File};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{bail, Context, Result};
use walkdir::WalkDir;

const PACK_MAGIC: &[u8; 8] = b"SSNXPAK1";
const INDEX_MAGIC: &[u8; 8] = b"SSNXIDX1";
const PACK_VERSION: u32 = 2;
const FNV_OFFSET: u64 = 1_469_598_103_934_665_603;
const FNV_PRIME: u64 = 1_099_511_628_211;
const PACK_HEADER_SIZE: u64 = 40;
const INDEX_HEADER_SIZE: u64 = 56;
const DISK_ENTRY_SIZE: u64 = 24;

#[derive(Debug, Clone)]
struct FileEntry {
    rel: String,
    full: PathBuf,
    size: u64,
    offset: u64,
    path_offset: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct PackProgress {
    pub stage: &'static str,
    pub done: u64,
    pub total: u64,
}

pub type ProgressFn<'a> = &'a mut dyn FnMut(PackProgress);

#[allow(dead_code)]
#[derive(Debug, Clone, Default)]
pub struct PackStats {
    pub entries: u64,
    pub data_bytes: u64,
    pub pack_bytes: u64,
    pub index_bytes: u64,
    pub pack_id: u64,
    pub data_checksum: u64,
}

fn fnv_bytes(mut hash: u64, bytes: &[u8]) -> u64 {
    for b in bytes {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

fn align16(v: u64) -> u64 {
    (v + 15) & !15
}

fn write_u32(w: &mut impl Write, v: u32) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}
fn write_u64(w: &mut impl Write, v: u64) -> std::io::Result<()> {
    w.write_all(&v.to_le_bytes())
}

fn collect(assets_root: &Path) -> Result<Vec<FileEntry>> {
    if !assets_root.is_dir() {
        bail!("assets directory not found: {}", assets_root.display());
    }
    let mut entries = Vec::new();
    for item in WalkDir::new(assets_root).follow_links(false) {
        let item = item?;
        if !item.file_type().is_file() {
            continue;
        }
        let path = item.path();
        let rel = path
            .strip_prefix(assets_root)
            .with_context(|| format!("strip {}", path.display()))?
            .to_string_lossy()
            .replace('\\', "/");
        if rel.is_empty()
            || rel == "assets.nxpack"
            || rel == "assets.nxidx"
            || rel == "assets.nxpack.tmp"
            || rel == "assets.nxidx.tmp"
            || rel.split('/').any(|p| p == "..")
        {
            continue;
        }
        let size = item.metadata()?.len();
        entries.push(FileEntry {
            rel,
            full: path.to_path_buf(),
            size,
            offset: 0,
            path_offset: 0,
        });
    }
    entries.sort_by(|a, b| a.rel.as_bytes().cmp(b.rel.as_bytes()));
    if entries.is_empty() {
        bail!("no files found under {}", assets_root.display());
    }
    if entries.len() > 100_000 {
        bail!("too many files ({}), limit is 100000", entries.len());
    }
    Ok(entries)
}

pub fn build_pack(assets_root: &Path, out_root: &Path, progress: ProgressFn<'_>) -> Result<PackStats> {
    let mut entries = collect(assets_root)?;
    fs::create_dir_all(out_root)?;

    let mut pack_size = PACK_HEADER_SIZE;
    let mut paths_size = 0u64;
    for e in &mut entries {
        pack_size = align16(pack_size);
        e.offset = pack_size;
        pack_size += e.size;
        let len = e.rel.len();
        if len > u32::MAX as usize || paths_size + len as u64 + 1 > u32::MAX as u64 {
            bail!("asset path table too large");
        }
        e.path_offset = paths_size as u32;
        paths_size += len as u64 + 1;
    }

    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() as u64;
    let mut pack_id = FNV_OFFSET;
    for e in &entries {
        pack_id = fnv_bytes(pack_id, e.rel.as_bytes());
        pack_id = fnv_bytes(pack_id, &e.size.to_le_bytes());
    }
    pack_id = fnv_bytes(pack_id, &nonce.to_le_bytes());
    if pack_id == 0 {
        pack_id = 1;
    }

    let pack_tmp = out_root.join("assets.nxpack.tmp");
    let index_tmp = out_root.join("assets.nxidx.tmp");
    let pack_path = out_root.join("assets.nxpack");
    let index_path = out_root.join("assets.nxidx");
    let _ = fs::remove_file(&pack_tmp);
    let _ = fs::remove_file(&index_tmp);

    let mut data_checksum = FNV_OFFSET;
    {
        let file = File::create(&pack_tmp)?;
        let mut out = BufWriter::with_capacity(1 << 20, file);
        out.write_all(PACK_MAGIC)?;
        write_u32(&mut out, PACK_VERSION)?;
        write_u32(&mut out, 0)?;
        write_u64(&mut out, pack_id)?;
        write_u64(&mut out, pack_size)?;
        write_u64(&mut out, 0)?; // checksum patched below
        let mut written = PACK_HEADER_SIZE;
        for e in &entries {
            if e.offset > written {
                out.write_all(&vec![0u8; (e.offset - written) as usize])?;
                written = e.offset;
            }
            let mut input = BufReader::with_capacity(1 << 20, File::open(&e.full)?);
            let mut buf = vec![0u8; 1 << 20];
            loop {
                let n = input.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                out.write_all(&buf[..n])?;
                data_checksum = fnv_bytes(data_checksum, &buf[..n]);
                written += n as u64;
                progress(PackProgress {
                    stage: "Building asset pack",
                    done: written,
                    total: pack_size,
                });
            }
        }
        if written != pack_size {
            bail!("internal pack size mismatch");
        }
        out.seek(SeekFrom::Start(0))?;
        out.write_all(PACK_MAGIC)?;
        write_u32(&mut out, PACK_VERSION)?;
        write_u32(&mut out, 0)?;
        write_u64(&mut out, pack_id)?;
        write_u64(&mut out, pack_size)?;
        write_u64(&mut out, data_checksum)?;
        out.flush()?;
        out.get_ref().sync_all()?;
    }

    let mut entry_blob = Vec::with_capacity(entries.len() * DISK_ENTRY_SIZE as usize);
    let mut path_blob = Vec::with_capacity(paths_size as usize);
    for e in &entries {
        entry_blob.extend_from_slice(&e.offset.to_le_bytes());
        entry_blob.extend_from_slice(&e.size.to_le_bytes());
        entry_blob.extend_from_slice(&(e.path_offset).to_le_bytes());
        entry_blob.extend_from_slice(&(e.rel.len() as u32).to_le_bytes());
        path_blob.extend_from_slice(e.rel.as_bytes());
        path_blob.push(0);
    }
    if path_blob.len() as u64 != paths_size {
        bail!("internal path table mismatch");
    }
    let mut index_checksum = fnv_bytes(FNV_OFFSET, &entry_blob);
    index_checksum = fnv_bytes(index_checksum, &path_blob);

    {
        let file = File::create(&index_tmp)?;
        let mut out = BufWriter::new(file);
        out.write_all(INDEX_MAGIC)?;
        write_u32(&mut out, PACK_VERSION)?;
        write_u32(&mut out, entries.len() as u32)?;
        write_u64(&mut out, pack_id)?;
        write_u64(&mut out, pack_size)?;
        write_u64(&mut out, paths_size)?;
        write_u64(&mut out, index_checksum)?;
        write_u64(&mut out, data_checksum)?;
        out.write_all(&entry_blob)?;
        out.write_all(&path_blob)?;
        out.flush()?;
        out.get_ref().sync_all()?;
    }

    fs::rename(&pack_tmp, &pack_path)?;
    fs::rename(&index_tmp, &index_path)?;

    let stats = verify_pack(&pack_path, &index_path, progress)?;
    Ok(stats)
}

fn read_exact_at(file: &mut File, buf: &mut [u8], off: u64) -> Result<()> {
    file.seek(SeekFrom::Start(off))?;
    file.read_exact(buf)?;
    Ok(())
}

pub fn verify_pack(pack_path: &Path, index_path: &Path, progress: ProgressFn<'_>) -> Result<PackStats> {
    let pack_meta = fs::metadata(pack_path)?;
    let index_meta = fs::metadata(index_path)?;
    let mut pack = File::open(pack_path)?;
    let mut index = File::open(index_path)?;

    let mut ph = [0u8; PACK_HEADER_SIZE as usize];
    read_exact_at(&mut pack, &mut ph, 0)?;
    let mut ih = [0u8; INDEX_HEADER_SIZE as usize];
    read_exact_at(&mut index, &mut ih, 0)?;

    if &ph[0..8] != PACK_MAGIC || &ih[0..8] != INDEX_MAGIC {
        bail!("pack magic mismatch");
    }
    let p_version = u32::from_le_bytes(ph[8..12].try_into().unwrap());
    let i_version = u32::from_le_bytes(ih[8..12].try_into().unwrap());
    let count = u32::from_le_bytes(ih[12..16].try_into().unwrap());
    let pack_id = u64::from_le_bytes(ph[16..24].try_into().unwrap());
    let index_pack_id = u64::from_le_bytes(ih[16..24].try_into().unwrap());
    let ph_file_size = u64::from_le_bytes(ph[24..32].try_into().unwrap());
    let ph_data_checksum = u64::from_le_bytes(ph[32..40].try_into().unwrap());
    let ih_pack_size = u64::from_le_bytes(ih[24..32].try_into().unwrap());
    let paths_size = u64::from_le_bytes(ih[32..40].try_into().unwrap());
    let index_checksum = u64::from_le_bytes(ih[40..48].try_into().unwrap());
    let ih_data_checksum = u64::from_le_bytes(ih[48..56].try_into().unwrap());

    if p_version != PACK_VERSION || i_version != PACK_VERSION {
        bail!("pack version mismatch");
    }
    if pack_id != index_pack_id || ph_data_checksum != ih_data_checksum {
        bail!("pack/index header mismatch");
    }
    if ph_file_size != pack_meta.len() || ih_pack_size != pack_meta.len() {
        bail!("pack size mismatch");
    }
    if count == 0 || count > 100_000 {
        bail!("invalid entry count");
    }
    let expected_index = INDEX_HEADER_SIZE + count as u64 * DISK_ENTRY_SIZE + paths_size;
    if expected_index != index_meta.len() {
        bail!("index size mismatch");
    }

    let mut entries = vec![0u8; count as usize * DISK_ENTRY_SIZE as usize];
    read_exact_at(&mut index, &mut entries, INDEX_HEADER_SIZE)?;
    let mut paths = vec![0u8; paths_size as usize];
    read_exact_at(&mut index, &mut paths, INDEX_HEADER_SIZE + entries.len() as u64)?;
    let mut checksum = fnv_bytes(FNV_OFFSET, &entries);
    checksum = fnv_bytes(checksum, &paths);
    if checksum != index_checksum {
        bail!("index checksum mismatch");
    }

    let mut data_bytes = 0u64;
    let mut parse: Vec<(String, u64, u64)> = Vec::with_capacity(count as usize);
    for i in 0..count as usize {
        let b = &entries[i * 24..i * 24 + 24];
        let offset = u64::from_le_bytes(b[0..8].try_into().unwrap());
        let size = u64::from_le_bytes(b[8..16].try_into().unwrap());
        let path_off = u32::from_le_bytes(b[16..20].try_into().unwrap()) as usize;
        let path_len = u32::from_le_bytes(b[20..24].try_into().unwrap()) as usize;
        if path_len == 0 || path_off + path_len + 1 > paths.len() || paths[path_off + path_len] != 0 {
            bail!("invalid path entry {}", i);
        }
        let rel = std::str::from_utf8(&paths[path_off..path_off + path_len])?;
        if rel.contains('\\') || rel.starts_with('/') || rel.split('/').any(|p| p == "..") {
            bail!("invalid path {}", rel);
        }
        if offset < PACK_HEADER_SIZE || offset + size > pack_meta.len() {
            bail!("payload out of range for {}", rel);
        }
        if i > 0 && rel <= parse[i - 1].0.as_str() {
            bail!("path table is not sorted");
        }
        parse.push((rel.to_string(), offset, size));
        data_bytes += size;
    }

    let mut actual = FNV_OFFSET;
    let mut done = 0u64;
    let mut buf = vec![0u8; 1 << 20];
    for (_rel, offset, size) in &parse {
        let mut remaining = *size;
        let mut at = *offset;
        while remaining > 0 {
            let n = remaining.min(buf.len() as u64) as usize;
            read_exact_at(&mut pack, &mut buf[..n], at)?;
            actual = fnv_bytes(actual, &buf[..n]);
            remaining -= n as u64;
            at += n as u64;
            done += n as u64;
            progress(PackProgress {
                stage: "Verifying asset pack",
                done,
                total: data_bytes,
            });
        }
    }
    if actual != ph_data_checksum {
        bail!("payload checksum mismatch");
    }

    Ok(PackStats {
        entries: count as u64,
        data_bytes,
        pack_bytes: pack_meta.len(),
        index_bytes: index_meta.len(),
        pack_id,
        data_checksum: actual,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_and_verify_fixture() {
        let dir = tempfile::tempdir().unwrap();
        let assets = dir.path().join("assets");
        fs::create_dir_all(assets.join("bin/Data/Managed")).unwrap();
        fs::write(assets.join("bin/Data/level1"), vec![7u8; 100_000]).unwrap();
        fs::write(assets.join("bin/Data/boot.config"), b"boot-config-test").unwrap();
        fs::write(assets.join("bin/Data/Managed/global-metadata.dat"), vec![3u8; 5000]).unwrap();
        fs::write(assets.join("bin/Data/empty"), b"").unwrap();
        let out = dir.path().join("out");
        let stats = build_pack(&assets, &out, &mut |_| {}).unwrap();
        assert_eq!(stats.entries, 4);
        assert_eq!(stats.data_bytes, 105_018);
        assert!(out.join("assets.nxpack").is_file());
        assert!(out.join("assets.nxidx").is_file());
        let verified = verify_pack(&out.join("assets.nxpack"), &out.join("assets.nxidx"), &mut |_| {}).unwrap();
        assert_eq!(verified.data_checksum, stats.data_checksum);
    }
}
