/*
 * Copyright (C) 2003 Bernardo Innocenti <bernie@develer.com>
 *
 * Based on former do_div() implementation from asm-parisc/div64.h:
 *	Copyright (C) 1999 Hewlett-Packard Co
 *	Copyright (C) 1999 David Mosberger-Tang <davidm@hpl.hp.com>
 *
 *
 * Generic C version of 64bit/32bit division and modulo, with
 * 64bit result and 32bit remainder.
 *
 * The fast case for (n>>32 == 0) is handled inline by do_div(). 
 *
 * Code generated for this function might be very inefficient
 * for some CPUs. __div64_32() can be overridden by linking arch-specific
 * assembly versions such as arch/ppc/lib/div64.S and arch/sh/lib/div64.S.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/math64.h>

/* Not needed on 64bit architectures */
#if BITS_PER_LONG == 32

/* our own fls implementation to make sure constant propagation is fine */
inline int __div64_fls(int bits)
{
	unsigned int __left = bits, __nr = 0;

	if (__left & 0xffff0000)
		__nr += 16, __left >>= 16;

	if (__left & 0x0000ff00)
		__nr +=  8, __left >>=  8;

	if (__left & 0x000000f0)
		__nr +=  4, __left >>=  4;

	if (__left & 0x0000000c)
		__nr +=  2, __left >>=  2;

	if (__left & 0x00000002)
		__nr +=  1;

	return __nr;
}

/*
 * If the divisor happens to be constant, we determine the appropriate
 * inverse at compile time to turn the division into a few inline
 * multiplications instead which is much faster.
 */
uint32_t __attribute__((weak)) __div64_32(uint64_t *n, uint32_t base)
{
	unsigned int __r, __b = base;

	if (!__builtin_constant_p(__b) || __b == 0) {
		/* non-constant divisor (or zero): slow path */
		uint64_t rem = *n;
		uint64_t b = base;
		uint64_t res, d = 1;
		uint32_t high = rem >> 32;

		/* Reduce the thing a bit first */
		res = 0;
		if (high >= base) {
			high /= base;
			res = (uint64_t) high << 32;
			rem -= (uint64_t) (high*base) << 32;
		}

		while ((int64_t)b > 0 && b < rem) {
			b = b+b;
			d = d+d;
		}

		do {
			if (rem >= b) {
				rem -= b;
				res += d;
			}
			b >>= 1;
			d >>= 1;
		} while (d);

		*n = res;
		__r = rem;
	} else if ((__b & (__b - 1)) == 0) {
		/*
		 * Trivial: __b is constant and a power of 2
		 * gcc does the right thing with this code.
		 * Even though code is the same as above but
		 * we make it visually as a separate path.
		 * Still only one of these branches will survive
		 * pre-processor stage, so let's leave it here.
		 */
		__r = *n;
		__r &= (__b - 1);
		*n /= __b;
	} else {
		/* Start of preprocessor calculations */

		/*
		 * Multiply by inverse of __b: *n/b = *n*(p/b)/p
		 * We rely on the fact that most of this code gets
		 * optimized away at compile time due to constant
		 * propagation and only a couple inline assembly
		 * instructions should remain. Better avoid any
		 * code construct that might prevent that.
		 */
		unsigned long long __res, __x, __t, __m, __n = *n;
		unsigned int __p;
		/* preserve low part of *n for reminder computation */
		__r = __n;
		/* determine number of bits to represent __b */
		__p = 1 << __div64_fls(__b);
		/* compute __m = ((__p << 64) + __b - 1) / __b */
		__m = (~0ULL / __b) * __p;
		__m += (((~0ULL % __b + 1) * __p) + __b - 1) / __b;
		/* compute __res = __m*(~0ULL/__b*__b-1)/(__p << 64) */
		__x = ~0ULL / __b * __b - 1;
		__res = (__m & 0xffffffff) * (__x & 0xffffffff);
		__res >>= 32;
		__res += (__m & 0xffffffff) * (__x >> 32);
		__t = __res;
		__res += (__x & 0xffffffff) * (__m >> 32);
		__t = (__res < __t) ? (1ULL << 32) : 0;
		__res = (__res >> 32) + __t;
		__res += (__m >> 32) * (__x >> 32);
		__res /= __p;
		/* End of preprocessor calculations */

		/* Start of run-time calculations */
		__res = (unsigned int)__m * (unsigned int)__n;
		__res >>= 32;
		__res += (unsigned int)__m * (__n >> 32);
		__t = __res;
		__res += (unsigned int)__n * (__m >> 32);
		__t = (__res < __t) ? (1ULL << 32) : 0;
		__res = (__res >> 32) + __t;
		__res += (__m >> 32) * (__n >> 32);
		__res /= __p;

		/*
		 * The reminder can be computed with 32-bit regs
		 * only, and gcc is good at that.
		 */
		{
			unsigned int __res0 = __res;
			unsigned int __b0 = __b;

			__r -= __res0 * __b0;
		}
		/* End of run-time calculations */

		*n = __res;
	}
	return __r;
}

