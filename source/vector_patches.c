/* vector_patches.c -- game-specific IL2CPP patches for Vector.
 *
 * All patches are self-verifying (expected instruction word checked first) and
 * are applied only after the modules are loaded and finalized.
 */
#include <stdint.h>
#include "config.h"
#include "util.h"
#include "so_util.h"
#include "vector_patches.h"

/* ---------------------------------------------------------------------------
 * License gate
 *
 * Vector is the paid build. LicenseCheckingManager.InitInternal selects
 * LicenseCheckingAndroid (Google LVL via com.android.vending) and
 * Preloader.LicenseChecking waits for LicenseCheckingManager.Result:
 *
 *   Result == 1 (Allow)  -> continue to LoadProcess
 *   anything else        -> UnityEngine.Application.Quit()
 *
 * On Switch there is no Play Store licensing service, so the Android checker
 * can never answer. Vector also ships LicenseCheckingNone, whose RunCheck is:
 *
 *   0x2A27024  str  x30, [sp, #-0x10]!
 *   0x2A27028  cbz  x1, ...
 *   0x2A2702C  mov  w8, #1
 *   0x2A27030  str  w8, [x1, #0x18]     ; LicenseCheckingManager.Result = 1
 *   0x2A27034  ldr  x30, [sp], #0x10
 *   0x2A27038  ret
 *
 * The Android RunCheck has the same signature (x0 = this, x1 = manager), so
 * replacing its entry with an unconditional branch to the None implementation
 * makes the gate return Allow without emulating the Play licensing service.
 *
 * This does not forge a purchase or bypass PairIP; it selects the game's own
 * "no license service" implementation for a lawfully owned copy running on
 * non-Android hardware. Purchases stay unavailable.
 * ------------------------------------------------------------------------- */
#define VEC_RVA_LICENSE_ANDROID_RUNCHECK  0x2A26A50u
#define VEC_RVA_LICENSE_NONE_RUNCHECK     0x2A27024u
/* str x30, [sp, #-0x40]! -- first word of LicenseCheckingAndroid.RunCheck */
#define VEC_LICENSE_ANDROID_ENTRY_WORD    0xF81C0FFEu

/* ---- startup service gates ------------------------------------------------
 * AppsFlyerHandler.get_IsSupportedPlatform returns true for Android (11) and
 * iPhonePlayer (8); on this Android build it would try to initialize the
 * AppsFlyer Java SDK. FirebaseHandler.get_IsSuported similarly gates Firebase.
 * Both are irrelevant to offline play. Their entries share the prologue
 *   str x30,[sp,#-0x20]! ; stp x20,x19,[sp,#0x10]
 * so replacing the first two words with `mov w0,#0 ; ret` returns false
 * without touching the stack.
 *
 * Preloader.<LicenseChecking>d__4::MoveNext checks Options.GDPR (byte +0x2A)
 * and, when false, shows the consent popup and waits for a tap. The port has no
 * ads/telemetry and no input injection yet, so the cbz is replaced with a NOP:
 * execution falls through to Preloader::LoadProcess. Remove this patch once
 * native touch injection is wired and the popup can be answered normally.
 * ------------------------------------------------------------------------- */
#define VEC_RVA_APPSFLYER_IS_SUPPORTED    0x2A259A8u
#define VEC_RVA_FIREBASE_IS_SUPPORTED     0x2A08D5Cu
#define VEC_RVA_PRELOADER_GDPR_CBZ        0x2A22D08u

/* ---- offline startup: disable ConfigManager's remote config download ------
 * ConfigManager.InitInternal creates the default ADConfig, parses the local
 * file, then starts StartDownloadConfig via Routiner.Go. On Switch the
 * UnityWebRequest to assets.nekkimobile.ru fails DNS ("Cannot resolve
 * destination host") and leaves a job group pending; the next nativeRender
 * waits on it forever (black screen, UnityMain cond_wait). The default
 * ADConfig is all-zero chances, which is the correct offline policy, so skip
 * the download entirely:
 *   0x2A35E78  bl StartDownloadConfig  -> nop
 *   0x2A35E88  b  Routiner.Go          -> ret
 * The intervening epilogue restores x20/x19/x30 and the stack. */
