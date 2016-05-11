#ifndef _ASM_X86_PARITY_H
#define _ASM_X86_PARITY_H

#include <asm/cpufeatures.h>

/*
 * the generic version use the Parity Flag directly
 *
 * Parity flag - Set if the least-significant byte of the
 *               result contains an even number of 1 bits;
 *               cleared otherwise.
 */

static inline unsigned int __arch_parity4(unsigned int w)
{
	unsigned int res = 0;

	asm("test $0xf, %1; setpo %b0"
		: "+q" (res)
		: "r" (w)
		: "cc");

	return res;
}

static inline unsigned int __arch_parity8(unsigned int w)
{
	unsigned int res = 0;

	asm("test %1, %1; setpo %b0"
		: "+q" (res)
		: "r" (w)
		: "cc");

	return res;
}

static inline unsigned int __arch_parity16(unsigned int w)
{
	unsigned int res = 0;

	asm("xor %h1, %b1; setpo %b0"
		: "+q" (res), "+q" (w)
		: : "cc");

	return res;
}

#ifdef CONFIG_64BIT
/* popcnt %eax, %eax -- redundant REX prefix for alignment */
#define POPCNT32 ".byte 0xf3,0x40,0x0f,0xb8,0xc0"
/* popcnt %rax, %rax */
#define POPCNT64 ".byte 0xf3,0x48,0x0f,0xb8,0xc0"
#else
/* popcnt %eax, %eax */
#define POPCNT32 ".byte 0xf3,0x0f,0xb8,0xc0"
#endif

static __always_inline unsigned int __arch_parity32(unsigned int w)
{
	unsigned int res;
	unsigned int tmp;

	asm(ALTERNATIVE(
		"	mov	%%eax, %1	\n"
		"	shr	$16, %%eax	\n"
		"	xor	%1, %%eax	\n"
		"	xor	%%ah, %%al	\n"
		"	mov	$0, %%eax	\n"
		"	setpo	%%al	\n",
		POPCNT32 "			\n"
		"	and	$1, %%eax	\n",
		X86_FEATURE_POPCNT)
		: "=a" (res), "=&r" (tmp)
		: "a" (w)
		: "cc");

	return res;
}

#ifdef CONFIG_X86_32
static inline unsigned int __arch_parity64(__u64 w)
{
	return __arch_parity32((unsigned int)(w >> 32) ^ (unsigned int)w);
}
#else
static __always_inline unsigned int __arch_parity64(__u64 w)
{
	unsigned int res;
	__u64 tmp;

	asm(ALTERNATIVE(
		"	mov	%%rax, %1	\n"
		"	shr	$32, %%rax	\n"
		"	xor	%k1, %%eax	\n"
		"	mov	%%eax, %k1	\n"
		"	shr	$16, %%eax	\n"
		"	xor	%k1, %%eax 	\n"
		"	xor	%%ah, %%al	\n"
		"	mov	$0, %%eax	\n"
		"	setpo	%%al	\n",
		POPCNT64 "			\n"
		"	and	$1, %%eax	\n",
		X86_FEATURE_POPCNT)
		: "=a" (res), "=&r" (tmp)
		: "a" (w)
		: "cc");

	return res;
}
#endif /* CONFIG_X86_32 */

#undef POPCNT32
#undef POPCNT64

#endif

