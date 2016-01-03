/* Copyright 2016 Tom Herbert <tom@herbertland.com>
 *
 * Checksum partial calculation
 *
 * __wsum csum_partial(const void *buff, int len, __wsum sum)
 * computes the checksum of a memory block at buff, length len,
 * and adds in "sum" (32-bit)
 *
 * returns a 32-bit number suitable for feeding into itself
 * or csum_tcpudp_magic
 *
 * Register usage
 *  %rdi: argument 1, buff
 *  %rsi: argument 2, length
 *  %rdx: argument 3, add in value
 *  %rcx: counter and tmp
 *  %r10: buffer alignment
 *  %r11: tmp
 *
 * Basic algorithm:
 *   1) Handle buffer that is not aligned to 8 bytes
 *   2) Sum 8 bytes at a time using adcq (unroll main loop
 *      to do 64 bytes at a time)
 *   3) Sum remaining length (less than 8 bytes)
 *
 * If buffer is not aligned to 8 bytes and and length is less than
 * or equal to 8 - alignment (whole buffer is in one quad), then
 * treat this as a special case.
 */

#include <linux/linkage.h>
#include <asm/errno.h>
#include <asm/asm.h>

#define branch_tbl_align	.L_branch_tbl_align
#define branch_tbl_len		.L_branch_tbl_len
#define branch_tbl_small_align	.L_branch_tbl_small_align
#define branch_tbl_small_align_a1 .L_branch_tbl_small_align_a1

ENTRY(csum_partial)
	xorq	%rax, %rax

	/* Determine buffer alignment to 8 bytes */
	movl	%edi, %r10d
	andl	$0x7, %r10d
	jnz	1f

	/* Quick check if length is small */
10:	cmpl	$8, %esi
	jle	20f

	/* Determine number of quads (n). Sum over first n % 8  quads */
	movl	%esi, %ecx
	shrl	$3, %ecx
	andl	$0x7, %ecx
	negq	%rcx
	lea	25f(, %rcx, 4), %r11
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

25:	adcq	$0, %rax
	shlq	$3, %rcx
	subq	%rcx, %rdi /* %rcx is already negative length */

	/* Now determine number of blocks of 8 quads. Sum 64 bytes at a time
	 * using unrolled loop.
	 */
	movl	%esi, %ecx
	shrl	$6, %ecx
	jz	30f
	clc

35:	adcq	0*8(%rdi),%rax
	adcq	1*8(%rdi),%rax
	adcq	2*8(%rdi),%rax
	adcq	3*8(%rdi),%rax
	adcq	4*8(%rdi),%rax
	adcq	5*8(%rdi),%rax
	adcq	6*8(%rdi),%rax
	adcq	7*8(%rdi),%rax
	lea	64(%rdi), %rdi
	loop	35b

	adcq	$0, %rax

30:	andl	$0x7, %esi

	/* Handle remaining length which is <= 8 bytes */
20:	jmpq *branch_tbl_len(, %rsi, 8)

/* Length tabel targets */

101:	/* Length 1 */
	addb	(%rdi), %al
	adcb	$0, %ah
	adcq	$0, %rax
	jmp	50f
103:	/* Length 3 */
	addb	2(%rdi), %al
	adcb	$0, %ah
	adcq	$0, %rax
102:	/* Length 2 */
	addw	(%rdi), %ax
	adcq	$0, %rax
	jmp	50f
105:	/* Length 5 */
	addb	4(%rdi), %al
	adcb	$0, %ah
	adcq	$0, %rax
104:	/* Length 4 */
	movl	(%rdi), %ecx
	addq	%rcx, %rax
	adcq	$0, %rax
	jmp	50f
107:	/* Length 7 */
	addb	6(%rdi), %al
	adcb	$0, %ah
	adcq	$0, %rax
106:	/* Length 6 */
	movl	(%rdi), %ecx
	addq	%rcx, %rax
	adcw	4(%rdi), %ax
	adcq	$0, %rax
	jmp	50f
108:	/* Length 8 */
	addq	(%rdi), %rax
	adcq	$0, %rax
100:	/* Length 0 */

	/* If alignment is odd we need to roll whole sum by 8 bits */
50:	test	$1, %r10d
	jnz	65f

	/* Fold sum to 32 bits and add initial sum value from argument */
60:	movq	%rax, %rcx
	shrq	$32, %rcx
	addl	%ecx, %eax
	adcl	%edx, %eax
	adcl	$0, %eax
	ret

65:	rolq	$8, %rax
	jmp	60b

	/* Process non-zero alignment */
1:	cmpl	$8, %esi
	jl	68f
	jmpq	*branch_tbl_align(, %r10, 8)

	/* Non-zero alignment and length < 8. See if buffer is in one quad */
68:	test	%rsi, %rsi
	je	60b
	movl	$8, %ecx
	subl	%r10d, %ecx
	cmpl	%ecx, %esi
	jle	70f
	jmpq	*branch_tbl_align(, %r10, 8)