#define VEC_RVA_CONFIG_DOWNLOAD_BL        0x2A35E78u
#define VEC_RVA_CONFIG_DOWNLOAD_B         0x2A35E88u
#define VEC_CONFIG_DOWNLOAD_BL_WORD       0x94000015u  /* bl 0x2A35ECC */
#define VEC_CONFIG_DOWNLOAD_B_WORD        0x17B9DD64u  /* b  0x18AD418 */

/* Preloader.<InitAds>d__7::MoveNext: the Google Mobile Ads consent side
 * coroutine. It is not a boot barrier, but its update can leave a Unity job
 * pending on a console with no Play/Ads services. Return false immediately so
 * the side coroutine completes without touching the ads SDK. */
#define VEC_RVA_PRELOADER_INITADS_MOVENEXT 0x2A22958u
#define VEC_PRELOADER_INITADS_PROLOGUE_0   0xF81D0FFEu
#define VEC_PRELOADER_INITADS_PROLOGUE_1   0xA90157F6u
#define VEC_RETURN_FALSE_0                 0x52800000u
#define VEC_RETURN_FALSE_1                 0xD65F03C0u
#define VEC_PROLOGUE_0                    0xF81E0FFEu  /* str x30,[sp,#-0x20]! */
#define VEC_PROLOGUE_1                    0xA9014FF4u  /* stp x20,x19,[sp,#0x10] */
#define VEC_RETURN_FALSE_0                0x52800000u  /* mov w0,#0 */
#define VEC_RETURN_FALSE_1                0xD65F03C0u  /* ret */
#define VEC_PRELOADER_GDPR_CBZ_WORD       0x34000228u  /* cbz w8, popup */
#define VEC_ARM64_NOP                     0xD503201Fu

/* ---- audio ---------------------------------------------------------------
 *
 * Unity's Android FMOD output defaults to the Java AudioTrack driver, which
 * needs the Java FMODAudioDevice.run() loop. This port has no JVM, so that
 * path is silent and never calls our OpenSL ES shim.
 *
 * The output type is chosen here in libunity:
 *
 *   0x6F5EC0  cmp  w0, #3
 *   0x6F5EC4  mov  w8, #0x15          ; 21 = AudioTrack
 *   0x6F5EC8  mov  w9, #0x17          ; 23 = AAudio
 *   0x6F5ED0  cmp  w0, #2
 *   0x6F5ED4  mov  w9, #0x16          ; 22 = OpenSL ES
 *   0x6F5ED8  csel w21, w9, w8, eq
 *   0x6F5EDC  ldr  x0, [x19, #0x158]  ; FMOD system
 *   0x6F5EE0  mov  w1, w21            <- patch
 *   0x6F5EE8  bl   System::setOutput
 *
 * Force w1 to 22 (our opensles.c shim). This is the same output-type mapping
 * the Phigros/aota ports use for this Unity generation. Verify-first: the
 * guard word is the exact `mov w1,w21`; any other build skips the patch.
 */
#define VEC_RVA_FMOD_SET_OUTPUT           0x6F5EE0u
#define VEC_FMOD_SET_OUTPUT_WORD          0x2A1503E1u  /* mov w1, w21 */
#define VEC_FMOD_SET_OUTPUT_OPENSL        0x528002C1u  /* movz w1, #22 */

/* ---- intro/outro video ----------------------------------------------------
 *
 * UI.VideoScreenView plays `Assets/Videos/intro.mp4` after the loading scene.
 * Unity's Android VideoPlayer uses MediaExtractor/MediaCodec, which this port
 * does not implement (hardware log 0.2.4:
 *   "[stub] AMediaExtractor_new called -- unsupported on Switch"
 *   "Unity: Video error: VideoPlayer cannot play clip : Assets/Videos/intro.mp4"
 * ). The screen then stays black forever: OnError at 0x2A40184 only logs the
 * message and never tells the flow that the video finished.
 *
 * OnError(VideoPlayer source, string message) already has the source in x1,
 * exactly like OnFinished(VideoPlayer source). Branch straight to OnFinished
 * (0x2A40214), which unsubscribes the video events, stops the player and calls
 * the payload's completion callback. This skips both the intro and the ending
 * movie; the rest of the game does not need the video.
 *
 *   0x2A40184  str x30,[sp,#-0x30]!  ->  b 0x2A40214
 */
