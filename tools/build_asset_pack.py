#!/usr/bin/env python3
"""build_asset_pack.py -- build VectorNX's assets.nxpack / assets.nxidx on a PC.

The loader can mount a single read-only pack and serve every asset open from a
sorted in-RAM path index. This script builds that pair; it does not modify the
APK or the extracted assets.

Usage:
    python tools/build_asset_pack.py --assets "E:\\switch\\vector\\assets" ^
        --out "E:\\switch\\vector"
    python tools/build_asset_pack.py --assets stage/vector/assets --out stage/vector
    python tools/build_asset_pack.py --assets stage/vector/assets --verify-only

The output names are fixed: <out>/assets.nxpack and <out>/assets.nxidx.

Format (must stay in sync with source/asset_pack.c):
  PackHeader  : <8sIIQQQ  = 40 bytes  magic SSNXPAK1
  IndexHeader : <8sIIQQQQQ = 56 bytes magic SSNXIDX1
  DiskEntry   : <QQII     = 24 bytes
  payload     : each file 16-byte aligned, file order = sorted path order
  path blob   : NUL-terminated sorted paths, offsets into this blob

Checksums are FNV-1a 64 with the reference port's offset basis
1469598103934665603 (not the standard 14695981039346656037).
"""
import argparse
import os
import struct
import sys
import time

PACK_MAGIC = b"SSNXPAK1"
INDEX_MAGIC = b"SSNXIDX1"
PACK_VERSION = 2
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
PACK_HEADER = struct.Struct("<8sIIQQQ")
INDEX_HEADER = struct.Struct("<8sIIQQQQQ")
DISK_ENTRY = struct.Struct("<QQII")
PACK_HEADER_SIZE = PACK_HEADER.size          # 40
INDEX_HEADER_SIZE = INDEX_HEADER.size        # 56
DISK_ENTRY_SIZE = DISK_ENTRY.size            # 24
ALIGN = 16


def fnv1a(data, value=FNV_OFFSET):
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def align16(value):
    return (value + (ALIGN - 1)) & ~(ALIGN - 1)


def find_assets_root(path):
    """Accept either .../vector/assets or .../vector."""
    path = os.path.abspath(path)
    if os.path.isdir(os.path.join(path, "bin")) or (
            not os.path.isdir(os.path.join(path, "assets"))):
        return path
    return os.path.join(path, "assets")


def collect(assets_root, out_root):
    files = []
    out_pack = os.path.abspath(os.path.join(out_root, "assets.nxpack"))
    out_index = os.path.abspath(os.path.join(out_root, "assets.nxidx"))
    skip = {out_pack, out_index, out_pack + ".tmp", out_index + ".tmp"}
    for directory, _dirs, names in os.walk(assets_root):
        for name in names:
            full = os.path.abspath(os.path.join(directory, name))
            if full in skip:
                continue
            if not os.path.isfile(full):
                continue
            relative = os.path.relpath(full, assets_root).replace(os.sep, "/")
            if relative.startswith("../"):
                continue
            files.append((relative, full, os.path.getsize(full)))
    files.sort(key=lambda item: item[0].encode("utf-8"))
    return files


def progress(label, done, total):
    if total <= 0:
        return
    percent = done * 100 // total
    if percent != getattr(progress, "_last", -1):
        progress._last = percent
        print(f"\r  {label}: {percent:3d}%  ({done:,}/{total:,} bytes)", end="", flush=True)


