/* unity_input.h -- fake android.view.MotionEvent / KeyEvent for nativeInjectEvent
 *
 * nativeInjectEvent(env, thiz, InputEvent, deviceId) hands the engine a Java
 * input event jobject, which libunity then queries back via JNI. The methods
 * Unity resolves on these objects are:
 *   MotionEvent: getAction getActionMasked getActionIndex getPointerCount
 *                getPointerId getX getY getPressure getSize getOrientation
 *                getEventTime getToolType getButtonState getFlags getHistorySize
 *   KeyEvent:    getAction getKeyCode getRepeatCount getMetaState getUnicodeChar getFlags
 *   InputEvent:  getDeviceId getSource   (shared base; both kinds answer these)
 *
 * The constructors stash the values in a tagged UEvent; the JNI dispatch in
 * jni_fake.c routes getters by that tag (GetObjectClass reports our handle as
 * java/lang/Object, so class-name routing alone would miss them).
 */
#ifndef UNITY_INPUT_H
#define UNITY_INPUT_H

#include <stdarg.h>
#include <stdint.h>
#include "config.h"   /* VEC_TOUCH_SLOTS */

#define UI_MAX_POINTERS 12

/* Injected-event copy ring. MotionEvent.obtain() copies an event and the
 * engine may read that copy after nativeInjectEvent returns, so copies are
 * never recycled immediately. Two ring slots per injected event (our handle +
 * the engine's copy), held for up to 16 frames. */
#define UI_INJECT_PER_FRAME (2 * VEC_TOUCH_SLOTS + 8)
#define UI_SLOTS_PER_EVENT  2
#define UI_EVENT_HOLD_FRAMES 16
#define UI_EVENT_COPIES     1024
#define UI_EVENT_COPY_MASK  (UI_EVENT_COPIES - 1)

#if UI_EVENT_COPIES < (UI_INJECT_PER_FRAME * UI_SLOTS_PER_EVENT * UI_EVENT_HOLD_FRAMES)
#error "UI_EVENT_COPIES too small for VEC_TOUCH_SLOTS."
#endif

/* Android action / source / keycode constants */
#define AMOTION_ACTION_DOWN          0
#define AMOTION_ACTION_UP            1
#define AMOTION_ACTION_MOVE          2
#define AMOTION_ACTION_CANCEL        3
#define AMOTION_ACTION_POINTER_DOWN  5
#define AMOTION_ACTION_POINTER_UP    6
#define AMOTION_ACTION_MASK          0xff
#define AMOTION_ACTION_PTR_IDX_SHIFT 8
#define AINPUT_SOURCE_TOUCHSCREEN    0x1002
#define AINPUT_SOURCE_KEYBOARD       0x0101
#define AMOTION_TOOL_TYPE_FINGER     1
#define AKEY_ACTION_DOWN             0
#define AKEY_ACTION_UP               1
#define AKEYCODE_BACK                4

/* constructors -> opaque jobject. action already encodes the pointer index in
 * its high byte for POINTER_DOWN/UP. */
void *unity_motionevent(int action, int count,
                        const int *ids, const float *xs, const float *ys);
void *unity_keyevent(int action, int keycode);
/* MotionEvent.obtain(src) copy factory -- returns a separate UEvent copy. */
void *unity_motionevent_obtain(void *src);

/* dispatch hooks (called from jni_fake.c's dispatch_int / dispatch_float) */
int       input_owns_class(const char *cls);
int       input_owns_recv (const void *recv);     /* true if recv is our handle */
int       input_recv_is_motion(const void *recv); /* true if MotionEvent       */
uint64_t  input_dispatch_int  (void *recv, const void *id, va_list va);
float     input_dispatch_float(void *recv, const void *id, va_list va);
/* Hand the pointer index to the next input_dispatch_* call when it arrives via
 * the JNI jvalue-array ("A") path, which carries no varargs. -1 clears it. */
void      input_set_a_index(int idx);

#endif /* UNITY_INPUT_H */
