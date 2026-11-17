/* unity_input_hook_stub.c -- temporary no-input shim for the Vector bring-up.
 *
 * The Bad Piggies base feeds Unity through il2cpp Input.* hooks. Vector will
 * instead synthesize android.view.MotionEvent/KeyEvent objects and drive
 * nativeInjectEvent (the same path Phigros uses), so the Input.* hook layer is
 * not part of this port. Until that module lands, keep the HID plumbing in
 * android_native_unity.c linkable and report zero touches.
 */
#include "unity_input_hook.h"

void nx_input_hook_update_multi(const NxTouchIn *in, int n) {
  (void)in;
  (void)n;
}

void nx_input_hook_update(int active, float ux, float uy) {
  (void)active;
  (void)ux;
  (void)uy;
}

int nx_hook_touch_count(void) {
  extern int nx_live_touch_count(void);
  return nx_live_touch_count();
}
