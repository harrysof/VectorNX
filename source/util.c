/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

// File-only logger (DEBUG_LOG builds only): open once + flush per line so the tail survives a
// crash, mutex-serialised across engine threads. Drops the high-frequency dlsym/dlopen/JNI spam.
/* Runtime gate: config.txt `log=0` turns the file off after read_config().
 * It starts at 1 so the boot lines (including the config read itself) are
 * durable if something fails before the setting is known. This is the switch
 * that removes per-frame SD writes from a play session; the compile-time
 * DEBUG_LOG still has to be 1 for the code to exist. */
int g_debug_log = 1;

#if DEBUG_LOG
static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
#if !VECTOR_LOG_IO
  if (!strncmp(t, "[io]", 4)) return 1;
#endif
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
}
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  static FILE *f = NULL;
  static int pending;
  va_list list;
  /* log=0 silences the play build, but never the two lines that matter when
   * something has gone wrong: the exception dump and the watchdog's stall
   * report. Both are rare, so they do not reintroduce per-frame SD traffic. */
  if (!g_debug_log &&
      strncmp(text, "[crash]", 7) &&
      strncmp(text, "[wd]", 4)) return 0;
  if (log_is_noisy(text)) return 0;
  mutexLock(&g_log_lock);
  if (!f) f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text); vfprintf(f, text, list); va_end(list);
    /* Batch ordinary lines: 493 per-open [io] lines were each a full SD
     * flush. Crash lines still flush immediately so the dump is durable. */
    pending++;
    if (pending >= 8 || !strncmp(text, "[crash]", 7)) {
      fflush(f);
      pending = 0;
    }
  }
  mutexUnlock(&g_log_lock);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
