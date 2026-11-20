# VectorNX

Run **Vector** (Nekki) on a Nintendo Switch.

VectorNX is a homebrew loader that boots the original Android **arm64-v8a**
build of Vector through the Switch's Unity runtime. It contains **no game code
and no extracted game data**; the icon is user-supplied artwork. You must own
Vector and supply your own APK.

> **Legal:** buy Vector on Google Play and keep the APK private. Do not
> redistribute the APK, the extracted `.so` files, or `assets/bin/Data`.
> Nothing in this repository is a replacement for the game.

---

## What you need

- A Nintendo Switch with Atmosphère and hbmenu (title override recommended).
- **Vector Full** — package `com.nekki.vector.paid`, version **1.5.0**
  (build 5000131):
  <https://play.google.com/store/apps/details?id=com.nekki.vector.paid>
- A **patched paid APK** of that game. The stock Play Store APK still contacts
  Google Play Services for license/authentication and may not boot; use a
  patched copy of your own purchased APK with that check removed.
- Use the **single `.apk`** file, not an `.apks` / `.xapk` split bundle: the
  staging script reads `lib/arm64-v8a/*.so` and `assets/bin/Data` from one APK.
- Python 3 (for the staging script).
- A microSD card (FAT32 or exFAT).
- Optional: devkitPro to build the NRO yourself.

> **Legal:** you must own Vector. Patch your own purchased copy; do not
> download or share a pirated APK. Do not redistribute the patched APK, the
> extracted `.so` files, or `assets/bin/Data`.

The loader was derived against the 1.5.0 build 5000131. The staging script
prints the APK hash and warns if it differs from the development copy; that
warning is safe when the APK still contains the same arm64 libraries and Unity
data. The development reference was a patched single APK:

```text
size     156,088,008 bytes
SHA-256  72EB91A77604D53BF896458B1D8EC6927EC1A8ED929DA4DB964C5004A243FFAD
```

A patched APK from another source/patch tool will have a different hash.

---

## Quick start

### 1. Extract the APK

You need `vector_nx.nro` first: download it from the repository's
**Releases** page, or build it yourself (see below).

The staging script reads the APK **without modifying it** and writes only the
files the loader needs:

```powershell
python tools/stage_sd.py <patched-vector.apk> --out stage/vector --nro vector_nx.nro
```

Add `--pack` to also build `assets.nxpack` / `assets.nxidx` (recommended; see
below):

```powershell
python tools/stage_sd.py <patched-vector.apk> --out stage/vector `
    --nro vector_nx.nro --pack
```

Output:

```text
stage/vector/
  vector_nx.nro
  libmain.so
  libunity.so
  libil2cpp.so
  assets/bin/Data/...     (Unity data, loose fallback)
  assets.nxpack           (optional packed assets)
  assets.nxidx
  save/                   (created empty)
```

### 2. Copy to the SD card

Copy the **contents** of `stage/vector/` to:

```text
sdmc:/switch/vector/
```

So the card contains `sdmc:/switch/vector/vector_nx.nro`,
`sdmc:/switch/vector/libunity.so`, `sdmc:/switch/vector/assets/...`, etc.

### 3. Launch

Open hbmenu, hold **R** over any game, and choose `vector_nx`.

### Optional but recommended: packed assets

A loose Unity data tree is hundreds of separate files, and every asset open is
a FAT directory lookup. On the 0.5.0 hardware log the first level load took
**14.0 s with 348 opens**. The pack turns those opens into binary searches in a
path index held in RAM.

If you already staged (or already have the game on the SD card), build the pack
without re-extracting:

```powershell
python tools/build_asset_pack.py `
    --assets "E:\switch\vector\assets" `
    --out    "E:\switch\vector"
```

Replace `E:` with the SD card's drive letter. The script verifies the pack it
wrote; `--verify-only` re-checks an existing pair.

The loader tries the pack first and falls back to the loose tree, so a missing
or invalid pack cannot break the game. Keep the loose tree for now: the boot
data check still expects it. With the pack mounted the boot log says:

```text
[pack] mounted: 1234 entries, 456 MB payload
```

Without it:

```text
[pack] not mounted: No complete asset pack was found (using loose assets)
```

---

## Build it yourself

Requires devkitPro with the Switch toolchain:

```bash
pacman -S switch-dev devkitA64 switch-tools
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib switch-libpng
export DEVKITPRO=/opt/devkitpro

