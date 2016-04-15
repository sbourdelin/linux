/*
 * Copyright 2004-2009 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef _BLACKFIN_BITOPS_H
#define _BLACKFIN_BITOPS_H

#include <linux/compiler.h>

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

/*
 * hweightN: returns the hamming weight (i.e. the number
 * of bits set) of a N-bit word
 */

static inline unsigned int __arch_hweight32(unsigned int w)
{
	unsigned int res;

	__asm__ ("%0.l = ONES %1;"
		"%0 = %0.l (Z);"
		: "=d" (res) : "d" (w));
	return res;
}

static inline unsigned int __arch_hweight64(__u64 w)
{
	return __arch_hweight32((unsigned int)(w >> 32)) +
	       __arch_hweight32((unsigned int)w);
}

static inline unsigned int __arch_hweight16(unsigned int w)
{
	return __arch_hweight32(w & 0xffff);
}

static inline unsigned int __arch_hweight8(unsigned int w)
{
	return __arch_hweight32(w & 0xff);
}

#include <asm-generic/bitops/const_hweight.h>

/**
 * ffz - find the first zero bit in a long word
 * @x: The long word to find the bit in
 *
 * Returns the bit-number (0..31) of the first (least significant) zero bit.
 * Undefined if no zero exists, so code should check against ~0UL first...
 */
static inline unsigned long ffz(unsigned long x)
{
	return hweight32(x & (~x - 1));
}

/**
 * ffs - find first bit set
 * @x: the word to search
 *
 * This is defined the same way as
 * the libc and compiler builtin ffs routines, therefore
 * differs in spirit from the above ffz (man ffs).
 */
static inline int ffs(int x)
{
	if (!x)
		return 0;
	return hweight32(x ^ (x - 1));
}

/**
 * __ffs - find first bit in word.
 * @x: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static inline unsigned long __ffs(unsigned long x)
{
	return hweight32(~x & (x - 1));
}

/*
 * Find the last (most significant) bit set.  Returns 0 for x==0 and
 * bits are numbered from 1..32 (e.g., fls(9) == 4).
 */
static inline int fls(int x)
{
	if (!x)
		return 0;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return hweight32(x);
}

/*
 * Find the last (most significant) bit set.  Undefined for x==0.
 * Bits are numbered from 0..31 (e.g., __fls(9) == 3).
 */
static inline unsigned long __fls(unsigned long x)
{
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return hweight32(x) - 1;
}

#include <asm-generic/bitops/fls64.h>
#include <asm-generic/bitops/find.h>

#include <asm-generic/bitops/sched.h>
#include <asm-generic/bitops/lock.h>

#include <asm-generic/bitops/ext2-atomic.h>

#include <asm/barrier.h>

#ifndef CONFIG_SMP
#include <linux/irqflags.h>
/*
 * clear_bit may not imply a memory barrier
 */
#include <asm-generic/bitops/atomic.h>
#include <asm-generic/bitops/non-atomic.h>
#else

#include <asm/byteorder.h>	/* swab32 */
#include <linux/linkage.h>

asmlinkage int __raw_bit_set_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_clear_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_toggle_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_test_set_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_test_clear_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_test_toggle_asm(volatile unsigned long *addr, int nr);

asmlinkage int __raw_bit_test_asm(const volatile unsigned long *addr, int nr);

static inline void set_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	__raw_bit_set_asm(a, nr & 0x1f);
}

static inline void clear_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	__raw_bit_clear_asm(a, nr & 0x1f);
}

static inline void change_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	__raw_bit_toggle_asm(a, nr & 0x1f);
}

static inline int test_bit(int nr, const volatile unsigned long *addr)
{
	volatile const unsigned long *a = addr + (nr >> 5);
	return __raw_bit_test_asm(a, nr & 0x1f) != 0;
}

static inline int test_and_set_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	return __raw_bit_test_set_asm(a, nr & 0x1f);
}

static inline int test_and_clear_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	return __raw_bit_test_clear_asm(a, nr & 0x1f);
}

static inline int test_and_change_bit(int nr, volatile unsigned long *addr)
{
	volatile unsigned long *a = addr + (nr >> 5);
	return __raw_bit_test_toggle_asm(a, nr & 0x1f);
}

#define test_bit __skip_test_bit
#include <asm-generic/bitops/non-atomic.h>
#undef test_bit

#endif /* CONFIG_SMP */

/* Needs to be after test_bit and friends */
#include <asm-generic/bitops/le.h>

#endif				/* _BLACKFIN_BITOPS_H */
