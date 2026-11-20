# VectorNX Setup (Windows launcher)

A small, fast Rust/egui launcher that prepares a Switch SD card from your own
Vector APK. It does **not** launch the game and it does **not** contain or
download game data.

## What it does

1. Reads the APK's Android manifest and checks the package
   (`com.nekki.vector.paid`) and version (`1.5.0`).
2. Shows the APK SHA-256, signature presence, and a best-effort patch status.
   Patched/unverified APKs are allowed after an explicit warning; stock or
   wrong-version APKs are flagged. Patch detection is heuristic (known dev
   hash + Play licensing strings), not a cryptographic verification.
3. Extracts `lib/arm64-v8a/{libmain,libunity,libil2cpp}.so` and
   `assets/bin/Data` (joining `.splitN` groups).
4. **Always** builds and verifies `assets.nxpack` + `assets.nxidx` (the asset
   pack is not optional).
5. Copies `vector_nx.nro` and writes `config.txt`.
6. Never deletes `save/`, `UserData.data`, or `prefs.kv`.

## Build

Requirements: Rust stable (MSVC toolchain) and Visual Studio C++ Build Tools.

```powershell
# From a Developer PowerShell, or use build.bat which finds VsDevCmd via vswhere:
cd launcher
cargo build --release
# or
.\build.bat --release
```

Output: `launcher/target/release/launcher.exe`.

## Release layout

Put these next to the exe:

```text
vectornx-launcher.exe
vector_nx.nro
```

The launcher looks for `vector_nx.nro` beside the exe, in `payload/`, or in the
current directory. You can also browse for it. The NRO is not embedded in the
binary, so releases can update the loader independently.

## Use

1. Select your legally obtained Vector 1.5.0 APK.
2. Select the SD card root (e.g. `E:\`) or an existing `switch\vector` folder.
3. Choose language, render size, logging, and whether to replace existing game
   data.
4. Install. The progress panel shows extraction, packing and verification.
5. Launch `vector_nx.nro` from hbmenu on the console.

The pack is mandatory and verified by a second read pass. If an existing
`assets/bin/Data` tree is present and "Replace existing game data" is on, it is
replaced; save files outside that tree are left alone.

## CLI (for testing / automation)

```powershell
launcher.exe --check <apk>
launcher.exe --install --apk <apk> --out <sd-folder> --nro <nro> [--lang en] [--width 960] [--height 540] [--log 1] [--replace 1]
```

## Notes

- 60 fps is hard-locked in the loader; the launcher writes `framerate=60`.
- `Performance` is 960x540; `Quality` is 1280x720. The compositor upscales.
- The asset pack is stored on the SD card next to the loose `assets` tree. The
  loose tree is kept because the loader's boot data check still expects it.
- No APK, game libraries, assets, or pack files are committed to this
  repository.

## Artwork

`assets/vector_bg.jpg` is the Vector key art supplied by the project owner.
`assets/icon.png` is the project's user-supplied Vector icon. The launcher
shows the art behind a single dark panel and uses only one accent colour
(Vector blue) plus amber/red for warnings and errors.
