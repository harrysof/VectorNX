/* config.h -- Vector (Nekki) Nintendo Switch wrapper configuration.
 *
 * Forked from the MIT-licensed SoLoader lineage via badpiggies_nx
 * (colorsheep_nx / laytonbmr_nx / vln_nx -> badpiggies_nx).
 * The loader-tuning constants (heap split, mmap arena, overcommit window) are
 * engine-generation properties and are inherited. Only the game identity and
 * path constants below are Vector-specific.
 *
 * Target: Vector 1.5.0 (com.nekki.vector.paid), Unity 2022.3.62f2 / IL2CPP /
 * arm64-v8a. No game code or assets are shipped with this wrapper.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Newlib heap for the engine/libc++/il2cpp managed heaps; the rest -> .so loader. */
#define MEMORY_MB 768

/* mmap arena. Unity reserves aligned pools by over-mmapping then trimming
 * head/tail; we back anon mmaps from an aligned arena with a per-page bitmap so
 * sub-range munmap frees only trimmed pages. The engine is patched to 64 MB
 * regions by vector_patches.c, so the arena MUST also be 64 MB-aligned. */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)896 * 1024 * 1024)

/* Stack-region overcommit arena (libc_shim.c). Engine-generic; inherited. */
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 512 * 1024 * 1024)
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* --- inherited SoLoader leftovers (unused by Unity, kept for base parity) --- */
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

/* --- Vector game identity (JNI Context shim: getPackageName/versionCode) --- */
#define CS_PACKAGE       "com.nekki.vector.paid"
#define CS_VERSION_NAME  "1.5.0"
#define CS_VERSION_CODE  5000131
#define CS_APP_GUID      "00000000-0000-0000-0000-000000000000"
/* Android Settings.Secure.ANDROID_ID is normally a stable 16-hex-character
 * value. Vector derives a fixed-length encryption byte array from it. */
#define CS_ANDROID_ID    "9f6a13c8d4e75b20"

#define CONFIG_NAME "config.txt"
#define VECTOR_VERSION "0.6.0-perf2"
#define LOG_NAME    "sdmc:/switch/vector/debug.log"

/* Game data root == the .nro's own folder (SoLoader SD convention):
 * nro + libs + assets all live in sdmc:/switch/vector/. */
#define GAME_HOME   "sdmc:/switch/vector"

/* on-hardware file logging (debug.log). Set to 0 for release builds. */
#define DEBUG_LOG 1

/* Per-open/per-read [io] lines are useful for bring-up but dominate load time:
 * debugPrintf flushes every line to the SD card. Keep them off for playable
 * builds; flip to 1 when diagnosing a file-path problem. Errors and state
 * changes ([mmap], [oc], [patch], [boot], [perf]) are always kept. */
#define VECTOR_LOG_IO 0

/* FORCED render resolution -- applied in main.c in both docked and handheld mode.
 * The default is 960x540 (75% of the panel) so the GPU keeps a stable frame rate;
 * the compositor upscales to the 1280x720 panel. config.txt can override it with
 * render_width / render_height (e.g. 1280 / 720 for full quality).
 * Touch/pointer coordinates scale from this automatically. */
#define VEC_FORCE_SCREEN_W 960
#define VEC_FORCE_SCREEN_H 540

/* Maximum simultaneous injected touch pointers. The Switch panel reports up to
 * 10; the engine's TouchController is finger-indexed. */
#define VEC_TOUCH_SLOTS 10

/* ---- Unity job-worker caps ------------------------------------------------
 * The Switch build gets three CPU cores. Unity 2022.3 sizes its JobSystem from
 * the device, and on this engine generation that has been measured at NINE
 * background workers plus two job workers -- far more runnable threads than
 * three cores can schedule without introducing frame-time jitter. The lcgo
 * port found this on hardware (15 engine threads, PlatformThread::Create
 * waiting forever) and caps it from boot.config. We read the real boot.config
 * now (see unity_jni.c's Scanner), and append these two keys; the engine's
 * parser takes the LAST occurrence, so the user's own file stays untouched.
 * Set VEC_BOOT_WORKER_CAP to 0 to leave the game's values exactly as shipped. */
#define VEC_BOOT_WORKER_CAP 1
#define VEC_BOOT_BG_WORKERS 3
#define VEC_BOOT_JOB_WORKERS 2

/* ---- performance rollback switches ---------------------------------------
 * Each change is independently reversible so a hardware regression can be
 * isolated in one rebuild instead of backing out the whole patch. */
#define VEC_ASYNC_COMMIT    1   /* 0 = fsdevCommitDevice on the calling thread */
#define VEC_FONT_RAM_CACHE  1   /* 0 = open the cached .fonts files from SD   */
#define VEC_FORCE_FPS       1   /* 0 = leave Unity's managed frame cap alone   */
#define VEC_ASSET_PACK      1   /* 0 = ignore assets.nxpack, use loose assets  */

/* Boehm GC stop-the-world bridge. Vector disables automatic GC, but Unity still
 * runs an explicit collection at the end of the loading scene (the 0.2.3 log
 * hangs in GC_suspend_all at libil2cpp+0x181026c, retrying with 3s sleeps
 * because Switch never delivers POSIX signals). The bridge posts the ack that
 * the suspend handler would post. Offsets are derived from THIS libil2cpp
 * (Unity 2022.3.62f2) in libc_shim.c. */
#define VECTOR_GC_BRIDGE 1

extern int screen_width;
extern int screen_height;

typedef struct {
  char language[16];    /* "auto" => follow Switch system language, else ISO code */
  int  render_width;    /* internal render size; compositor upscales to the panel */
  int  render_height;
  int  framerate;       /* 60 or 30; drives swap interval + targetFrameRate */
  int  log;             /* 1 = write debug.log (slow); 0 = play build       */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
