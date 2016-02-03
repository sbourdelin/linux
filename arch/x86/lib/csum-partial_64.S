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
 * CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS determines whether alignment of the
 * buffer must be dealt with.
 *
 * If CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS is set then the steps are:
 *     1) Initialize accumulator to initial sum
 *     2) Sum 8 bytes at a time using adcq (unroll main loop
 *        to do 128 bytes at a time)
 *     3) Sum remaining length (less than 8 bytes)
 *
 * If CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS is not set then the steps are:
 *     1) Handle buffer that is not aligned to 8 bytes, sum up to 8 byte
 *        alignment
 *     2) Sum 8 bytes at a time using adcq (unroll main loop
 *        to do 128 bytes at a time)
 *     3) Sum remaining length (less than 8 bytes)
 *     4) Roll result if alignment is odd and add in initial sum argument
 *     5) If buffer is not aligned to 8 bytes and length is less than
 *        or equal to 8 - alignment (whole buffer is in one quad), then
 *        treat that as a special case.
 *
 * Register usage:
 *   %rdi: argument #1, buff
 *   %rsi: argument #2, length
 *   %rdx: argument #3, add in value
 *   %rax,%eax: accumulator and return value
 *   %rcx,%ecx: counter and tmp
 *   %r11: tmp
 *   %r10: alignment (0-7) - when CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS is set
 */

#include <linux/linkage.h>
#include <asm/errno.h>
#include <asm/asm.h>

#define branch_tbl_len .L_branch_tbl_len

#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS

/* Close the carry chain and return. */
#define	RETURN			\
	adcl	$0, %eax;	\
	ret

#else /* CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS */

/* Before returning need to roll the result if alignment was odd and then add
 * in the initial sum.
 */
#define	RETURN			\
	adcl	$0, %eax;	\
	test	$0x1, %r10d;	\
	jz	99f;		\
	roll	$8, %eax;	\
99:	addl	%edx, %eax;	\
	adcl	$0, %eax;	\
	ret

#define branch_tbl_align .L_branch_tbl_align

#endif /* CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS */

ENTRY(csum_partial)

#ifdef	CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
	movl	%edx, %eax	/* Initialize with initial sum argument */
#else	/* CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS */
	test	%esi, %esi	/* Zero length? */
	jne	310f
	movl	%edx, %eax
	ret

310:	xorl	%eax, %eax

	/* Determine alignment */
	movl	%edi, %r10d
	andl	$0x7, %r10d
	jz	10f
	movl	$8, %ecx
	subl	%r10d, %ecx
	cmpl	%ecx, %esi
	jle	320f
	clc
	jmpq	*branch_tbl_align(, %r10, 8)

	/* Whole buffer fits into one quad. Sum up to a four byte alignment
	 * and then call into the length table to finish.
	 */
320:	test	$0x1, %r10d
	jz	330f
	movb	(%rdi), %ah /* Align to two bytes */
	decl	%esi
	lea	1(%rdi), %rdi
330:	cmpl	$2, %esi
	jl	340f
	test	$0x2, %r10d
	jz	340f
	addw	(%rdi), %ax /* Align to four bytes */
	adcl	$0, %eax
	lea	2(%rdi), %rdi
	subl	$2, %esi
340:
	clc
	jmpq *branch_tbl_len(, %rsi, 8)

/* Jumps table for alignments */

201:	/* Align 1 */
	adcw	5(%rdi), %ax
203:	/* Align 3 */
	adcw	3(%rdi), %ax
205:	/* Align 5 */
	adcw	1(%rdi), %ax
207:	/* Align 7 */
	adcl	$0, %eax
	addb	(%rdi), %ah
	jmp	222f
202:	/* Align 2 */
	adcw	4(%rdi), %ax
204:	/* Align 4 */
	adcw	2(%rdi), %ax
206:	/* Align 6 */
	adcw	(%rdi), %ax

222:	adcl	$0, %eax
	subl	%ecx, %esi /* %rcx is 8 - alignment */
	addq	%rcx, %rdi
200:
	/* Fall through */

#endif /* CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS */

	/* Check length */
10:	cmpl	$8, %esi
	jg	30f
	jl	20f

	/* Exactly 8 bytes length */
	addl	(%rdi), %eax
	adcl	4(%rdi), %eax
	RETURN

	/* Less than 8 bytes length */
20:	clc
	jmpq *branch_tbl_len(, %rsi, 8)

	/* Greater than 8 bytes length. Determine number of quads (n). Sum
	 * over first n % 16 quads
	 */
30:	movl	%esi, %ecx
	shrl	$3, %ecx
	andl	$0xf, %ecx
	negq	%rcx
	lea	40f(, %rcx, 4), %r11
	clc
	jmp	*%r11

.align 8
	adcq	14*8(%rdi),%rax
	adcq	13*8(%rdi),%rax
	adcq	12*8(%rdi),%rax
	adcq	11*8(%rdi),%rax
	adcq	10*8(%rdi),%rax
	adcq	9*8(%rdi),%rax
	adcq	8*8(%rdi),%rax
	adcq	7*8(%rdi),%rax
	adcq	6*8(%rdi),%rax
	adcq	5*8(%rdi),%rax
	adcq	4*8(%rdi),%rax
	adcq	3*8(%rdi),%rax
	adcq	2*8(%rdi),%rax
	adcq	1*8(%rdi),%rax
	adcq 	0*8(%rdi),%rax
	nop
40:	/* #quads % 16 jump table base */

	adcq	$0, %rax
	shlq	$3, %rcx
	subq	%rcx, %rdi /* %rcx is already negative length */

	/* Now determine number of blocks of 8 quads. Sum 128 bytes at a time
	 * using unrolled loop.
	 */
	movl	%esi, %ecx
	shrl	$7, %ecx
	jz	60f
	clc

	/* Main loop */
50:	adcq	0*8(%rdi),%rax
	adcq	1*8(%rdi),%rax
	adcq	2*8(%rdi),%rax
	adcq	3*8(%rdi),%rax
	adcq	4*8(%rdi),%rax
	adcq	5*8(%rdi),%rax
	adcq	6*8(%rdi),%rax
	adcq	7*8(%rdi),%rax
	adcq	8*8(%rdi),%rax
	adcq	9*8(%rdi),%rax
	adcq	10*8(%rdi),%rax
	adcq	11*8(%rdi),%rax
	adcq	12*8(%rdi),%rax
	adcq	13*8(%rdi),%rax
	adcq	14*8(%rdi),%rax
	adcq	15*8(%rdi),%rax
	lea	128(%rdi), %rdi
	loop	50b

	adcq	$0, %rax

	/* Handle remaining length which is <= 8 bytes */
60:	andl	$0x7, %esi

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
	RETURN
106:	/* Length 6 */
	adcw	4(%rdi), %ax
104:	/* Length 4, optimized  for double word access*/
	adcl	(%rdi), %eax
	RETURN
102:	/* Length 2 */
	adcw	(%rdi), %ax
100:	/* Length 0 */
	RETURN

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

#ifndef	CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
.L_branch_tbl_align:
	.quad	200b
	.quad	201b
	.quad	202b
	.quad	203b
	.quad	204b
	.quad	205b
	.quad	206b
	.quad	207b
#endif

