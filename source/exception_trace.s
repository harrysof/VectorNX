/* AArch64 trampoline for the exact IndexOutOfRange helper in Vector's
 * libil2cpp. It must be standalone assembly: devkitA64 GCC ignores the
 * `naked` attribute on C functions. The sequence logs the caller, then
 * reproduces the original helper's prologue/calls directly, with targets set
 * by the guarded runtime installer. */
	.arch armv8-a
	.text
	.align 2
	.global vec_index_exception_trace_hook
	.type vec_index_exception_trace_hook, %function
vec_index_exception_trace_hook:
	str x30, [sp, #-16]!
	/* The direct caller is IL2CPP's generic bounds-check wrapper. Its caller
	 * LR was saved at the incoming SP before it called the throw helper. */
	ldr x0, [sp, #16]
	bl vec_log_index_exception_caller
	ldr x30, [sp], #16

	/* Original: str x30,[sp,#-16]!; bl CreateIndexOutOfRangeException */
	str x30, [sp, #-16]!
	adrp x17, vec_trace_create_target
	ldr x17, [x17, #:lo12:vec_trace_create_target]
	blr x17
	mov x1, xzr
	adrp x17, vec_trace_raise_target
	ldr x17, [x17, #:lo12:vec_trace_raise_target]
	br x17
	.size vec_index_exception_trace_hook, .-vec_index_exception_trace_hook

	.data
	.align 3
	.hidden vec_trace_create_target
	.global vec_trace_create_target
	.type vec_trace_create_target, %object
	.size vec_trace_create_target, 8
vec_trace_create_target:
	.xword 0

	.hidden vec_trace_raise_target
	.global vec_trace_raise_target
	.type vec_trace_raise_target, %object
	.size vec_trace_raise_target, 8
vec_trace_raise_target:
	.xword 0
