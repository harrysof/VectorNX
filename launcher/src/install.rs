//! APK extraction, mandatory asset-pack build, NRO/config installation.

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;

use anyhow::{bail, Context, Result};
use zip::ZipArchive;

use crate::apk;
use crate::pack;

const LIBS: &[&str] = &["libmain.so", "libunity.so", "libil2cpp.so"];
const REQUIRED_DATA: &[&str] = &[
    "globalgamemanagers",
    "level0",
    "level1",
    "level2",
    "sharedassets0.assets",
    "Managed/Metadata/global-metadata.dat",
    "boot.config",
];

#[derive(Debug, Clone)]
pub struct InstallOptions {
    pub apk: PathBuf,
    pub output: PathBuf,
    pub nro: PathBuf,
    pub language: String,
    pub width: u32,
    pub height: u32,
    pub log: bool,
    pub replace_assets: bool,
}

#[derive(Debug, Clone)]
pub struct InstallSummary {
    pub output: PathBuf,
    pub entries: u64,
    pub pack_bytes: u64,
    pub data_bytes: u64,
    pub nro_version: Option<String>,
}

#[derive(Debug, Clone)]
pub enum InstallEvent {
    Stage {
        name: String,
        progress: f32,
        detail: String,
    },
    Done(InstallSummary),
    Error(String),
}

impl InstallEvent {
    fn stage(name: impl Into<String>, progress: f32, detail: impl Into<String>) -> Self {
        Self::Stage {
            name: name.into(),
            progress: progress.clamp(0.0, 1.0),
            detail: detail.into(),
        }
    }
}

fn cancelled(cancel: &AtomicBool) -> bool {
    cancel.load(Ordering::Relaxed)
}

fn emit(tx: &Sender<InstallEvent>, event: InstallEvent) {
    let _ = tx.send(event);
}

pub fn resolve_output(selected: &Path) -> PathBuf {
    let text = selected.to_string_lossy().replace('/', "\\").to_lowercase();
    if text.ends_with("switch\\vector") || text.ends_with("\\vector") {
        selected.to_path_buf()
    } else if text.ends_with("\\switch") {
        selected.join("vector")
    } else {
        selected.join("switch").join("vector")
    }
}

fn ensure_relative(rel: &str) -> Result<()> {
    if rel.is_empty()
        || rel.starts_with('/')
        || rel.contains('\\')
        || rel.split('/').any(|p| p.is_empty() || p == "." || p == "..")
    {
        bail!("unsafe zip path: {}", rel);
    }
    Ok(())
}

fn write_config(out: &Path, opts: &InstallOptions) -> Result<()> {
    let language = if opts.language.trim().is_empty() {
        "auto"
    } else {
        opts.language.trim()
    };
    let text = format!(
        "# Vector (vector_nx) config\n\
         #\n\
         # language: 'auto' follows the Switch system language, or an ISO code.\n\
         language={language}\n\
         \n\
         # Internal render size. The compositor upscales to the panel.\n\
         render_width={w}\n\
         render_height={h}\n\
         \n\
         # Hard-locked at 60 fps; 30 is ignored by the loader.\n\
         framerate=60\n\
         \n\
         # log: 1 writes debug.log; 0 is the play setting.\n\
         log={log}\n",
        language = language,
        w = opts.width,
        h = opts.height,
        log = if opts.log { 1 } else { 0 },
    );
    fs::write(out.join("config.txt"), text)?;
    Ok(())
}

/// A split group: the base name and the zip indices of its parts.
type SplitMap = BTreeMap<String, Vec<(u32, usize)>>;

fn collect_assets(archive: &mut ZipArchive<File>) -> Result<SplitMap> {
    let mut groups: SplitMap = BTreeMap::new();
    for i in 0..archive.len() {
        let entry = archive.by_index(i)?;
        let name = entry.name().map(|c| c.to_string()).unwrap_or_default();
        if !name.starts_with("assets/bin/Data/") || name.ends_with('/') {
            continue;
        }
        let rel = &name["assets/bin/Data/".len()..];
        ensure_relative(rel)?;
        if let Some(pos) = rel.rfind(".split") {
            let suffix = &rel[pos + 6..];
            if !suffix.is_empty() && suffix.chars().all(|c| c.is_ascii_digit()) {
                let base = rel[..pos].to_string();
                let part = suffix.parse::<u32>().unwrap_or(0);
                groups.entry(base).or_default().push((part, i));
                continue;
            }
        }
        groups.entry(rel.to_string()).or_default().push((0, i));
    }
    for parts in groups.values_mut() {
        parts.sort_by_key(|(part, _)| *part);
    }
    Ok(groups)
}

fn copy_entry(
    archive: &mut ZipArchive<File>,
    index: usize,
    out: &mut impl Write,
    cancel: &AtomicBool,
) -> Result<u64> {
    let mut entry = archive.by_index(index)?;
    let mut buf = vec![0u8; 1 << 20];
    let mut total = 0u64;
    loop {
        if cancelled(cancel) {
            bail!("cancelled");
        }
        let n = entry.read(&mut buf)?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n])?;
        total += n as u64;
    }
    Ok(total)
}