def build_pack(assets_root, out_root, verify=True):
    assets_root = find_assets_root(assets_root)
    out_root = os.path.abspath(out_root)
    if not os.path.isdir(assets_root):
        raise SystemExit(f"assets directory not found: {assets_root}")
    os.makedirs(out_root, exist_ok=True)

    files = collect(assets_root, out_root)
    if not files:
        raise SystemExit(f"no files found under {assets_root}")
    if len(files) > 100000:
        raise SystemExit(f"too many files: {len(files)} (limit 100000)")

    pack_size = PACK_HEADER_SIZE
    entries = []
    paths_size = 0
    total_data = 0
    for relative, _full, size in files:
        pack_size = align16(pack_size)
        encoded = relative.encode("utf-8")
        if not encoded or len(encoded) > 0xFFFFFFFF:
            raise SystemExit(f"path too long: {relative}")
        entries.append({
            "path": relative,
            "full": _full,
            "bytes": encoded,
            "size": size,
            "offset": pack_size,
            "path_offset": paths_size,
        })
        pack_size += size
        paths_size += len(encoded) + 1
        total_data += size

    nonce = int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF
    pack_id = FNV_OFFSET
    for entry in entries:
        pack_id = fnv1a(entry["bytes"], pack_id)
        pack_id = fnv1a(struct.pack("<Q", entry["size"]), pack_id)
    pack_id = fnv1a(struct.pack("<Q", nonce), pack_id)
    if pack_id == 0:
        pack_id = 1

    pack_tmp = os.path.join(out_root, "assets.nxpack.tmp")
    index_tmp = os.path.join(out_root, "assets.nxidx.tmp")
    pack_path = os.path.join(out_root, "assets.nxpack")
    index_path = os.path.join(out_root, "assets.nxidx")

    data_checksum = FNV_OFFSET
    print(f"Packing {len(files):,} files, {total_data:,} bytes -> {pack_path}")
    progress._last = -1
    with open(pack_tmp, "wb") as out:
        out.write(PACK_HEADER.pack(PACK_MAGIC, PACK_VERSION, 0, pack_id, pack_size, 0))
        written = PACK_HEADER_SIZE
        for entry in entries:
            padding = entry["offset"] - written
            if padding:
                out.write(b"\0" * padding)
                written += padding
            with open(entry["full"], "rb") as source:
                while True:
                    chunk = source.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
                    data_checksum = fnv1a(chunk, data_checksum)
                    written += len(chunk)
                    progress("data", min(written, pack_size), pack_size)
        if written != pack_size:
            raise SystemExit(f"internal size mismatch: wrote {written}, expected {pack_size}")
        out.seek(0)
        out.write(PACK_HEADER.pack(PACK_MAGIC, PACK_VERSION, 0, pack_id,
                                   pack_size, data_checksum))
        out.flush()
        os.fsync(out.fileno())
    print()

    entries_blob = bytearray()
    paths_blob = bytearray()
    for entry in entries:
        entries_blob += DISK_ENTRY.pack(entry["offset"], entry["size"],
                                        entry["path_offset"], len(entry["bytes"]))
        paths_blob += entry["bytes"] + b"\0"
    if len(entries_blob) != len(entries) * DISK_ENTRY_SIZE:
        raise SystemExit("internal entry table size mismatch")
    if len(paths_blob) != paths_size:
        raise SystemExit("internal path table size mismatch")

    index_checksum = fnv1a(bytes(entries_blob))
    index_checksum = fnv1a(bytes(paths_blob), index_checksum)
    index_header = INDEX_HEADER.pack(INDEX_MAGIC, PACK_VERSION, len(entries),
                                     pack_id, pack_size, paths_size,
                                     index_checksum, data_checksum)
    with open(index_tmp, "wb") as out:
        out.write(index_header)
        out.write(entries_blob)
        out.write(paths_blob)
        out.flush()
        os.fsync(out.fileno())

    os.replace(pack_tmp, pack_path)
    os.replace(index_tmp, index_path)
    print(f"  pack  {pack_path}  {pack_size:,} bytes")
    print(f"  index {index_path}  {INDEX_HEADER_SIZE + len(entries_blob) + paths_size:,} bytes")
    print(f"  pack_id 0x{pack_id:016x}  data_checksum 0x{data_checksum:016x}")

    if verify:
        verify_pack(pack_path, index_path, check_data=True)
    return pack_path, index_path


