/* vector_entrypoints.h -- UnityPlayer native method surface for Vector.
 *
 * Source build: Vector 1.5.0 (com.nekki.vector.paid), Unity 2022.3.62f2
 * (7670c08855a9), arm64-v8a / IL2CPP.
 *
 * The 49 JNINativeMethod records were extracted from THIS libunity.so by
 * walking the R_AARCH64_RELATIVE relocation map and scanning the writable
 * tables for {name, signature, fnPtr} triples:
 *   python workspace/analysis/extract_jni_methods.py \
 *          workspace/native/lib/arm64-v8a/libunity.so
 * Full machine-readable table: workspace/analysis/unityplayer_native_methods.tsv
 *
 * DRIVING MODEL
 * -------------
 * libunity exports JNI_OnLoad but keeps the UnityPlayer natives file-local.
 * Their only public exposure is the RegisterNatives call in JNI_OnLoad, which
 * the fake JNIEnv captures. main.c therefore resolves them BY NAME after
 * JNI_OnLoad; the offsets below are a cross-check and diagnostic aid, not the
 * primary source of truth. If the game updates, re-run the extractor.
 *
 * The offsets are link-time virtual addresses for this exact libunity.so.
 * Runtime address = unity_mod.load_virtbase + offset.
 *
 * Signature notes:
 *   initJni is (Context) -> the native receives (env, thiz, Context), 3 args.
 *   nativeInjectEvent is (InputEvent,I) -> (env, thiz, event, deviceId), 4 args.
 *   nativeRecreateGfxState is (I,Surface) -> (env, thiz, int, Surface), 4 args.
 */
#ifndef VECTOR_ENTRYPOINTS_H
#define VECTOR_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- exported JNI entry point (from .dynsym) ---------------------------- */
#define VEC_OFF_JNI_OnLoad                    0x614E34u

/* ---- drive-critical natives (verified against the extractor) ------------ */
#define VEC_OFF_initJni                       0x614030u /* 3-arg thunk -> 0x5FCAF4 */
#define VEC_OFF_nativeDone                    0x61403Cu
#define VEC_OFF_nativePause                   0x6140CCu
#define VEC_OFF_nativeResume                  0x614130u
#define VEC_OFF_nativeLowMemory               0x614178u
#define VEC_OFF_nativeApplicationUnload       0x6141C0u
#define VEC_OFF_nativeFocusChanged            0x614210u
#define VEC_OFF_nativeRecreateGfxState        0x614264u
#define VEC_OFF_nativeSendSurfaceChangedEvent 0x6142CCu
#define VEC_OFF_nativeRender                  0x614324u
#define VEC_OFF_nativeInjectEvent             0x614384u

/* ---- messaging / input / misc ------------------------------------------- */
#define VEC_OFF_nativeSetInputArea            0x614678u
#define VEC_OFF_nativeSetKeyboardIsVisible    0x6146F8u
#define VEC_OFF_nativeSetInputString          0x614750u
#define VEC_OFF_nativeSetInputSelection       0x6147F0u
#define VEC_OFF_nativeSoftInputCanceled       0x614858u
#define VEC_OFF_nativeSoftInputLostFocus      0x6148A8u
#define VEC_OFF_nativeReportKeyboardConfigChanged 0x6148F8u
#define VEC_OFF_nativeSoftInputClosed         0x614940u
#define VEC_OFF_nativeUnitySendMessage        0x614990u
#define VEC_OFF_nativeIsAutorotationOn        0x614B40u
#define VEC_OFF_nativeMuteMasterAudio         0x614BA0u
#define VEC_OFF_nativeSetLaunchURL            0x614BFCu
#define VEC_OFF_nativeHidePreservedContent    0x614D34u
#define VEC_OFF_nativeOrientationChanged      0x614D7Cu
#define VEC_OFF_nativeGetNoWindowMode         0x614DDCu

/* ---- libil2cpp VM registration (do NOT call JNI_OnLoad) -----------------
 * Vector's libil2cpp JNI_OnLoad @0x1790DD0 starts with
 *   __android_log_print(4, "IL2CPP", "JNI_OnLoad")
 * whose GOT slot can mis-bind in a fake-Android loader (instruction abort).
 * Its only essential effects are two stores:
 *   0x1790DFC  str x19, [base + 0x382BDF8]   ; cache JavaVM
 *   0x1790DF8  x0 = base + 0x1790E14         ; JNI handler fn
 *   0x1790E00  bl  0x17901A0                 ; str x0, [base + 0x382BD78]
 * Both targets are in libil2cpp's .bss. Replicate the stores; skip the call. */
#define VEC_IL2CPP_VM_GLOBAL     0x382BDF8u
#define VEC_IL2CPP_HANDLER_SLOT  0x382BD78u
#define VEC_IL2CPP_HANDLER_FN    0x1790E14u

/* ---- engine entry-point signatures (JNI calling convention) -------------- */
typedef void     (*fn_initJni)(void*, void*, void*);            /* env,thiz,Context      */
typedef void     (*fn_gfxstate)(void*, void*, int32_t, void*);  /* env,thiz,int,Surface  */
typedef void     (*fn_v)(void*, void*);                         /* env,thiz -> void       */
typedef uint8_t  (*fn_z)(void*, void*);                         /* env,thiz -> bool       */
typedef void     (*fn_vz)(void*, void*, int32_t);               /* env,thiz,int -> void   */
typedef uint8_t  (*fn_inject)(void*, void*, void*, int32_t);    /* env,thiz,event,device  */
typedef int      (*fn_jnionload)(void* /*vm*/, void* /*reserved*/);

