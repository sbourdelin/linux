#ifndef _ASM_GENERIC_BITOPS_POPC_PARITY_H_
#define _ASM_GENERIC_BITOPS_POPC_PARITY_H_

#include <asm/types.h>

static inline unsigned int __arch_parity32(unsigned int w)
{
	return __builtin_popcount(w) & 1;
}

static inline unsigned int __arch_parity16(unsigned int w)
{
	return __arch_parity32(w & 0xffff);
}

static inline unsigned int __arch_parity8(unsigned int w)
{
	return __arch_parity32(w & 0xff);
}

static inline unsigned int __arch_parity4(unsigned int w)
{
	return __arch_parity32(w & 0xf);
}

static inline unsigned int __arch_parity64(__u64 w)
{
	return (unsigned int)__builtin_popcountll(w) & 1;
}

#endif

