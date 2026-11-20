/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include "nx_pointer.h"
#include "libc_shim.h"   /* GAME_HOME, fopen_fake/fclose_fake (locked file IO) */
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height */
#include "unity_input_hook.h"
#include "unity_input.h"   /* fake MotionEvent/KeyEvent for nativeInjectEvent */

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 720, g_h = 1280;   /* fbstub45 PORTRAIT (stable) */
extern int screen_width, screen_height;   /* the render resolution main.c resolved (config/auto) */

/* One present per display refresh for 60 fps, one every second refresh for 30.
 *
 * The port previously left the NWindow swap interval unset and paced itself
 * with svcSleepThread. That is a free-running 60 Hz timer against a 60 Hz
 * display: the two clocks beat, so even a "60 fps" average periodically shows
 * a held or repeated frame. libnx's default is 1, but switch-mesa rebuilds the
 * swapchain inside eglCreateWindowSurface and does not necessarily preserve it.
 * Every working port in this lineage (lcgo, CloverPit) sets it explicitly and
 * re-asserts it after the surface is created; lcgo's note is unambiguous:
 * unset means the compositor does not throttle at all. */
static u32 nx_swap_interval(void) {
  /* Hard-locked 60: one present per display refresh. There is no 30 fps mode
   * in the shipped build; the config key is kept only so older config.txt files
   * still parse and are rewritten. */
  return 1u;
}

void android_native_update_mode(void){
  /* The NWindow buffer MUST match the resolution Unity renders/reports (screen_width/height),
   * or the game renders at one size into a buffer of another -> zoom/crop. main.c sets those
   * from config.txt or the 1440x2560 default; mirror them here. */
  if (screen_width > 0 && screen_height > 0)                   { g_w = (u32)screen_width; g_h = (u32)screen_height; }
  else if (appletGetOperationMode() == AppletOperationMode_Console) { g_w = 1080; g_h = 1920; }
  else                                                         { g_w = 720;  g_h = 1280; }
}
u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented. */
/* LANDSCAPE, NO ROTATION. Layton renders landscape (confirmed on hardware), so the buffer
 * (bw x bh, 16:9) maps 1:1 to the landscape panel with the identity transform -- no compositor
 * rotation. (The port originally assumed portrait and rotated 90deg, which was the whole
 * "zoomed/tiny" bug.) */
/* Remember the last geometry so we can re-assert it (see nx_window_reassert). */
static u32 g_geom_bw = 1920, g_geom_bh = 1080;

static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  g_geom_bw = bw; g_geom_bh = bh;
  nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0u);   /* identity: landscape buffer -> landscape panel */
  nwindowSetSwapInterval(w, nx_swap_interval());
  static int nlog = 0;
  if (nlog < 6) {
    nlog++;
    u32 aw = 0, ah = 0;
    nwindowGetDimensions(w, &aw, &ah);
    debugPrintf("[gfx] set_geom: render %ux%u, nwindow reports %ux%u, transform=0 (landscape)\n",
                bw, bh, aw, ah);
  }
}

/* switch-mesa creates its swapchain inside eglCreateWindowSurface and may reset the NWindow's
 * crop/dimensions to its own defaults; re-assert our crop (and identity transform) afterwards
 * so only the rendered content is presented. Cheap: two property writes. */
void nx_window_reassert(void) {
  NWindow *w = nwindowGetDefault();
  nwindowSetCrop(w, 0, 0, g_geom_bw, g_geom_bh);
  nwindowSetTransform(w, 0u);
  /* mesa resets the swap interval when it builds/rebuilds the swapchain, so
   * this is re-applied after every eglCreateWindowSurface and for the first
   * few presents. Cheap: one property write. */
  nwindowSetSwapInterval(w, nx_swap_interval());
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  if (width > 0 && height > 0) nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
/* Unity 6's InitializeUILooper() does `if (ALooper_forThread()==NULL) { log
 * "Couldn't retrieve native ALooper for UI thread"; return; }` -- and leaves the
 * global NdkLooper singleton NULL. A later NdkLooper::CreateHandler() then calls
 * WaitForCreation() on that null pointer -> Data Abort (ldrb [null+0xe8]). On real
 * Android the UI thread already owns a Java-created looper; we have none, so return
 * a create-on-demand looper (like ALooper_pollOnce already does) so the NdkLooper
 * is constructed (its ctor sets the [+0xe8] "created" flag WaitForCreation reads). */
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 1); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID polling -> Unity input.
 * Unity ingests input through the Java UnityPlayer (touch -> nativeInjectEvent /
 * key path). The exact native event struct is engine-internal: recover the
 * registered "injectEvent"/"nativePointer*" method from libunity's JNI_OnLoad
 * (PORTING_PLAN.md S4) and fill in feed_one_touch(). Until then this reads HID
 * but doesn't yet hand it to the engine.
 * ========================================================================== */