/* Names as registered by libunity's JNI_OnLoad. main.c looks these up via
 * jni_lookup_unity_native() after JNI_OnLoad has run. */
#define VEC_NATIVE_initJni                  "initJni"
#define VEC_NATIVE_nativeRecreateGfxState   "nativeRecreateGfxState"
#define VEC_NATIVE_nativeSendSurfaceChanged "nativeSendSurfaceChangedEvent"
#define VEC_NATIVE_nativeRender             "nativeRender"
#define VEC_NATIVE_nativeInjectEvent        "nativeInjectEvent"
#define VEC_NATIVE_nativeResume             "nativeResume"
#define VEC_NATIVE_nativePause              "nativePause"
#define VEC_NATIVE_nativeFocusChanged       "nativeFocusChanged"
#define VEC_NATIVE_nativeDone               "nativeDone"
#define VEC_NATIVE_nativeApplicationUnload  "nativeApplicationUnload"

/* link-time offset -> runtime address (libunity links at base 0) */
#define VEC_UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what Java UnityPlayer does; main.c does it here):
 *
 *   initJni(env, thiz, context);                         // early init
 *   nativeRecreateGfxState(env, thiz, 0, surface);       // give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);            // build GL state
 *   nativeResume(env, thiz);
 *   nativeFocusChanged(env, thiz, 1);
 *   for (;;) {
 *       // input: nativeInjectEvent(env, thiz, motionEvent, deviceId);
 *       if (!nativeRender(env, thiz)) break;             // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 * ===========================================================================
 *
 * Full 49-method table (offset / name / signature), for reference:
 *
 *   0x37E53C nativeProxyInvoke        (JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;
 *   0x37E670 nativeProxyFinalize      (J)V
 *   0x37E6B8 nativeProxyLogJNIInvokeException (J)V
 *   0x37E734 nativeProxyJNIFreeGCHandle (J)V
 *   0x5E9CC4 nativeGetSoftInputType   ()I
 *   0x5ED150 initializeARCore         (Landroid/app/Activity;)V
 *   0x5ED1B4 pauseARCore              ()V
 *   0x5ED208 resumeARCore             ()V
 *   0x5EF5FC nativeStatusQueryResult  (Ljava/lang/String;II)V
 *   0x5F043C initHFPStatusJni         ()V
 *   0x5F0488 deinitHFPStatusJni       ()V
 *   0x5F67EC onAudioVolumeChanged     (I)V
 *   0x5F6A30 nativeUpdateOrientationLockState (I)V
 *   0x60CC90 initCamera2Jni           ()V
 *   0x60CCDC deinitCamera2Jni         ()V
 *   0x611430 nativeSurfaceTextureReady (Ljava/lang/Object;)V
 *   0x611598 nativeFrameReady         (Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;III)V
 *   0x614030 initJni                  (Landroid/content/Context;)V
 *   0x61403C nativeDone               ()Z
 *   0x6140CC nativePause              ()Z
 *   0x614130 nativeResume             ()V
 *   0x614178 nativeLowMemory          ()V
 *   0x6141C0 nativeApplicationUnload  ()V
 *   0x614210 nativeFocusChanged       (Z)V
 *   0x614264 nativeRecreateGfxState   (ILandroid/view/Surface;)V
 *   0x6142CC nativeSendSurfaceChangedEvent ()V
 *   0x614324 nativeRender             ()Z
 *   0x614384 nativeInjectEvent        (Landroid/view/InputEvent;I)Z
 *   0x614678 nativeSetInputArea       (IIII)V
 *   0x6146F8 nativeSetKeyboardIsVisible (Z)V
 *   0x614750 nativeSetInputString     (Ljava/lang/String;)V
 *   0x6147F0 nativeSetInputSelection  (II)V
 *   0x614858 nativeSoftInputCanceled  ()V
 *   0x6148A8 nativeSoftInputLostFocus ()V
 *   0x6148F8 nativeReportKeyboardConfigChanged ()V
 *   0x614940 nativeSoftInputClosed    ()V
 *   0x614990 nativeUnitySendMessage   (Ljava/lang/String;Ljava/lang/String;[B)V
 *   0x614B40 nativeIsAutorotationOn   ()Z
 *   0x614BA0 nativeMuteMasterAudio    (Z)V
 *   0x614BFC nativeSetLaunchURL       (Ljava/lang/String;)V
 *   0x614D34 nativeHidePreservedContent ()V
 *   0x614D7C nativeOrientationChanged (II)V
 *   0x614DDC nativeGetNoWindowMode    ()Z
 *   0xB30F68 nOnChoreographer          (JJ)V
 *   0xB33088 nSetSupportedRefreshPeriods (J[J[I)V
 *   0xB33268 nOnRefreshPeriodChanged  (JJJJ)V
 *   0xC6B07C fmodGetInfo              (I)I
 *   0xC6B144 fmodProcess              (Ljava/nio/ByteBuffer;)I
 *   0xC6B1D0 fmodProcessMicData       (Ljava/nio/ByteBuffer;I)I
 */
#endif /* VECTOR_ENTRYPOINTS_H */
