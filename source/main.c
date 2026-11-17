/* main.c -- Vector (Nekki) Nintendo Switch wrapper entry point.
 *
 * Unity 2022.3.62f2 (7670c08855a9) / IL2CPP, arm64-v8a.
 *
 * Loads libmain + libunity + libil2cpp, resolves their imports against native
 * Switch implementations, and drives the lifecycle the Java UnityPlayer normally
 * runs:
 *
 *   JNI_OnLoad -> initJni -> recreate GFX -> surface changed -> resume/focus
 *   -> nativeRender loop -> nativeApplicationUnload -> nativeDone
 *
 * The UnityPlayer natives are recovered BY NAME from libunity's RegisterNatives
 * table (jni_fake.c: jni_lookup_unity_native), so no fragile per-function offsets
 * are needed for the lifecycle. The offsets in vector_entrypoints.h are a
 * cross-check and are used by the patch/diagnostic code.
 *
 * Forked from the MIT-licensed SoLoader lineage via badpiggies_nx
 * (colorsheep_nx / laytonbmr_nx / vln_nx). The memory/heap/overcommit
 * scaffolding is inherited; Vector-specific deltas are the identity/config,
 * the license patch, and the eventual FMOD/OpenSL and input modules.
 *
 * This project contains no game code and no game assets.
 */
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "libc_shim.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "vector_entrypoints.h"
#include "unity_input_hook.h"
#include "vector_patches.h"
#include "nx_fonts.h"
#include "diag.h"

#define DATA_ROOT  GAME_HOME
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* unity_glue.c */
void unity_environment_init(const char *data_root);

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* Strong override of nx_crash_handler.c's weak stub: name addresses that fall
 * inside our loaded .so images (creport can't, since they aren't real modules). */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap, uintptr_t *base_out) {
  const struct { const char *n; so_module *m; } mods[] = {
    { "libmain.so", &main_mod }, { "libunity.so", &unity_mod }, { "libil2cpp.so", &il2cpp_mod },
  };
  for (unsigned i = 0; i < sizeof(mods)/sizeof(*mods); i++) {
    uintptr_t b = (uintptr_t)mods[i].m->load_virtbase;
    if (b && addr >= b && addr < b + mods[i].m->load_size) {
      snprintf(name_out, name_cap, "%s", mods[i].n);
      *base_out = b;
      return 1;
    }
  }
  return 0;
}

extern uintptr_t g_il2cpp_base;       /* libc_shim.c: GC stop-the-world bridge */
extern size_t    g_il2cpp_size;       /* libc_shim.c: bounds guard for the same */
extern void      nx_slow_open_dump(void);
extern uint64_t  nx_stat_read_ticks, nx_stat_read_bytes;
extern void nx_sd_flush(void);        /* libc_shim.c: periodic SD commit       */

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

/* libunity ~18M + libil2cpp ~61M + relocated segments/BSS. */
#define SO_REGION_BYTES (320u * 1024 * 1024)

/* ==========================================================================
 * Inherited memory/heap/overcommit scaffolding (MIT, via badpiggies_nx).
 * ========================================================================== */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* ==========================================================================
 * Vector data layout and module load.
 * ========================================================================== */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/level0",
    "assets/bin/Data/level1",
    "assets/bin/Data/level2",
    "assets/bin/Data/sharedassets0.assets",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/boot.config",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see README.md).\n"
                  "Run tools/stage_sd.py to assemble it from your own APK.", files[i]);
  }
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* engine entry points (resolved by name from RegisterNatives, post-JNI_OnLoad) */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;
static fn_inject   Unity_nativeInjectEvent;   /* resolved for the input phase */

static AppletHookCookie g_applet_cookie;
static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnExitRequest)
    nx_sd_flush();
}

/* The engine's native UnityChoreographer thread never starts on this port (its
 * constructor start event is not signalled), but the frame limiter at
 * libunity+0x5E1588 waits for the counter it is supposed to advance. Pulse the
 * same counter the thread's tick (0x60B9BC) would update, using the loader's
 * pthread shims exactly like the working Phigros frame-pacing pulse:
 *
 *   mutex  libunity+0x104B190
 *   cond   libunity+0x104B1B8
 *   count  libunity+0x104B1E8
 *   stamp  libunity+0x104B1F0 / previous 0x104B1F8
 */
extern int pthread_mutex_lock_fake(pthread_mutex_t **uid);
extern int pthread_mutex_unlock_fake(pthread_mutex_t **uid);
extern int pthread_cond_broadcast_fake(pthread_cond_t **cnd);

