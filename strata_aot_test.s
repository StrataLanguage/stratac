	.def	@feat.00;
	.scl	3;
	.type	0;
	.endef
	.globl	@feat.00
@feat.00 = 0
	.file	"test"
	.def	forty_two;
	.scl	2;
	.type	32;
	.endef
	.text
	.globl	forty_two
	.p2align	4
forty_two:
	movl	$42, %eax
	retq