/* Alignment table targets */

201:	/* Align 1 */
	movl	3(%rdi), %eax
	addb	(%rdi), %ah
	adcw	1(%rdi),%ax
	adcq	$0, %rax
	subl	$7, %esi
	addq	$7, %rdi
	jmp	10b
202:	/* Align 2 */
	movw	(%rdi), %ax
	addl	2(%rdi), %eax
	adcl	$0, %eax
	subl	$6, %esi
	addq	$6, %rdi
	jmp	10b
203:	/* Align 3 */
	movb	(%rdi), %ah
	addl	1(%rdi),%eax
	adcl	$0, %eax
	subl	$5, %esi
	addq	$5, %rdi
	jmp	10b
204:	/* Align 4 */
	movl	(%rdi), %eax
	subl	$4, %esi
	addq	$4, %rdi
	jmp	10b
205:	/* Align 5 */
	movb	(%rdi), %ah
	addw	1(%rdi),%ax
	adcw	$0, %ax
	subl	$3, %esi
	addq	$3, %rdi
	jmp	10b
206:	/* Align 6 */
	movw	(%rdi), %ax
	subl	$2, %esi
	addq	$2, %rdi
	jmp	10b
207:	/* Align 7 */
	movb	(%rdi), %ah
	subl	$1, %esi
	addq	$1, %rdi
200:	/* Align 0 */
	jmp	10b

	/* Non-zero alignment and buffer is in one quad (len <= 8 - align) */
70:	decl	%esi
	test	$0x1, %r10d
	jnz	75f
	jmpq	*branch_tbl_small_align(, %rsi, 8)

/* Small length with even alignment table targets */

301:	/* Length 1, align is 2,4, or 6 */
	movb	(%rdi), %al
	jmp	60b
302:	/* Length 2, align is 2, 4, or 6 */
	movw	(%rdi), %ax
	jmp	60b
303:	/* Length 3, align is 2 or 4 */
	movb	2(%rdi), %al
	addw	(%rdi), %ax
	adcw	$0, %ax
	jmp	60b
304:	/* Length 4, align is 2 or 4 */
	movw	(%rdi), %ax
	addw	2(%rdi), %ax
	adcw	$0, %ax
	jmp	60b
305:	/* Length 5, align must be 2 */
	movb	4(%rdi), %al
	addw	(%rdi), %ax
	adcw	2(%rdi), %ax
	adcw	$0, %ax
	jmp	60b
306:	/* Length 6, align must be 2 */
	movw	(%rdi), %ax
	addl	2(%rdi), %eax
	adcl	$0, %eax
	jmp	60b

75:	jmp *branch_tbl_small_align_a1(, %rsi, 8)

/* Small length with odd alignement table targets */

401: /* Length 1, align is 1, 3, 5, or 7 */
	movb	(%rdi), %al
	jmp	60b
402: /* Length 2, align is 1, 3, or 5 */
	movb	(%rdi), %al
	movb	1(%rdi), %ah
	jmp	60b
404: /* Length 4, align is 1 or 3 */
	movb	(%rdi), %ah
	movb	3(%rdi), %al
	addw	1(%rdi), %ax
	adcw	$0, %ax
	rolq	$8, %rax
	jmp	60b
405: /* Length 5, align is 1 or 3 */
	adcw	3(%rdi), %ax
403: /* Length 3, align is 1, 3, or 5 */
	adcb	(%rdi), %ah
	adcw	1(%rdi), %ax
	adcw	$0, %ax
	rolq	$8, %rax
	jmp	60b
406: /* Length 6, align must be 1 */
	movb	5(%rdi), %al
	movb	(%rdi), %ah
	addw	1(%rdi), %ax
	adcw	3(%rdi), %ax
	adcl	$0, %eax
	rolq	$8, %rax
	jmp	60b
407: /* Length 7, align must be 1 */
	movb	(%rdi), %ah
	addw	1(%rdi), %ax
	adcl	3(%rdi), %eax
	adcl	$0, %eax
	rolq	$8, %rax
	jmp	60b
ENDPROC(csum_partial)

/* Jump tables */

.section .rodata
.align 64
.L_branch_tbl_align:
	.quad	200b
	.quad	201b
	.quad	202b
	.quad	203b
	.quad	204b
	.quad	205b
	.quad	206b
	.quad	207b

.L_branch_tbl_len:
	.quad	100b
	.quad	101b
	.quad	102b
	.quad	103b
	.quad	104b
	.quad	105b
	.quad	106b
	.quad	107b
	.quad	108b

.L_branch_tbl_small_align:
	.quad	301b
	.quad	302b
	.quad	303b
	.quad	304b
	.quad	305b
	.quad	306b

.L_branch_tbl_small_align_a1:
	.quad	401b
	.quad	402b
	.quad	403b
	.quad	404b
	.quad	405b
	.quad	406b
	.quad	407b
