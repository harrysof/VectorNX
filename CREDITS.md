# Credits

This port is a wrapper/loader. It contains **no Vector game code and no game
assets**. The user supplies the libraries and Unity data from their own legally
obtained APK.

## Loader lineage (MIT)

- **badpiggies_nx** — the direct base for this fork: `so_util`, `libc_shim`,
  `imports`, `jni_fake`, `opensles`, the memory/heap/overcommit scaffolding,
  the `android_native_unity` NDK surface, and the lifecycle skeleton.
  https://github.com/ChanseyIsTheBest/badpiggies_nx
- **phigros_nx** — the Unity 2022.3.62f2 (exact revision) reference for the
  engine boot sequence, RegisterNatives capture, and native MotionEvent
  injection design. https://github.com/ChanseyIsTheBest/phigros_nx
- **VNX-Unity-Runtime** — independent documentation of the Unity native boot
  chain (`libmain` → `libunity` → `libil2cpp`). https://github.com/Viridite/VNX-Unity-Runtime
- **colorsheep_nx**, **laytonbmr_nx**, **vln_nx** — the SoLoader/cursor lineage
  credited by badpiggies_nx.
- **TheOfficialFloW**, **fgsfds**, **Andy Nguyen** — the original so-loader
  technique and the Switch/Vita ports that established it.

## Toolchain / libraries

- **devkitPro / devkitA64 / libnx** — NRO build and Horizon APIs.
- **Mesa** (Switch port), **libdrm_nouveau**, **SDL2** — GLES3/EGL and audio/HID.
- **libpng**, **zlib**.
- **Il2CppDumper** (Perfare) — managed metadata recovery.
- **jadx** (skylot) — Java layer analysis.
- **UnityPy** — Unity serialized-data inspection.
- **capstone** — AArch64 disassembly used to verify offsets and derive patches.

## Legal

Do not redistribute `libmain.so`, `libunity.so`, `libil2cpp.so`,
`global-metadata.dat`, the APK, or any extracted textures/audio/video. The NRO
is a loader only. It does not include a patched APK and does not forge
purchases; it expects a patched paid APK supplied by the user and applies only
the runtime patches needed for offline play.