#define VEC_RVA_VIDEO_ON_ERROR            0x2A40184u
#define VEC_VIDEO_ON_ERROR_WORD           0xF81D0FFEu  /* str x30,[sp,#-0x30]! */
#define VEC_RVA_VIDEO_ON_FINISHED         0x2A40214u

/* ---- frame pacing / UnityChoreographer ------------------------------------
 *
 * Unity 2022.3's Android frame timer lazily creates a native
 * "UnityChoreographer" helper on the first real render frame. On this port
 * that object's start event is never signalled: UnityMain parks in
 * pthread_cond_wait inside the constructor at 0x60B6C4 and the frame counter
 * stops at 2 (0.1.14 hardware log: `[wd] STALL #1 ... last frame=2`).
 *
 * Two verify-first fixes:
 *
 *   0x5EAC20  Swappy::IsEnabledAndActive()  -> mov w0,#0 ; ret
 *   0x60B6CC  cbz x8, wait                  -> b 0x60B6E8 (skip the wait)
 *
 * The first is the same frame-pacing gate the exact-revision Phigros port
 * disables. The second only skips the unsatisfied start-event predicate; the
 * object's mutex and condition variable remain initialized. If a later log
 * shows a different wait, that is a new bottleneck, not this one.
 * ------------------------------------------------------------------------- */
#define VEC_RVA_SWAPPY_IS_ENABLED_ACTIVE  0x5EAC20u
#define VEC_SWAPPY_ENTRY_0                0xA9BF4FFEu  /* stp x30,x19,[sp,#-0x10]! */
#define VEC_SWAPPY_ENTRY_1                0x900052F3u  /* adrp x19, #0x1046000 */
#define VEC_RVA_UNITY_CHOREO_WAIT         0x60B6CCu
#define VEC_UNITY_CHOREO_WAIT_WORD        0xB4000068u  /* cbz x8, 0x60B6D8 */
#define VEC_UNITY_CHOREO_SKIP_WORD        0x14000007u  /* b 0x60B6E8 */

/* The second frame limiter waits for the Choreographer frame counter at
 * libunity+0x104B1E8 to reach the target frame:
 *
 *   0x608CB4  lock 0x104B190
 *   0x608CD8  ldr x21,[0x104B1E8] ; cmp x21,x19 ; b.ge done
 *   0x608CEC  pthread_cond_wait(0x104B1B8, 0x104B190)
 *
 * The counter is only advanced by the UnityChoreographer thread's tick
 * (0x60B9BC); that thread is not running on this port. main.c now calls
 * 0x60B9BC once per frame, and this call-site NOP makes the wait non-blocking
 * even if the singleton has not appeared yet. The caller does not use the
 * return value; the frame-time extrapolation uses x20 and the stack sample.
 */
#define VEC_RVA_FRAME_PACING_WAIT_CALL    0x5E1588u
#define VEC_FRAME_PACING_WAIT_CALL_WORD   0x94009DCBu  /* bl 0x608CB4 */

/* ---- managed-exception attribution ---------------------------------------
 *
 * 0.1.9 proved generated code bypasses exported il2cpp_raise_exception. The
 * IndexOutOfRange throw helper itself is shared by the generated bounds checks:
 *
 *   0x17F2598  str x30,[sp,#-16]!
 *   0x17F259C  bl  0x17F25A8  (create IndexOutOfRangeException)
 *   0x17F25A0  mov x1,xzr
 *   0x17F25A4  bl  0x17F24B4  (Exception::Raise)
 *
 * The naked trampoline records the original caller LR, then reproduces those
 * four instructions directly. It is diagnostic only: the generated exception
 * object and the original raise path are untouched.
 * ------------------------------------------------------------------------- */