def verify_pack(pack_path, index_path, check_data=True):
    with open(pack_path, "rb") as f:
        pack_header = PACK_HEADER.unpack(f.read(PACK_HEADER_SIZE))
        pack_stat_size = os.fstat(f.fileno()).st_size
    with open(index_path, "rb") as f:
        index_header = INDEX_HEADER.unpack(f.read(INDEX_HEADER_SIZE))
        index_stat_size = os.fstat(f.fileno()).st_size

    (magic, version, _reserved, pack_id, pack_size, data_checksum) = pack_header
    (imagic, iversion, count, ipack_id, ipack_size, paths_size,
     index_checksum, idata_checksum) = index_header

    errors = []
    if magic != PACK_MAGIC:
        errors.append("pack magic")
    if imagic != INDEX_MAGIC:
        errors.append("index magic")
    if version != PACK_VERSION or iversion != PACK_VERSION:
        errors.append("version")
    if pack_id != ipack_id:
        errors.append("pack_id mismatch")
    if pack_size != pack_stat_size or ipack_size != pack_stat_size:
        errors.append("pack size mismatch")
    if data_checksum != idata_checksum:
        errors.append("data checksum mismatch between headers")
    if count == 0 or count > 100000:
        errors.append("entry count")
    if paths_size == 0 or paths_size > (64 << 20):
        errors.append("path blob size")

    expected_index = INDEX_HEADER_SIZE + count * DISK_ENTRY_SIZE + paths_size
    if expected_index != index_stat_size:
        errors.append(f"index size {index_stat_size} != {expected_index}")

    entries = []
    paths = b""
    if not errors:
        with open(index_path, "rb") as f:
            f.seek(INDEX_HEADER_SIZE)
            raw_entries = f.read(count * DISK_ENTRY_SIZE)
            paths = f.read(paths_size)
        for i in range(count):
            offset, size, path_offset, path_length = DISK_ENTRY.unpack_from(
                raw_entries, i * DISK_ENTRY_SIZE)
            if path_length == 0 or path_offset + path_length + 1 > len(paths):
                errors.append(f"entry {i} path bounds")
                break
            raw_path = paths[path_offset:path_offset + path_length]
            if paths[path_offset + path_length] != 0:
                errors.append(f"entry {i} path not NUL-terminated")
                break
            try:
                path = raw_path.decode("utf-8")
            except UnicodeDecodeError:
                errors.append(f"entry {i} path not UTF-8")
                break
            if not path or path.startswith("/") or "\\" in path or ":" in path:
                errors.append(f"entry {i} invalid path {path!r}")
                break
            if i and path <= entries[-1][0]:
                errors.append(f"entry {i} path order")
                break
            if offset < PACK_HEADER_SIZE or offset + size > pack_size:
                errors.append(f"entry {i} payload bounds")
                break
            entries.append((path, offset, size))

    if not errors:
        actual_index_checksum = fnv1a(raw_entries)
        actual_index_checksum = fnv1a(paths, actual_index_checksum)
        if actual_index_checksum != index_checksum:
            errors.append("index checksum")

    if not errors and check_data:
        actual = FNV_OFFSET
        total = sum(size for _path, _offset, size in entries)
        done = 0
        progress._last = -1
        with open(pack_path, "rb") as f:
            for _path, offset, size in entries:
                f.seek(offset)
                remaining = size
                while remaining:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        errors.append("short payload read")
                        break
                    actual = fnv1a(chunk, actual)
                    remaining -= len(chunk)
                    done += len(chunk)
                    progress("verify", done, total)
                if errors:
                    break
        print()
        if actual != data_checksum:
            errors.append(f"payload checksum 0x{actual:016x} != 0x{data_checksum:016x}")

    if errors:
        print("VERIFY FAILED:")
        for error in errors:
            print(f"  - {error}")
        raise SystemExit(1)
    print(f"  verified {count:,} entries, {pack_size:,} bytes")
    return True


def main():
    parser = argparse.ArgumentParser(description="Build VectorNX's asset pack.")
    parser.add_argument("--assets", default=os.path.join("stage", "vector", "assets"),
                        help="assets directory (the one containing bin/Data)")
    parser.add_argument("--out", default=os.path.join("stage", "vector"),
                        help="directory for assets.nxpack / assets.nxidx")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the post-build payload verification")
    parser.add_argument("--verify-only", action="store_true",
                        help="verify an existing pack without rebuilding")
    args = parser.parse_args()

    assets_root = find_assets_root(args.assets)
    out_root = os.path.abspath(args.out)
    if args.verify_only:
        verify_pack(os.path.join(out_root, "assets.nxpack"),
                    os.path.join(out_root, "assets.nxidx"), check_data=True)
        return
    build_pack(assets_root, out_root, verify=not args.no_verify)


if __name__ == "__main__":
    main()