/* HID -> Unity input. The Switch touchscreen passes straight through (all fingers) into
 * il2cpp UnityEngine.Input via the hooks in unity_input_hook.c.
 *
 * On top of that, the reusable nx_pointer module (nx_pointer.{c,h}) provides the on-screen
 * cursor, USB mouse, gyro pointing and the +/-/stick/A controls, plus its own GL overlay
 * (drawn from the eglSwapBuffers wrapper in imports.c). We feed it a NxpConfig once, pump
 * it each frame, and merge its pointer events into the same multi-touch stream the panel
 * uses -- so a cursor tap is just another finger and rides the same slot allocator. */

static PadState g_pad;                              /* B -> Android Back edge only */

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();

  /* nx_pointer owns the pad/touch/mouse/gyro from here on. It reads cursor.png off the SD
   * card during init, so this must run before the engine spawns its worker threads. The
   * data_dir is the game folder (GAME_HOME); cursor.png and pointer.cfg live alongside the
   * save. We hand it the port's LOCKED file wrappers -- newlib's handle table is not
   * thread-safe and the engine hammers it, so settings I/O must go through fopen_fake/
   * fclose_fake, not raw fopen. The cursor id must stay small: Unity maps an Android
   * pointer id into a fixed-size touch pool, so an id like 1000 is dropped there. Panel
   * fingers take 0..VEC_TOUCH_SLOTS-1; the virtual cursor takes the next slot. */
  NxpConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.screen_w = (screen_width  > 0) ? screen_width  : 1280;
  cfg.screen_h = (screen_height > 0) ? screen_height : 720;
  cfg.panel_w  = 1280;
  cfg.panel_h  = 720;
  cfg.data_dir = GAME_HOME;
  cfg.cursor_id = VEC_TOUCH_SLOTS;                  /* first id above the panel fingers */
  cfg.max_touch_slots = VEC_TOUCH_SLOTS;
  cfg.stick_speed = 0.0f;                           /* 0 => module default (14 px/frame) */
  cfg.mouse_sens  = 0.0f;
  cfg.log = NULL;
  cfg.fopen_fn  = fopen_fake;
  cfg.fclose_fn = fclose_fake;
  nxp_init(&cfg);
}

/* nativeInjectEvent signature: (env, thiz, InputEvent, deviceId) -> bool */
typedef uint8_t (*inject_fn)(void*,void*,void*,int32_t);

/* ---- live pointer set ----------------------------------------------------
 * Android sends the engine the FULL set of pointers currently down with every
 * event; the action word says what happened, and for POINTER_DOWN/UP its high
 * byte says which INDEX in that array changed. So we keep the set ourselves
 * instead of forwarding one event at a time. Panel fingers (ids 0..9) and the
 * nx_pointer virtual cursor (id 10) share this allocator, which is what makes
 * "touch the screen while the cursor is up" behave like real multitouch. */
static int   g_live_id[UI_MAX_POINTERS];
static float g_live_x [UI_MAX_POINTERS];
static float g_live_y [UI_MAX_POINTERS];
static int   g_live_n = 0;
static int   g_live_down_frame[UI_MAX_POINTERS];
static int   g_frame_no = 0;

/* An UP held back so its DOWN gets a frame of its own. */
static struct { int id; float x, y; } g_defer_up[UI_MAX_POINTERS];
static int   g_defer_n = 0;

/* Events carried to the next frame because their pointer already completed a
 * DOWN this frame (two taps inside one frame). */
static NxpEvent g_carry[UI_MAX_POINTERS * 2];
static int      g_carry_n = 0;

static unsigned g_inj_ok, g_inj_rejected;

/* ---- controller swipe gestures -------------------------------------------
 * Vector is touch-only, so the face buttons synthesize real drags:
 *
 *   B = swipe up      Y = swipe right
 *   A = swipe down    X = swipe left
 *
 * A gesture is DOWN + four MOVEs + UP, six frames at 60 Hz, centred on the
 * virtual cursor (or the screen centre when the cursor is hidden). That is a
 * real MotionEvent stroke, not a tap: the game's swipe recogniser sees the
 * finger travel ~30% of the short screen axis in ~100 ms.
 *
 * ZL / ZR remain tap/confirm (menus), and the right-stick click is Back.
 */
