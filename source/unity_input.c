/* unity_input.c -- fake MotionEvent / KeyEvent backing nativeInjectEvent.
 * See unity_input.h for the method surface. Plain C, no libnx dependency.
 *
 * The implementation is the same one used by the working Phigros port: each
 * event is a tagged UEvent, the JNI getters are routed by that tag, and the
 * constructors take slots from a ring because the engine keeps an obtain()
 * copy after nativeInjectEvent returns.
 */

#include <string.h>
#include <time.h>
#include "unity_input.h"

struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

enum { UI_TAG = 0x55494531 /*'UIE1'*/, KIND_MOTION, KIND_KEY };

typedef struct {
  uint32_t tag; int kind;
  int   action;                 /* raw action (masked | ptrindex<<8)        */
  int   count;
  int   ids[UI_MAX_POINTERS];
  float xs [UI_MAX_POINTERS];
  float ys [UI_MAX_POINTERS];
  int   keycode;                /* KeyEvent                                 */
  int64_t time_ms;              /* getEventTime: this event                 */
  int64_t down_ms;              /* getDownTime: start of THIS gesture       */
} UEvent;

static UEvent   g_ev_copies[UI_EVENT_COPIES];
static unsigned g_ev_copy_i;

static UEvent *ev_alloc(void){
  const unsigned slot = __atomic_fetch_add(&g_ev_copy_i, 1u, __ATOMIC_RELAXED);
  return &g_ev_copies[slot & UI_EVENT_COPY_MASK];
}

static int64_t now_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* Time of the current gesture's first ACTION_DOWN, for getDownTime(). */
static int64_t g_gesture_down_ms = 0;
static int  has(const char *s,const char *sub){ return strstr(s,sub)!=NULL; }

/* ---- constructors ------------------------------------------------------- */
void *unity_motionevent(int action,int count,const int *ids,const float *xs,const float *ys){
  UEvent *e=ev_alloc(); memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_MOTION; e->action=action; e->time_ms=now_ms();
  if ((action & AMOTION_ACTION_MASK) == AMOTION_ACTION_DOWN)
    g_gesture_down_ms = e->time_ms;
  e->down_ms = g_gesture_down_ms ? g_gesture_down_ms : e->time_ms;
  if (count>UI_MAX_POINTERS) count=UI_MAX_POINTERS;
  e->count=count;
  for (int i=0;i<count;i++){ e->ids[i]=ids?ids[i]:i; e->xs[i]=xs?xs[i]:0; e->ys[i]=ys?ys[i]:0; }
  return e;
}
void *unity_keyevent(int action,int keycode){
  UEvent *e=ev_alloc(); memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_KEY; e->action=action; e->keycode=keycode; e->time_ms=now_ms();
  e->down_ms = e->time_ms;
  return e;
}

/* MotionEvent.obtain(MotionEvent src): Android's copy factory. The engine
 * copies our injected event and reads the copy after inject returns, so this
 * must be a real, separate UEvent. */
void *unity_motionevent_obtain(void *src){
  UEvent *s = src;
  if (!s || s->tag!=UI_TAG) return src;
  UEvent *d = ev_alloc();
  *d = *s;
  return d;
}

/* ---- ownership ---------------------------------------------------------- */
int input_owns_class(const char *cls){
  return has(cls,"view/MotionEvent") || has(cls,"view/KeyEvent") ||
         has(cls,"view/InputEvent");
}
int input_owns_recv(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG;
}
int input_recv_is_motion(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG && e->kind==KIND_MOTION;
}

static int s_a_index = -1;
void input_set_a_index(int idx){ s_a_index = idx; }

static int ptr_index(const struct FakeID *id, va_list va, int ai){
  if (ai >= 0) return ai;
  if (strstr(id->sig,"(I)")) { int idx=va_arg(va,int); return idx; }
  return 0;
}

/* ---- int / long getters ------------------------------------------------- */
uint64_t input_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  const int ai = s_a_index; s_a_index = -1;   /* consume the A-path index once */
  if (!e || e->tag!=UI_TAG) return 0;

  /* shared InputEvent base */
  if (has(m,"getDeviceId")) return 0;
  if (has(m,"getSource"))   return (uint64_t)(e->kind==KIND_MOTION?AINPUT_SOURCE_TOUCHSCREEN:AINPUT_SOURCE_KEYBOARD);
  if (has(m,"getDownTime"))  return (uint64_t)e->down_ms;
  if (has(m,"getEventTime")) return (uint64_t)e->time_ms;
  if (has(m,"getMetaState")) return 0;
  if (has(m,"getFlags"))     return 0;

  if (e->kind==KIND_MOTION){
    if (has(m,"getActionMasked")) return (uint64_t)(e->action & AMOTION_ACTION_MASK);
    if (has(m,"getActionIndex"))  return (uint64_t)((e->action>>AMOTION_ACTION_PTR_IDX_SHIFT)&0xff);
    if (has(m,"getAction"))       return (uint64_t)e->action;
    if (has(m,"getPointerCount")) return (uint64_t)e->count;
    if (has(m,"getPointerId")){ int i=ptr_index(id, va, ai); return (uint64_t)((i>=0&&i<e->count)?e->ids[i]:0); }
    if (has(m,"getToolType"))     return AMOTION_TOOL_TYPE_FINGER;
    if (has(m,"getButtonState"))  return 0;
    if (has(m,"getHistorySize"))  return 0;
    return 0;
  }
  /* KeyEvent */
  if (has(m,"getKeyCode"))     return (uint64_t)e->keycode;
  if (has(m,"getAction"))      return (uint64_t)e->action;
  if (has(m,"getRepeatCount")) return 0;
  if (has(m,"getUnicodeChar")||has(m,"GetUnicodeChar")) return 0;
  return 0;
}

/* ---- float getters ------------------------------------------------------ */
float input_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  const int ai = s_a_index; s_a_index = -1;
  if (!e || e->tag!=UI_TAG || e->kind!=KIND_MOTION) return 0.0f;
  int i = ptr_index(id, va, ai);
  if (i<0 || i>=e->count) i=0;
  if (has(m,"getRawX")||(has(m,"getX"))) return e->count? e->xs[i] : 0.0f;
  if (has(m,"getRawY")||(has(m,"getY"))) return e->count? e->ys[i] : 0.0f;
  if (has(m,"getPressure"))   return 1.0f;
  if (has(m,"getSize"))       return 0.1f;
  if (has(m,"getOrientation"))return 0.0f;
  return 0.0f;
}