pub fn run(
    opts: InstallOptions,
    tx: Sender<InstallEvent>,
    cancel: &AtomicBool,
) -> Result<InstallSummary> {
    let result = run_inner(&opts, &tx, cancel);
    match &result {
        Ok(summary) => emit(&tx, InstallEvent::Done(summary.clone())),
        Err(err) => emit(&tx, InstallEvent::Error(err.to_string())),
    }
    result
}

fn run_inner(
    opts: &InstallOptions,
    tx: &Sender<InstallEvent>,
    cancel: &AtomicBool,
) -> Result<InstallSummary> {
    emit(
        tx,
        InstallEvent::stage("Validating input", 0.01, "Reading APK manifest"),
    );
    let info = apk::inspect(&opts.apk)?;
    if !info.package_ok() {
        bail!(
            "wrong package: {} (expected {})",
            info.package,
            apk::VECTOR_PACKAGE
        );
    }
    if !info.version_ok() {
        bail!(
            "wrong version: {} (expected {})",
            info.version_name,
            apk::VECTOR_VERSION
        );
    }
    let (nro_size, nro_version) = apk::inspect_nro(&opts.nro)?;
    if nro_size < 1 << 20 {
        bail!("loader NRO looks too small");
    }

    let out = resolve_output(&opts.output);
    fs::create_dir_all(&out)?;
    fs::create_dir_all(out.join("assets/bin/Data"))?;
    fs::create_dir_all(out.join("save"))?;

    if opts.replace_assets {
        let data = out.join("assets/bin/Data");
        if data.exists() {
            emit(
                tx,
                InstallEvent::stage("Preparing output", 0.02, "Replacing existing game data"),
            );
            fs::remove_dir_all(&data).with_context(|| format!("remove {}", data.display()))?;
            fs::create_dir_all(&data)?;
        }
    }

    let archive_file = File::open(&opts.apk)?;
    let mut archive = ZipArchive::new(archive_file)?;

    emit(
        tx,
        InstallEvent::stage("Extracting libraries", 0.03, "libmain / libunity / libil2cpp"),
    );
    for (i, lib) in LIBS.iter().enumerate() {
        if cancelled(cancel) {
            bail!("cancelled");
        }
        let entry_name = format!("lib/arm64-v8a/{lib}");
        let mut entry = archive
            .by_name(&entry_name)
            .with_context(|| format!("missing {entry_name}"))?;
        let path = out.join(lib);
        let mut file = BufWriter::new(File::create(&path)?);
        std::io::copy(&mut entry, &mut file)?;
        file.flush()?;
        emit(
            tx,
            InstallEvent::stage(
                "Extracting libraries",
                0.03 + 0.02 * (i + 1) as f32,
                lib.to_string(),
            ),
        );
    }

    let groups = collect_assets(&mut archive)?;
    let total_bytes: u64 = groups
        .values()
        .flat_map(|parts| parts.iter())
        .filter_map(|(_, idx)| archive.by_index(*idx).ok().map(|e| e.size()))
        .sum();
    let mut done = 0u64;
    let data_root = out.join("assets/bin/Data");
    emit(
        tx,
        InstallEvent::stage(
            "Extracting game data",
            0.05,
            format!("{} files", groups.len()),
        ),
    );
    for (rel, parts) in &groups {
        if cancelled(cancel) {
            bail!("cancelled");
        }
        let path = data_root.join(rel.replace('/', std::path::MAIN_SEPARATOR_STR));
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut file = BufWriter::new(File::create(&path)?);
        for (_, index) in parts {
            done += copy_entry(&mut archive, *index, &mut file, cancel)?;
        }
        file.flush()?;
        let frac = if total_bytes > 0 {
            done as f32 / total_bytes as f32
        } else {
            1.0
        };
        emit(
            tx,
            InstallEvent::stage(
                "Extracting game data",
                0.05 + 0.50 * frac,
                format!("{rel}  ({:.1} MB)", done as f64 / 1_048_576.0),
            ),
        );
    }

    for required in REQUIRED_DATA {
        let path = data_root.join(required.replace('/', std::path::MAIN_SEPARATOR_STR));
        if !path.is_file() {
            bail!("required file missing after extraction: {}", required);
        }
    }

    emit(
        tx,
        InstallEvent::stage(
            "Building mandatory asset pack",
            0.55,
            "Packing assets/bin/Data",
        ),
    );
    let assets_root = out.join("assets");
    let stats = pack::build_pack(&assets_root, &out, &mut |p| {
        let (base, span) = if p.stage == "Verifying asset pack" {
            (0.86, 0.12)
        } else {
            (0.55, 0.31)
        };
        let frac = if p.total > 0 {
            p.done as f32 / p.total as f32
        } else {
            1.0
        };
        emit(
            tx,
            InstallEvent::stage(p.stage, base + span * frac, format!("{:.1} MB", p.done as f64 / 1_048_576.0)),
        );
    })?;

    emit(
        tx,
        InstallEvent::stage("Installing loader", 0.985, "vector_nx.nro"),
    );
    fs::copy(&opts.nro, out.join("vector_nx.nro"))?;

    write_config(&out, opts)?;

    emit(
        tx,
        InstallEvent::stage("Finalizing", 0.995, "config.txt and save/"),
    );
    fs::create_dir_all(out.join("save"))?;

    Ok(InstallSummary {
        output: out,
        entries: stats.entries,
        pack_bytes: stats.pack_bytes,
        data_bytes: stats.data_bytes,
        nro_version,
    })
}
