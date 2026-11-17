#!/usr/bin/env python3
"""stage_sd.py -- assemble the Vector SD-card layout from your own APK.

This script does NOT modify the APK and does NOT download anything. It reads the
arm64-v8a libraries and Unity data out of a legally obtained Vector APK and
writes the directory tree the loader expects:

    <out>/
      vector_nx.nro        (only if --nro is supplied)
      libmain.so
      libunity.so
      libil2cpp.so
      assets/bin/Data/...  (all Unity data, split files combined)
      save/                (created empty; the game writes here)

Usage:
    python tools/stage_sd.py Vector-Full-v1.5.0.apk --out E:\\switch\\vector
    python tools/stage_sd.py Vector-Full-v1.5.0.apk --out stage/vector --nro vector_nx.nro
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

# Expected sha256 of the retail Vector 1.5.0 APK used during development.
EXPECTED_SHA256 = "72EB91A77604D53BF896458B1D8EC6927EC1A8ED929DA4DB964C5004A243FFAD"
LIBS = ("libmain.so", "libunity.so", "libil2cpp.so")
REQUIRED = (
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/level0",
    "assets/bin/Data/level1",
    "assets/bin/Data/level2",
    "assets/bin/Data/sharedassets0.assets",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/boot.config",
)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def main():
    ap = argparse.ArgumentParser(description="Assemble the Vector SD-card layout.")
    ap.add_argument("apk", help="path to your legally obtained Vector APK")
    ap.add_argument("--out", default=os.path.join("stage", "vector"),
                    help="output directory (default: stage/vector)")
    ap.add_argument("--nro", default=None,
                    help="optional built vector_nx.nro to copy into the output")
    args = ap.parse_args()

    if not os.path.isfile(args.apk):
        sys.exit(f"APK not found: {args.apk}")

    digest = sha256(args.apk)
    print(f"APK: {args.apk}")
    print(f"SHA-256: {digest}")
    if digest != EXPECTED_SHA256:
        print("WARNING: this is not the 1.5.0 APK the port was derived against.")
        print("         The loader may still work, but offsets/patches are unverified.")

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    with zipfile.ZipFile(args.apk) as z:
        names = set(z.namelist())

        # 1. Native libraries (arm64-v8a only).
        for lib in LIBS:
            src = f"lib/arm64-v8a/{lib}"
            if src not in names:
                sys.exit(f"missing {src} in APK")
            dst = os.path.join(out, lib)
            with z.open(src) as fi, open(dst, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            print(f"  lib   {lib:16} {os.path.getsize(dst):>12,} bytes")

        # 2. Unity data, combining <base>.splitN groups.
        split_groups = {}
        for n in names:
            m = re.match(r"^(.+)\.split(\d+)$", n)
            if m and n.startswith("assets/"):
                split_groups.setdefault(m.group(1), []).append((int(m.group(2)), n))

        data_root = "assets/bin/Data/"
        extracted = 0
        skipped_splits = set()
        for n in sorted(names):
            if not n.startswith(data_root) or n.endswith("/"):
                continue
            m = re.match(r"^(.+)\.split(\d+)$", n)
            if m:
                skipped_splits.add(n)
                continue
            dst = os.path.join(out, n.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(n) as fi, open(dst, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            extracted += 1

        for base, parts in sorted(split_groups.items()):
            parts.sort()
            dst = os.path.join(out, base.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fo:
                for _, part in parts:
                    with z.open(part) as fi:
                        shutil.copyfileobj(fi, fo)
            print(f"  join  {os.path.relpath(dst, out):48} {os.path.getsize(dst):>12,} bytes "
                  f"({len(parts)} parts)")
            extracted += 1

        print(f"  data  {extracted} files under {data_root}")

    # 3. Runtime save directory.
    os.makedirs(os.path.join(out, "save"), exist_ok=True)

    # 4. Optional NRO copy.
    if args.nro:
        if not os.path.isfile(args.nro):
            sys.exit(f"NRO not found: {args.nro}")
        shutil.copy2(args.nro, os.path.join(out, "vector_nx.nro"))
        print(f"  nro   vector_nx.nro    {os.path.getsize(os.path.join(out, 'vector_nx.nro')):>12,} bytes")

    # 5. Verify the required layout.
    missing = [r for r in REQUIRED if not os.path.isfile(os.path.join(out, r.replace("/", os.sep)))]
    if missing:
        sys.exit("missing after staging:\n  " + "\n  ".join(missing))

    print(f"\nOK: SD layout ready at {out}")
    print("Copy that folder to sdmc:/switch/vector/ on the console, then launch")
    print("vector_nx.nro from the homebrew menu (title override recommended).")


if __name__ == "__main__":
    main()