make
```

The NRO is written to `vector_nx.nro`.

The icon is a 256x256 PNG at `icon.png` in the repository root; the build
embeds it and treats it as a dependency. The current NRO already contains the
supplied Vector icon. Make sure you have the right to redistribute the artwork.

---

## Controls

Handheld: use the touchscreen.

Controller:

| Input | Action |
|---|---|
| B / D-pad Up | swipe up |
| Y / D-pad Right | swipe right |
| A / D-pad Down | swipe down |
| X / D-pad Left | swipe left |
| ZL / ZR | tap / confirm |
| Right-stick click | Back |
| Left stick | move the cursor |
| `+` | show / hide the cursor |
| `-` | gyro pointing |
| Left-stick click + D-pad Up/Down | pointer sensitivity |

The swipes are real touch strokes (six frames, ~30% of the screen), so they
work with Vector's normal gesture handling.

---

## Configuration

`sdmc:/switch/vector/config.txt` is created on first run:

```ini
language=auto
render_width=960
render_height=540
framerate=60
log=0
```

- `language`: `auto` or an ISO code (`en`, `ru`, `ja`, `zh_CN`, ...).
- `render_width` / `render_height`: internal render size. The default
  **960x540** keeps the frame rate up; use `1280` / `720` for full quality.
  The Switch compositor upscales to the panel.
- `framerate`: hard-locked at **60**. The key is kept for compatibility; a
  value of `30` is ignored and rewritten to `60`. The window swap interval is
  pinned to 1 and Unity's managed `targetFrameRate` is re-asserted every
  second.
- `log`: `0` for play (no per-frame SD writes), `1` when you need `debug.log`.

---

## Performance notes

This build targets the two causes the hardware logs actually showed:

- **Frame pacing.** The NWindow swap interval is set explicitly to 1 (60 fps) or
  2 (30 fps), and `eglSwapInterval` is clamped to the same value. The old
  free-running 60 Hz sleep loop is gone; two independent 60 Hz clocks beat
  against each other and produce periodic held/repeated frames.
- **Managed frame cap.** Unity's Android player defaults `targetFrameRate` to
  30. The loader hard-locks it to 60 and re-asserts it every second through
  `il2cpp_resolve_icall`, without any libil2cpp offsets.
- **SD commit stalls.** `fsdevCommitDevice("sdmc")` is now performed by a
  low-priority worker instead of on the game thread. Focus-change and exit
  still commit synchronously.
- **Logging.** `log=0` is the default. The crash path still writes a dump.
- **Font fallback.** The console's shared fonts are decoded once and kept in
  RAM; `open()` serves them from a read-only memory fd, so Unity's repeated
  fallback probes do not touch the SD card.
- **Input.** The touchscreen HID ring is drained and replayed oldest-to-newest,
  so a swipe that spans a long frame is not lost or quantised to a single
  sample.
- **Packed assets.** With `assets.nxpack` mounted, asset opens become sorted
  index lookups and reads come from one already-open file. `pread`, `mmap`,
  `stat`, `fstat`, `access`, `opendir` and `readdir` all understand pack
  handles. The loose tree remains the fallback.

Set `log=1` to get `[fps]`/`[perf]` lines. The perf line now includes
`openms=` (total time inside asset opens) and slow SD commits print
`[sd] commit took <ms>`, so a remaining hitch can be attributed instead of
guessed.

---

## Notes

- The intro and ending **cinematics are skipped**. This port has no Android
  MediaExtractor/MediaCodec decoder; the game continues at the title menu.
- The first level load reads a lot from the microSD card. The loose tree uses a
  2 MB read-ahead window, 32 slots and per-file locks so the engine's loader
  threads can overlap SD reads; the asset pack removes the per-file FAT opens
  entirely. A faster card still helps both paths. `debug.log` reports frame
  times, `open=... openms=...`, `read=...ms/...KB` and slow file opens.
- If the screen is black, set `log=1` in `config.txt`, relaunch, and check the
  first line of `sdmc:/switch/vector/debug.log`. It must say
  `vector_nx 0.6.1-perf3`.
- Audio is routed through OpenSL ES. If it is silent, the `[fmod]` lines in
  `debug.log` show which stage failed.

---

## Credits

Built on the MIT-licensed SoLoader lineage (badpiggies_nx, colorsheep_nx,
laytonbmr_nx) and the Phigros/aota Unity ports. Uses devkitPro, libnx, Mesa,
SDL2 and zlib. Vector is © Nekki; this project is not affiliated with Nekki.