EXPORT_SYMBOL(__div64_32);

#ifndef div_s64_rem
s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
	u64 quotient;

	if (dividend < 0) {
		quotient = div_u64_rem(-dividend, abs(divisor), (u32 *)remainder);
		*remainder = -*remainder;
		if (divisor > 0)
			quotient = -quotient;
	} else {
		quotient = div_u64_rem(dividend, abs(divisor), (u32 *)remainder);
		if (divisor < 0)
			quotient = -quotient;
	}
	return quotient;
}
EXPORT_SYMBOL(div_s64_rem);
#endif

/**
 * div64_u64_rem - unsigned 64bit divide with 64bit divisor and remainder
 * @dividend:	64bit dividend
 * @divisor:	64bit divisor
 * @remainder:  64bit remainder
 *
 * This implementation is a comparable to algorithm used by div64_u64.
 * But this operation, which includes math for calculating the remainder,
 * is kept distinct to avoid slowing down the div64_u64 operation on 32bit
 * systems.
 */
#ifndef div64_u64_rem
u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	u32 high = divisor >> 32;
	u64 quot;

	if (high == 0) {
		u32 rem32;
		quot = div_u64_rem(dividend, divisor, &rem32);
		*remainder = rem32;
	} else {
		int n = 1 + fls(high);
		quot = div_u64(dividend >> n, divisor >> n);

		if (quot != 0)
			quot--;

		*remainder = dividend - quot * divisor;
		if (*remainder >= divisor) {
			quot++;
			*remainder -= divisor;
		}
	}

	return quot;
}
EXPORT_SYMBOL(div64_u64_rem);
#endif

/**
 * div64_u64 - unsigned 64bit divide with 64bit divisor
 * @dividend:	64bit dividend
 * @divisor:	64bit divisor
 *
 * This implementation is a modified version of the algorithm proposed
 * by the book 'Hacker's Delight'.  The original source and full proof
 * can be found here and is available for use without restriction.
 *
 * 'http://www.hackersdelight.org/hdcodetxt/divDouble.c.txt'
 */
#ifndef div64_u64
u64 div64_u64(u64 dividend, u64 divisor)
{
	u32 high = divisor >> 32;
	u64 quot;

	if (high == 0) {
		quot = div_u64(dividend, divisor);
	} else {
		int n = 1 + fls(high);
		quot = div_u64(dividend >> n, divisor >> n);

		if (quot != 0)
			quot--;
		if ((dividend - quot * divisor) >= divisor)
			quot++;
	}

	return quot;
}
EXPORT_SYMBOL(div64_u64);
#endif

/**
 * div64_s64 - signed 64bit divide with 64bit divisor
 * @dividend:	64bit dividend
 * @divisor:	64bit divisor
 */
#ifndef div64_s64
s64 div64_s64(s64 dividend, s64 divisor)
{
	s64 quot, t;

	quot = div64_u64(abs64(dividend), abs64(divisor));
	t = (dividend ^ divisor) >> 63;

	return (quot ^ t) - t;
}
EXPORT_SYMBOL(div64_s64);
#endif

#endif /* BITS_PER_LONG == 32 */

/*
 * Iterative div/mod for use when dividend is not expected to be much
 * bigger than divisor.
 */
u32 iter_div_u64_rem(u64 dividend, u32 divisor, u64 *remainder)
{
	return __iter_div_u64_rem(dividend, divisor, remainder);
}
EXPORT_SYMBOL(iter_div_u64_rem);