#define VEC_RVA_THROW_INDEX_OUT_OF_RANGE  0x17F2598u
#define VEC_RVA_INDEX_EXCEPTION_CREATE    0x17F25A8u
#define VEC_RVA_IL2CPP_RAISE_IMPL         0x17F24B4u
#define VEC_THROW_INDEX_ENTRY_0           0xF81F0FFEu
#define VEC_THROW_INDEX_ENTRY_1           0x94000003u
#define VEC_THROW_INDEX_ENTRY_2           0xAA1F03E1u
#define VEC_THROW_INDEX_ENTRY_3           0x97FFFFC4u

/* The trace itself changes C++ unwind metadata and causes an abort after it
 * successfully identifies the thrower. The caller is now known, so leave the
 * original helper unmodified while we inspect its actual input path. */
#define VEC_ENABLE_INDEX_EXCEPTION_TRACE   0

/* ---- Android identity compatibility ---------------------------------------
 *
 * EncryptUtil::GetUnicByteArray calls NativeUniqueIDGetter.get_AndroidID()
 * before it UTF-8 encodes and indexes the identifier.  The generic fake JNI
 * layer is not entered by UnityEngine.AndroidJavaClass for this call, so its
 * fallback cannot supply a value.  Replace precisely this generated getter
 * with a small ABI-compatible function that returns a managed string made by
 * libil2cpp itself.  The static field write in the original caller remains in
 * place, exactly as it does for the normal Android implementation.
 *
 * The four-word guard is the unmodified getter prologue from this Vector build.
 * If the export, address, or bytes do not match, no code is changed.
 * ------------------------------------------------------------------------- */
#define VEC_RVA_NATIVE_UNIQUEID_ANDROID_ID 0x18B20D8u
#define VEC_ANDROID_ID_ENTRY_0             0xF81D0FFEu
#define VEC_ANDROID_ID_ENTRY_1             0xA90157F6u
#define VEC_ANDROID_ID_ENTRY_2             0xA9024FF4u
#define VEC_ANDROID_ID_ENTRY_3             0x9000FB73u

typedef void *(*VecIl2CppStringNew)(const char *value);
static VecIl2CppStringNew g_vec_string_new;
static void *g_vec_android_id;

__attribute__((noinline, used)) static void *vec_native_uniqueid_android_id(void) {
  static int calls;
  if (calls < 3) {
    calls++;
    debugPrintf("[id] NativeUniqueID getter called (call %d)\n", calls);
  }
  if (!g_vec_android_id && g_vec_string_new) {
    g_vec_android_id = g_vec_string_new(CS_ANDROID_ID);
    debugPrintf("[id] NativeUniqueID AndroidID synthetic string: %s\n",
                g_vec_android_id ? "ready" : "FAILED");
  }
  return g_vec_android_id;
}

static int install_native_uniqueid_android_id(uintptr_t il2cpp_base,
                                              void *string_new) {
  const uint32_t expect[4] = {
    VEC_ANDROID_ID_ENTRY_0, VEC_ANDROID_ID_ENTRY_1,
    VEC_ANDROID_ID_ENTRY_2, VEC_ANDROID_ID_ENTRY_3
  };
  volatile uint32_t *site = (volatile uint32_t *)(il2cpp_base +
                                                   VEC_RVA_NATIVE_UNIQUEID_ANDROID_ID);
  if (!string_new) {
    debugPrintf("[patch] AndroidID helper skipped: il2cpp_string_new unavailable\n");
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    if (site[i] != expect[i]) {
      debugPrintf("[patch] AndroidID helper guard mismatch @0x%X+%d: have 0x%08X want 0x%08X -> skipped\n",
                  VEC_RVA_NATIVE_UNIQUEID_ANDROID_ID, i * 4, site[i], expect[i]);
      return 0;
    }
  }
  uint32_t detour[4] = { 0x58000051u, 0xD61F0220u, 0, 0 };
  *(uint64_t *)(detour + 2) = (uint64_t)(uintptr_t)vec_native_uniqueid_android_id;
  g_vec_string_new = (VecIl2CppStringNew)string_new;
  if (so_patch_code((void *)site, detour, sizeof detour) != 0) {
    g_vec_string_new = NULL;
    debugPrintf("[patch] AndroidID helper write failed -> skipped\n");
    return 0;
  }
  debugPrintf("[patch] AndroidID helper -> synthetic stable ID installed\n");
  return 1;
}

