	.build_version macos, 28, 0
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_add
	.p2align	2
_add:
	.cfi_startproc
	sub	sp, sp, #16
	.cfi_def_cfa_offset 16
	stp	w0, w1, [sp, #8]
	add	w0, w0, w1
	add	sp, sp, #16
	ret
	.cfi_endproc

	.globl	_mul
	.p2align	2
_mul:
	.cfi_startproc
	sub	sp, sp, #16
	.cfi_def_cfa_offset 16
	mul	w8, w0, w1
	stp	w0, w1, [sp, #8]
	mov	w0, w8
	add	sp, sp, #16
	ret
	.cfi_endproc

	.globl	_main
	.p2align	2
_main:
	.cfi_startproc
	stp	x20, x19, [sp, #-32]!
	stp	x29, x30, [sp, #16]
	.cfi_def_cfa_offset 32
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	.cfi_offset w19, -24
	.cfi_offset w20, -32
	mov	w0, #2
	mov	w1, #3
	bl	_add
	mov	w19, w0
	mov	w0, #4
	mov	w1, #5
	bl	_mul
	ldp	x29, x30, [sp, #16]
	add	w0, w19, w0
	ldp	x20, x19, [sp], #32
	ret
	.cfi_endproc

.subsections_via_symbols
