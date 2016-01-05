/* Copyright 2016 Tom Herbert <tom@herbertland.com>
 *
 * Checksum partial calculation
 *
 * __wsum csum_partial(const void *buff, int len, __wsum sum)
 *
 * Computes the checksum of a memory block at buff, length len,
 * and adds in "sum" (32-bit)
 *
 * Returns a 32-bit number suitable for feeding into itself
 * or csum_tcpudp_magic
 *
 * Register usage:
 *   %rdi: argument 1, buff
 *   %rsi: argument 2, length
 *   %rdx: argument 3, add in value
 *   %rax,%eax: accumulator and return value
 *   %rcx,%ecx: counter and tmp
 *   %r11: tmp
 *
 * Basic algorithm:
 *   1) Sum 8 bytes at a time using adcq (unroll main loop
 *      to do 64 bytes at a time)
 *   2) Sum remaining length (less than 8 bytes)
 *
 * Note that buffer aligment is not considered, unaligned accesses on x86 don't
 * seem to be a performance hit (CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS is set).
 */

#include <linux/linkage.h>
#include <asm/errno.h>
#include <asm/asm.h>

#define branch_tbl_len		.L_branch_tbl_len

ENTRY(csum_partial)
	movl	%edx, %eax	/* Initialize with initial sum argument */

	/* Check length */
	cmpl	$8, %esi
	jg	10f
	jl	20f

	/* Exactly 8 bytes length */
	addl	(%rdi), %eax
	adcl	4(%rdi), %eax
	adcl	$0, %eax
	ret

	/* Less than 8 bytes length */
20:	clc
	jmpq *branch_tbl_len(, %rsi, 8)

	/* Greater than 8 bytes length. Determine number of quads (n). Sum
	 * over first n % 8 quads
	 */
10:	movl	%esi, %ecx
	shrl	$3, %ecx
	andl	$0x7, %ecx
	negq	%rcx
	lea	20f(, %rcx, 4), %r11
	clc
	jmp	*%r11

.align 8
	adcq	6*8(%rdi),%rax
	adcq	5*8(%rdi),%rax
	adcq	4*8(%rdi),%rax
	adcq	3*8(%rdi),%rax
	adcq	2*8(%rdi),%rax
	adcq	1*8(%rdi),%rax
	adcq	0*8(%rdi),%rax
	nop
20:	/* #quads % 8 jump table base */

	adcq	$0, %rax
	shlq	$3, %rcx
	subq	%rcx, %rdi /* %rcx is already negative length */

	/* Now determine number of blocks of 8 quads. Sum 64 bytes at a time
	 * using unrolled loop.
	 */
	movl	%esi, %ecx
	shrl	$6, %ecx
	jz	30f
	clc

	/* Main loop */
40:	adcq	0*8(%rdi),%rax
	adcq	1*8(%rdi),%rax
	adcq	2*8(%rdi),%rax
	adcq	3*8(%rdi),%rax
	adcq	4*8(%rdi),%rax
	adcq	5*8(%rdi),%rax
	adcq	6*8(%rdi),%rax
	adcq	7*8(%rdi),%rax
	lea	64(%rdi), %rdi
	loop	40b

	adcq	$0, %rax

	/* Handle remaining length which is < 8 bytes */
30:	andl	$0x7, %esi

	/* Fold 64 bit sum to 32 bits */
	movq	%rax, %rcx
	shrq	$32, %rcx
	addl	%ecx, %eax

	jmpq *branch_tbl_len(, %rsi, 8)

/* Length table targets */

107:	/* Length 7 */
	adcw	4(%rdi), %ax
105:	/* Length 5 */
	adcw	2(%rdi), %ax
103:	/* Length 3 */
	adcw	(%rdi), %ax
101:	/* Length 1, grab the odd byte */
	adcb	-1(%rdi, %rsi), %al
	adcb	$0, %ah
	adcl	$0, %eax
	ret
106:	/* Length 6 */
	adcw	4(%rdi), %ax
104:	/* Length 4 */
	adcl	(%rdi), %eax
	adcl	$0, %eax
	ret
102:	/* Length 2 */
	adcw	(%rdi), %ax
100:	/* Length 0 */
	adcl	$0, %eax
	ret

.section .rodata
.align 64
.L_branch_tbl_len:
	.quad	100b
	.quad	101b
	.quad	102b
	.quad	103b
	.quad	104b
	.quad	105b
	.quad	106b
	.quad	107b