static uintptr_t g_vec_il2cpp_base;
extern uintptr_t vec_trace_create_target;
extern uintptr_t vec_trace_raise_target;

__attribute__((noinline, used)) void vec_log_index_exception_caller(uintptr_t caller) {
  static unsigned count;
  if (count < 8) {
    const intptr_t rva = (intptr_t)caller - (intptr_t)g_vec_il2cpp_base;
    debugPrintf("[exc] IndexOutOfRange #%u caller=%p (libil2cpp%+ld)\n",
                count + 1, (void *)caller, (long)rva);
  }
  count++;
}

extern void vec_index_exception_trace_hook(void);

static int install_index_exception_trace(uintptr_t il2cpp_base) {
  const uint32_t expect[4] = {
    VEC_THROW_INDEX_ENTRY_0, VEC_THROW_INDEX_ENTRY_1,
    VEC_THROW_INDEX_ENTRY_2, VEC_THROW_INDEX_ENTRY_3
  };
  volatile uint32_t *site = (volatile uint32_t *)(il2cpp_base + VEC_RVA_THROW_INDEX_OUT_OF_RANGE);
  for (int i = 0; i < 4; i++) {
    if (site[i] != expect[i]) {
      debugPrintf("[patch] index exception trace guard mismatch @0x%X+%d: have 0x%08X want 0x%08X -> skipped\n",
                  VEC_RVA_THROW_INDEX_OUT_OF_RANGE, i * 4, site[i], expect[i]);
      return 0;
    }
  }
  uint32_t detour[4] = { 0x58000051u, 0xD61F0220u, 0, 0 };  /* ldr x17,#8; br x17; .quad */
  *(uint64_t *)(detour + 2) = (uint64_t)(uintptr_t)vec_index_exception_trace_hook;
  g_vec_il2cpp_base = il2cpp_base;
  vec_trace_create_target = il2cpp_base + VEC_RVA_INDEX_EXCEPTION_CREATE;
  vec_trace_raise_target = il2cpp_base + VEC_RVA_IL2CPP_RAISE_IMPL;
  if (so_patch_code((void *)site, detour, sizeof detour) != 0) {
    debugPrintf("[patch] index exception trace write failed -> skipped\n");
    vec_trace_create_target = 0;
    vec_trace_raise_target = 0;
    return 0;
  }
  debugPrintf("[patch] index exception trace installed at libil2cpp+0x%X\n",
              VEC_RVA_THROW_INDEX_OUT_OF_RANGE);
  return 1;
}

static int patch_branch(uintptr_t base, uint32_t from_rva, uint32_t to_rva,
                        uint32_t expect_word, const char *tag) {
  volatile uint32_t *site = (volatile uint32_t *)(base + from_rva);
  uint32_t cur = *site;
  if (cur != expect_word) {
    debugPrintf("[patch] %s guard mismatch @0x%X: have 0x%08X want 0x%08X -> skipped\n",
                tag, from_rva, cur, expect_word);
    return 0;
  }
  int32_t delta = (int32_t)(to_rva - from_rva);
  if (delta & 3) {
    debugPrintf("[patch] %s target not 4-byte aligned -> skipped\n", tag);
    return 0;
  }
  uint32_t insn = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03FFFFFFu);  /* b target */
  if (so_patch_code((void *)site, &insn, sizeof insn) != 0) {
    debugPrintf("[patch] %s write failed -> skipped\n", tag);
    return 0;
  }
  debugPrintf("[patch] %s: b 0x%X from 0x%X (0x%08X)\n", tag, to_rva, from_rva, insn);
  return 1;
}