#define SWIPE_ID     (UI_MAX_POINTERS - 1)
#define SWIPE_FRAMES 6
typedef struct { int active, dir, frame; float x0, y0, x1, y1; } Swipe;
static Swipe g_swipe;

static void swipe_begin(int dir) {
  if (g_swipe.active) return;
  static int logged_controls;
  if (!logged_controls) {
    logged_controls = 1;
    debugPrintf("[input] controller swipes: B/D-pad Up=up Y/D-pad Right=right "
                "A/D-pad Down=down X/D-pad Left=left; ZL/ZR=tap; "
                "right-stick click=Back; left-stick click+D-pad=sensitivity\n");
  }

  float cx = (float)screen_width  * 0.5f;
  float cy = (float)screen_height * 0.5f;
  if (nxp_cursor_visible()) nxp_cursor_pos(&cx, &cy);

  float d = (screen_width < screen_height ? (float)screen_width : (float)screen_height) * 0.30f;
  if (d < 96.0f) d = 96.0f;
  const float h = d * 0.5f;
  const float margin = h + 2.0f;
  if (cx < margin) cx = margin;
  if (cx > (float)screen_width  - margin) cx = (float)screen_width  - margin;
  if (cy < margin) cy = margin;
  if (cy > (float)screen_height - margin) cy = (float)screen_height - margin;

  float x0 = cx, y0 = cy, x1 = cx, y1 = cy;
  switch (dir) {
    case 0: y0 = cy + h; y1 = cy - h; break;   /* up */
    case 1: x0 = cx - h; x1 = cx + h; break;   /* right */
    case 2: y0 = cy - h; y1 = cy + h; break;   /* down */
    default:x0 = cx + h; x1 = cx - h; break;   /* left */
  }

  g_swipe.active = 1;
  g_swipe.dir = dir;
  g_swipe.frame = 0;
  g_swipe.x0 = x0; g_swipe.y0 = y0;
  g_swipe.x1 = x1; g_swipe.y1 = y1;
}

static void swipe_event(NxpEvent *e) {
  const float t = (float)g_swipe.frame / (float)(SWIPE_FRAMES - 1);
  e->id = SWIPE_ID;
  e->x  = g_swipe.x0 + (g_swipe.x1 - g_swipe.x0) * t;
  e->y  = g_swipe.y0 + (g_swipe.y1 - g_swipe.y0) * t;
  e->phase = (g_swipe.frame == 0) ? NXP_DOWN :
             (g_swipe.frame == SWIPE_FRAMES - 1) ? NXP_UP : NXP_MOVE;
  g_swipe.frame++;
  if (g_swipe.frame >= SWIPE_FRAMES) g_swipe.active = 0;
}

static int live_find(int id){
  for (int i = 0; i < g_live_n; i++)
    if (g_live_id[i] == id) return i;
  return -1;
}

static void live_remove(int idx){
  for (int k = idx + 1; k < g_live_n; k++){
    g_live_id[k-1]         = g_live_id[k];
    g_live_x [k-1]         = g_live_x [k];
    g_live_y [k-1]         = g_live_y [k];
    g_live_down_frame[k-1] = g_live_down_frame[k];
  }
  g_live_n--;
}

static void emit(inject_fn inject, void *env, void *thiz, int action){
  if (g_live_n <= 0) return;
  void *event = unity_motionevent(action, g_live_n, g_live_id, g_live_x, g_live_y);
  if (inject(env, thiz, event, 0)) {
    if (!g_inj_ok)
      debugPrintf("[input] nativeInjectEvent accepted; touch/controller injection live\n");
    g_inj_ok++;
  } else {
    g_inj_rejected++;
    static int warned = 0;
    if (!warned) { warned = 1;
      debugPrintf("[input] nativeInjectEvent REJECTED an event (action=0x%x). "
                  "ok=%u rejected=%u\n", action, g_inj_ok, g_inj_rejected); }
  }
}