static void nx_choreo_tick(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  pthread_mutex_t **mtx  = (pthread_mutex_t **)(ub + 0x104B190);
  pthread_cond_t  **cond = (pthread_cond_t  **)(ub + 0x104B1B8);
  volatile uint64_t *counter = (volatile uint64_t *)(ub + 0x104B1E8);
  volatile uint64_t *stamp   = (volatile uint64_t *)(ub + 0x104B1F0);
  volatile uint64_t *prev    = (volatile uint64_t *)(ub + 0x104B1F8);
  if (pthread_mutex_lock_fake(mtx) != 0) return;
  *prev = *stamp;
  *stamp = now;
  ++*counter;
  pthread_cond_broadcast_fake(cond);
  pthread_mutex_unlock_fake(mtx);
  static int logged;
  if (!logged) {
    logged = 1;
    debugPrintf("[vsync] native frame counter pulse armed (count=%llu)\n",
                (unsigned long long)*counter);
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  cpu_boost(1);   /* FastLoad: scene/asset parsing is CPU-heavy during boot */
  debugPrintf("[boot] === vector_nx %s start (Unity 2022.3.62f2) ===\n", VECTOR_VERSION);

  /* CWD fix: title-override leaves cwd at the .nro folder or SD root; Unity reads
   * many files via relative paths, so chdir into DATA_ROOT. */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_data = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    debugPrintf("[boot] cwd '%s' -> chdir(%s)=%d; reachable(rel): metadata=%d ggm=%d\n",
                cwd, DATA_ROOT, rc, reach_meta, reach_data);
  }

  /* config.txt -- created with defaults on first run. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    if (read_config(cfg) != 0) write_config(cfg);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem: newlib=%u MB, mmap arena=%u MB @ %p (align=%u MB)\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base,
                (unsigned)(MMAP_ARENA_ALIGN >> 20));
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20), (unsigned)((tot - used) >> 20));
  }

  /* Stack-region overcommit arena. Any failure -> heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool);
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole -> heap-backed only\n");
    }
  }

  /* Vector is landscape. The internal target is configurable (default 960x540)
   * and the compositor upscales it to the panel. Touch/HID scaling is derived
   * from these values, so a resolution change stays consistent. */
  screen_width  = (config.render_width  >= 640) ? config.render_width  : VEC_FORCE_SCREEN_W;
  screen_height = (config.render_height >= 360) ? config.render_height : VEC_FORCE_SCREEN_H;
  debugPrintf("[gfx] boot mode=%s render=%dx%d (config)\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  /* Sweep Unity's case-sensitivity probe files, plus the hidden scratch the
   * probe is redirected to. Prevents junk accumulating on the SD card. */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, DATA_ROOT "/%s", de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  debugPrintf("[boot] ===== vector_nx %s build %s %s =====\n", VECTOR_VERSION, __DATE__, __TIME__);

  /* Unity's dynamic-font fallback discovers Android font paths; nx_fonts
   * synthesizes them from the console's shared fonts. Must run before load. */
  nx_fonts_init(GAME_HOME);

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_size = il2cpp_mod.load_size;
  debugPrintf("[boot] libmain=%p libunity=%p libil2cpp=%p\n",
              main_mod.load_virtbase, unity_mod.load_virtbase, il2cpp_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Vector-specific IL2CPP patches (license gate now; audio/input later). */
  void *il2cpp_string_new = (void *)so_try_find_addr_rx(&il2cpp_mod,
                                                         "il2cpp_string_new");
  debugPrintf("[boot] il2cpp_string_new=%p\\n", il2cpp_string_new);
  vector_install_patches((uintptr_t)unity_mod.load_virtbase,
                         (uintptr_t)il2cpp_mod.load_virtbase,
                         il2cpp_string_new);

  /* Main thread runs init_array + the engine lifecycle; give it a stable bionic TLS. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST (exported symbol): it runs
   * jni::Initialize() (caches the JavaVM) AND registers the UnityPlayer natives,
   * which our fake RegisterNatives captures for by-name resolution below. */
  {
    fn_jnionload Unity_JNI_OnLoad =
      (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!Unity_JNI_OnLoad) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] calling libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime WITHOUT calling its
   * JNI_OnLoad: the entry logs through __android_log_print first, whose GOT
   * slot is unsafe in this loader. Replicate the two stores the function makes
   * (see vector_entrypoints.h). */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + VEC_IL2CPP_VM_GLOBAL)    = fake_vm;
    *(void **)(b + VEC_IL2CPP_HANDLER_SLOT) = (void *)(b + VEC_IL2CPP_HANDLER_FN);
    debugPrintf("[boot] il2cpp VM global set (vm=%p, handler=%p); JNI_OnLoad skipped\n",
                fake_vm, (void *)(b + VEC_IL2CPP_HANDLER_FN));
  }

  /* Resolve the UnityPlayer natives BY NAME from the captured RegisterNatives table. */
  #define RESOLVE_NATIVE(var, cast, name) do {                         \
      var = (cast)jni_lookup_unity_native(name);                       \
      if (!(var)) fatal_error("UnityPlayer native not captured: %s", name); \
    } while (0)
  RESOLVE_NATIVE(Unity_initJni,                  fn_initJni,  VEC_NATIVE_initJni);
  RESOLVE_NATIVE(Unity_nativeRecreateGfxState,   fn_gfxstate, VEC_NATIVE_nativeRecreateGfxState);
  RESOLVE_NATIVE(Unity_nativeSendSurfaceChanged, fn_v,        VEC_NATIVE_nativeSendSurfaceChanged);
  RESOLVE_NATIVE(Unity_nativeRender,             fn_z,        VEC_NATIVE_nativeRender);
  RESOLVE_NATIVE(Unity_nativeResume,             fn_v,        VEC_NATIVE_nativeResume);
  RESOLVE_NATIVE(Unity_nativeFocusChanged,       fn_vz,       VEC_NATIVE_nativeFocusChanged);
  RESOLVE_NATIVE(Unity_nativeDone,               fn_z,        VEC_NATIVE_nativeDone);
  RESOLVE_NATIVE(Unity_nativeApplicationUnload,  fn_v,        VEC_NATIVE_nativeApplicationUnload);
  Unity_nativeInjectEvent = (fn_inject)jni_lookup_unity_native(VEC_NATIVE_nativeInjectEvent);
  #undef RESOLVE_NATIVE
  debugPrintf("[boot] natives resolved (initJni=%p render=%p inject=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender,
              (void *)Unity_nativeInjectEvent);

  install_bionic_tls(main_tls);

  /* Vector's initJni is 3-ARG (env,thiz,Context), same form as Unity 2020.3/2022.3. */
  debugPrintf("[boot] initJni(ctx)...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface changed; resume + focus\n");

  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true\n");

  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* Disable the Boehm GC before the first nativeRender. First-frame managed
   * allocations can trigger a stop-the-world whose POSIX signals Switch never
   * delivers -> nativeRender would never return. */
  {
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) { il2cpp_gc_set_mode(1); debugPrintf("[boot] il2cpp_gc_set_mode(DISABLED)\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
    if (il2cpp_gc_disable)  { il2cpp_gc_disable();   debugPrintf("[boot] il2cpp_gc_disable() -> GC OFF\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found\n");
  }

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");
  diag_watchdog_start();
  debugPrintf("[boot] GC off; entering render loop\n");

  /* The engine's native UnityChoreographer thread never starts on this port
   * (its constructor start event is not signalled), but the frame limiter at
   * libunity+0x5E1588 waits for the counter it is supposed to advance.
   * nx_choreo_tick() drives the same tick from the main loop. */

  int frame = 0;
  uint64_t next_frame_ns = 0;
  {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    next_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid(Unity_nativeInjectEvent, fake_env, fake_unityplayer_thiz);

    /* Time the engine's frame. A >20ms hitch prints what happened during it. */
    struct timespec _t0, _t1; clock_gettime(CLOCK_MONOTONIC, &_t0);
    unsigned _o0 = nx_stat_open, _c0 = nx_stat_commit, _m0 = nx_stat_mmap, _g0 = nx_stat_gcstop;
    const uint64_t _rt0 = nx_stat_read_ticks, _rb0 = nx_stat_read_bytes;

    nx_choreo_tick();

    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;

    clock_gettime(CLOCK_MONOTONIC, &_t1);
    {
      long _ms = (long)((_t1.tv_sec - _t0.tv_sec) * 1000 + (_t1.tv_nsec - _t0.tv_nsec) / 1000000);
      const uint64_t _ns = (uint64_t)(_t1.tv_sec - _t0.tv_sec) * 1000000000ull +
                           (uint64_t)(_t1.tv_nsec - _t0.tv_nsec);
      static uint64_t perf_sum_ns;
      static int perf_n, perf_over16;
      static long perf_max;
      perf_sum_ns += _ns; perf_n++;
      if (_ms > perf_max) perf_max = _ms;
      if (_ms > 17) perf_over16++;
      if (_ms >= 20) {
        const unsigned long long _rdms = (unsigned long long)((nx_stat_read_ticks - _rt0) / 19200);
        const unsigned long long _rdkb = (unsigned long long)((nx_stat_read_bytes - _rb0) / 1024);
        debugPrintf("[perf] frame %d took %ldms  open=%u sd=%u mmap=%u gc=%u  read=%llums/%lluKB  touch=%d\n",
                    frame, _ms,
                    nx_stat_open - _o0, nx_stat_commit - _c0,
                    nx_stat_mmap - _m0, nx_stat_gcstop - _g0,
                    _rdms, _rdkb, nx_hook_touch_count());
        if (_ms >= 50) nx_slow_open_dump();
      }
      if (perf_n >= 300) {
        const double avg = (double)perf_sum_ns / (double)perf_n / 1000000.0;
        debugPrintf("[fps] %d frames: avg %.1fms (%.1f fps), max %ldms, >16ms %d\n",
                    perf_n, avg, avg > 0.0 ? 1000.0 / avg : 0.0, perf_max, perf_over16);
        perf_sum_ns = 0; perf_n = 0; perf_max = 0; perf_over16 = 0;
      }
    }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
    if (frame == 1800) {
      /* Keep FastLoad for the whole session: the port is CPU/I-O bound and the
       * frame-rate target benefits more than the small thermal cost hurts. */
      debugPrintf("[boot] CPU boost stays on (performance mode)\n");
    }
    if ((frame % 120) == 0) nx_sd_flush();   /* commit pending save ~every 2s */

    /* ~60fps frame limiter. No real display vsync; pacing avoids flooding vi. */
    next_frame_ns += 16666667ULL;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if ((int64_t)(next_frame_ns - now) > 0) svcSleepThread(next_frame_ns - now);
    else next_frame_ns = now;
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