static int patch_words(uintptr_t base, uint32_t rva,
                       const uint32_t *expect, const uint32_t *repl,
                       int nwords, const char *tag) {
  volatile uint32_t *site = (volatile uint32_t *)(base + rva);
  for (int i = 0; i < nwords; i++) {
    uint32_t cur = site[i];
    if (cur != expect[i]) {
      debugPrintf("[patch] %s guard mismatch @0x%X+%d: have 0x%08X want 0x%08X -> skipped\n",
                  tag, rva, i * 4, cur, expect[i]);
      return 0;
    }
  }
  if (so_patch_code((void *)site, repl, (size_t)nwords * 4) != 0) {
    debugPrintf("[patch] %s write failed -> skipped\n", tag);
    return 0;
  }
  debugPrintf("[patch] %s: %d word(s) at 0x%X\n", tag, nwords, rva);
  return 1;
}

/* ---------------------------------------------------------------------------
 * Unity 2022.3.62f2 memory-region granularity: 256 MB -> 64 MB.
 *
 * This is the patch every working Unity port in this lineage applies. The
 * stock block allocator reserves 256 MB-aligned regions; on a 4 GB Switch that
 * reservation pattern exhausts the so-loader address space and the Dynamic
 * Heap dies (or over-reserves until the tenth 511 MB pool fails). The 23
 * instruction words below were derived from Vector's OWN libunity.so: all 23
 * {from} words are present at these RVAs and match the Phigros 2022.3.62f2
 * table byte-for-byte (same Unity revision, different link addresses). The
 * transforms are the standard MOVZ/MOVK 0x1000->0x0400, MOVN 0xF000->0xFC00,
 * UBFM/LSR 28->26, and the level-1 >>40->>>38 shifts.
 *
 * VERIFY-FIRST, ALL-OR-NOTHING: if any word is neither {from} nor {to}, patch
 * nothing and log. A partially patched allocator is worse than stock.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t rva, from, to; } VecGranWord;
static const VecGranWord VEC_GRANULARITY_WORDS[] = {
  { 0x3B141C, 0xD35CFC28, 0xD35AFC28 }, /* TLSAllocator lsr #28->#26 */
  { 0x3B1420, 0x52A20009, 0x52A08009 }, /* TLSAllocator movz 0x1000->0x0400 */
  { 0x3B0B78, 0x12BE0009, 0x12BF8009 }, /* LocalLowLevelAllocator MOVN */
  { 0x3B0B80, 0x92648D36, 0x92669536 }, /* LocalLowLevelAllocator and mask */
  { 0x3B2E78, 0x52A20009, 0x52A08009 }, /* BucketAllocator movz */
  { 0x3B5148, 0xD35CFD29, 0xD35AFD29 }, /* DynamicHeapAllocator lsr */
  { 0x3B514C, 0x52A2000A, 0x52A0800A }, /* DynamicHeapAllocator movz */
  { 0x3B55F4, 0x12BE000A, 0x12BF800A }, /* RequestLargeAllocMem MOVN */
  { 0x3B55FC, 0x92648D36, 0x92669536 }, /* RequestLargeAllocMem and mask */
  { 0x3B75FC, 0xD35CDC33, 0xD35AD433 }, /* VirtualAllocator::MarkMemoryBlocks ubfx */
  { 0x3B7600, 0xD35CFD15, 0xD35AFD15 }, /* VirtualAllocator::MarkMemoryBlocks lsr */
  { 0x3B7690, 0x52A20008, 0x52A08008 }, /* VirtualAllocator::ReserveMemoryBlock movz */
  { 0x3B79CC, 0xD35CFC28, 0xD35AFC28 }, /* GetMemoryBlockFromPointer lsr */
  { 0x3B79DC, 0x92646C28, 0x92667428 }, /* GetMemoryBlockFromPointer and mask */
  { 0x3B79E4, 0xD35C9C2A, 0xD35A942A }, /* GetMemoryBlockFromPointer ubfx */
  { 0x3B79FC, 0xD35CDC29, 0xD35AD429 }, /* GetMemoryBlockFromPointer ubfx */
  { 0x3B7A00, 0xF2A2000B, 0xF2A0800B }, /* GetMemoryBlockFromPointer movk */
  { 0x3B7A40, 0xCB0A7108, 0xCB0A6908 }, /* GetMemoryBlockFromPointer sub lsl */
  { 0x3B79F8, 0xB25C6FEB, 0xB25E77EB }, /* GetMemoryBlockFromPointer -256*gran */
  { 0x3B7A58, 0xD368FC28, 0xD366FC28 }, /* GetBlockInfoFromPointer L1 >>40->38 */
  { 0x3B7A68, 0xD35C9C29, 0xD35A9429 }, /* GetBlockInfoFromPointer ubfx */
  { 0x3B971C, 0xD368FC28, 0xD366FC28 }, /* GetAllocatorContainingPtr L1 >>40->38 */
  { 0x3B9734, 0xD35C9E89, 0xD35A9689 }, /* GetAllocatorContainingPtr ubfx */
};
#define VEC_GRANULARITY_N ((int)(sizeof(VEC_GRANULARITY_WORDS)/sizeof(VEC_GRANULARITY_WORDS[0])))

