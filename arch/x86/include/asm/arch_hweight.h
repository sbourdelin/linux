#ifndef _ASM_X86_HWEIGHT_H
#define _ASM_X86_HWEIGHT_H

#include <asm/cpufeatures.h>
#include <asm/static_cpu_has.h>

static __always_inline unsigned int __arch_hweight32(unsigned int w)
{
	unsigned int res;

	if (likely(static_cpu_has(X86_FEATURE_POPCNT))) {
		asm volatile("popcnt %[w], %[res]" : [res] "=r" (res) : [w] "r" (w));

		return res;
	}
	return __sw_hweight32(w);
}

static inline unsigned int __arch_hweight16(unsigned int w)
{
	return __arch_hweight32(w & 0xffff);
}

static inline unsigned int __arch_hweight8(unsigned int w)
{
	return __arch_hweight32(w & 0xff);
}

#ifdef CONFIG_X86_32
static inline unsigned long __arch_hweight64(__u64 w)
{
	return  __arch_hweight32((u32)w) +
		__arch_hweight32((u32)(w >> 32));
}
#else
static __always_inline unsigned long __arch_hweight64(__u64 w)
{
	unsigned long res;

	if (likely(static_cpu_has(X86_FEATURE_POPCNT))) {
		asm volatile("popcnt %[w], %[res]" : [res] "=r" (res) : [w] "r" (w));

		return res;
	}
	return __sw_hweight64(w);
}
#endif /* CONFIG_X86_32 */

#endif