int nx_live_touch_count(void) { return g_live_n; }

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  /* nx_pointer reads pad/touch/mouse/gyro and reports events in render space
   * with a TOP-LEFT origin -- exactly the space Android MotionEvents use. Do
   * not flip Y here (the old il2cpp Input hook path used bottom-left). */
  nxp_update();

  /* Controller gesture edges. nx_pointer's own pad is separate; this one is
   * only for the face-button swipes and the Back mapping. */
  padUpdate(&g_pad);
  const u64 pressed = padGetButtonsDown(&g_pad);
  const u64 held_pad = padGetButtons(&g_pad);
  if (!g_swipe.active && !(held_pad & HidNpadButton_StickL)) {
    if      (pressed & (HidNpadButton_B | HidNpadButton_Up))    swipe_begin(0); /* up    */
    else if (pressed & (HidNpadButton_Y | HidNpadButton_Right)) swipe_begin(1); /* right */
    else if (pressed & (HidNpadButton_A | HidNpadButton_Down))  swipe_begin(2); /* down  */
    else if (pressed & (HidNpadButton_X | HidNpadButton_Left))  swipe_begin(3); /* left  */
  }

  NxpEvent ev[UI_MAX_POINTERS * 2];
  int n = 0;
  for (int c = 0; c < g_carry_n; c++) ev[n++] = g_carry[c];
  g_carry_n = 0;
  n += nxp_poll(ev + n, (int)(sizeof ev / sizeof ev[0]) - n);
  if (g_swipe.active && n < (int)(sizeof ev / sizeof ev[0]))
    swipe_event(&ev[n++]);

  int move_pending = 0;

  /* Release last frame's deferred UPs first, so a quick re-tap cannot merge
   * into a drag. */
  for (int d = 0; d < g_defer_n; d++){
    const int idx = live_find(g_defer_up[d].id);
    if (idx < 0) continue;
    g_live_x[idx] = g_defer_up[d].x;
    g_live_y[idx] = g_defer_up[d].y;
    emit(inject, env, thiz,
         (g_live_n == 1)
           ? AMOTION_ACTION_UP
           : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    live_remove(idx);
  }
  g_defer_n = 0;
  g_frame_no++;

  for (int i = 0; i < n; i++){
    const int   id = ev[i].id;
    const float x  = ev[i].x, y = ev[i].y;
    int idx = live_find(id);

    if (ev[i].phase == NXP_MOVE){
      if (idx < 0) continue;                    /* never saw its DOWN */
      g_live_x[idx] = x; g_live_y[idx] = y;
      move_pending = 1;
      continue;
    }

    if (move_pending){
      emit(inject, env, thiz, AMOTION_ACTION_MOVE);
      move_pending = 0;
    }

    if (ev[i].phase == NXP_DOWN){
      /* A DOWN while this pointer's UP is still deferred means two taps
       * inside one frame: carry the second tap to the next frame so the first
       * one is actually visible for a frame. */
      int deferred_here = 0;
      for (int d = 0; d < g_defer_n; d++)
        if (g_defer_up[d].id == id) { deferred_here = 1; break; }
      if (deferred_here){
        if (move_pending){ emit(inject, env, thiz, AMOTION_ACTION_MOVE); move_pending = 0; }
        for (int k = i; k < n && g_carry_n < (int)(sizeof g_carry / sizeof g_carry[0]); k++)
          g_carry[g_carry_n++] = ev[k];
        break;
      }
      if (idx >= 0){                            /* already down -- treat as move */
        g_live_x[idx] = x; g_live_y[idx] = y;
        move_pending = 1;
        continue;
      }
      if (g_live_n >= UI_MAX_POINTERS) continue;
      idx = g_live_n++;
      g_live_id[idx] = id; g_live_x[idx] = x; g_live_y[idx] = y;
      g_live_down_frame[idx] = g_frame_no;
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_DOWN
             : (AMOTION_ACTION_POINTER_DOWN | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    }
    else if (ev[i].phase == NXP_UP){
      if (idx < 0) continue;
      g_live_x[idx] = x; g_live_y[idx] = y;
      /* If this pointer went DOWN in this same frame, hold its UP until the
       * next one: the engine turns injected events into its touch list before
       * the game's next Update(), so a same-frame DOWN+UP is a finger the game
       * never sees alive. */
      if (g_live_down_frame[idx] == g_frame_no && g_defer_n < UI_MAX_POINTERS){
        g_defer_up[g_defer_n].id = id;
        g_defer_up[g_defer_n].x  = x;
        g_defer_up[g_defer_n].y  = y;
        g_defer_n++;
        continue;
      }
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_UP
             : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
      live_remove(idx);
    }
  }

  if (move_pending) emit(inject, env, thiz, AMOTION_ACTION_MOVE);

  /* ---- right-stick click -> Android Back ---- */
  if (pressed & HidNpadButton_StickR) {
    inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
    inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  }
}