static int patch_granularity(uintptr_t base) {
  int stock = 0, patched = 0;
  for (int i = 0; i < VEC_GRANULARITY_N; i++) {
    const VecGranWord *w = &VEC_GRANULARITY_WORDS[i];
    uint32_t cur = *(volatile uint32_t *)(base + w->rva);
    if (cur == w->from) stock++;
    else if (cur == w->to) patched++;
    else {
      debugPrintf("[patch] granularity mismatch @0x%X: have 0x%08X want 0x%08X -> SKIP ALL (%d sites)\n",
                  w->rva, cur, w->from, VEC_GRANULARITY_N);
      return 0;
    }
  }
  if (patched == VEC_GRANULARITY_N) {
    debugPrintf("[patch] libunity granularity already 64MB (%d sites)\n", VEC_GRANULARITY_N);
    return 1;
  }
  if (stock != VEC_GRANULARITY_N) {
    debugPrintf("[patch] libunity granularity PARTIALLY patched (%d/%d) -> SKIP\n",
                patched, VEC_GRANULARITY_N);
    return 0;
  }
  for (int i = 0; i < VEC_GRANULARITY_N; i++) {
    const VecGranWord *w = &VEC_GRANULARITY_WORDS[i];
    so_patch_code((void *)(base + w->rva), &w->to, sizeof w->to);
  }
  debugPrintf("[patch] libunity region granularity 256MB->64MB patched (%d sites)\n",
              VEC_GRANULARITY_N);
  return 1;
}

int vector_install_patches(uintptr_t unity_base, uintptr_t il2cpp_base,
                           void *string_new) {
  int n = 0;

  /* MUST run before Unity's MemoryManager initializes. */
  n += patch_granularity(unity_base);

  n += patch_branch(il2cpp_base,
                    VEC_RVA_LICENSE_ANDROID_RUNCHECK,
                    VEC_RVA_LICENSE_NONE_RUNCHECK,
                    VEC_LICENSE_ANDROID_ENTRY_WORD,
                    "license: Android.RunCheck -> None.RunCheck");

  n += install_native_uniqueid_android_id(il2cpp_base, string_new);

  {
    const uint32_t expect[2] = { VEC_PROLOGUE_0, VEC_PROLOGUE_1 };
    const uint32_t ret_false[2] = { VEC_RETURN_FALSE_0, VEC_RETURN_FALSE_1 };
    n += patch_words(il2cpp_base, VEC_RVA_APPSFLYER_IS_SUPPORTED,
                     expect, ret_false, 2, "service: AppsFlyer IsSupportedPlatform -> false");
    n += patch_words(il2cpp_base, VEC_RVA_FIREBASE_IS_SUPPORTED,
                     expect, ret_false, 2, "service: Firebase IsSuported -> false");
  }

  {
    const uint32_t expect[1] = { VEC_PRELOADER_GDPR_CBZ_WORD };
    const uint32_t nop[1] = { VEC_ARM64_NOP };
    n += patch_words(il2cpp_base, VEC_RVA_PRELOADER_GDPR_CBZ,
                     expect, nop, 1, "preloader: skip GDPR consent popup");
  }

  {
    const uint32_t expect_bl[1] = { VEC_CONFIG_DOWNLOAD_BL_WORD };
    const uint32_t nop[1] = { VEC_ARM64_NOP };
    n += patch_words(il2cpp_base, VEC_RVA_CONFIG_DOWNLOAD_BL,
                     expect_bl, nop, 1, "config: skip remote download (bl)");
    const uint32_t expect_b[1] = { VEC_CONFIG_DOWNLOAD_B_WORD };
    const uint32_t ret[1] = { 0xD65F03C0u };
    n += patch_words(il2cpp_base, VEC_RVA_CONFIG_DOWNLOAD_B,
                     expect_b, ret, 1, "config: skip remote download (b)");
  }

  {
    const uint32_t expect[2] = { VEC_PRELOADER_INITADS_PROLOGUE_0,
                                 VEC_PRELOADER_INITADS_PROLOGUE_1 };
    const uint32_t ret_false[2] = { VEC_RETURN_FALSE_0, VEC_RETURN_FALSE_1 };
    n += patch_words(il2cpp_base, VEC_RVA_PRELOADER_INITADS_MOVENEXT,
                     expect, ret_false, 2, "preloader: skip InitAds side coroutine");
  }

  n += patch_branch(il2cpp_base,
                    VEC_RVA_VIDEO_ON_ERROR,
                    VEC_RVA_VIDEO_ON_FINISHED,
                    VEC_VIDEO_ON_ERROR_WORD,
                    "video: OnError -> OnFinished (skip intro/outro)");

  {
    const uint32_t expect[1] = { VEC_FMOD_SET_OUTPUT_WORD };
    const uint32_t opensl[1] = { VEC_FMOD_SET_OUTPUT_OPENSL };
    n += patch_words(unity_base, VEC_RVA_FMOD_SET_OUTPUT,
                     expect, opensl, 1, "audio: force FMOD output to OpenSL");
  }

  {
    const uint32_t expect[2] = { VEC_SWAPPY_ENTRY_0, VEC_SWAPPY_ENTRY_1 };
    const uint32_t disabled[2] = { VEC_RETURN_FALSE_0, VEC_RETURN_FALSE_1 };
    n += patch_words(unity_base, VEC_RVA_SWAPPY_IS_ENABLED_ACTIVE,
                     expect, disabled, 2, "pacing: Swappy disabled");
  }

  {
    const uint32_t expect[1] = { VEC_UNITY_CHOREO_WAIT_WORD };
    const uint32_t skip[1] = { VEC_UNITY_CHOREO_SKIP_WORD };
    n += patch_words(unity_base, VEC_RVA_UNITY_CHOREO_WAIT,
                     expect, skip, 1, "pacing: skip UnityChoreographer wait");
  }

  {
    const uint32_t expect[1] = { VEC_FRAME_PACING_WAIT_CALL_WORD };
    const uint32_t nop[1] = { VEC_ARM64_NOP };
    n += patch_words(unity_base, VEC_RVA_FRAME_PACING_WAIT_CALL,
                     expect, nop, 1, "pacing: skip frame-counter wait");
  }

  if (VEC_ENABLE_INDEX_EXCEPTION_TRACE) {
    n += install_index_exception_trace(il2cpp_base);
  } else {
    debugPrintf("[patch] index exception trace disabled (caller attributed)\n");
  }

  /* Phase 3 slots (not yet derived for this build):
   *   - FMOD output type -> OpenSL ES (22) instead of AudioTrack (21)
   *   - OpenSL buffer geometry validation
   * Add each here with a verified expected word; do not reuse Phigros offsets.
   */
  debugPrintf("[patch] vector patches applied: %d\n", n);
  return n;
}
