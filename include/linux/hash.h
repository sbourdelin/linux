#ifndef _LINUX_HASH_H
#define _LINUX_HASH_H
/* Fast hashing routine for ints,  longs and pointers.
   (C) 2002 Nadia Yvette Chambers, IBM */

/*
 * Knuth recommends primes in approximately golden ratio to the maximum
 * integer representable by a machine word for multiplicative hashing.
 * Chuck Lever verified the effectiveness of this technique:
 * http://www.citi.umich.edu/techreports/reports/citi-tr-00-1.pdf
 *
 * These primes are chosen to be bit-sparse, that is operations on
 * them can use shifts and additions instead of multiplications for
 * machines where multiplications are slow.
 */

#include <asm/types.h>
#include <linux/compiler.h>

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL
/*  2^63 + 2^61 - 2^57 + 2^54 - 2^51 - 2^18 + 1 */
#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001UL

#if BITS_PER_LONG == 32
#define GOLDEN_RATIO_PRIME GOLDEN_RATIO_PRIME_32
#define __hash_long(val) __hash_32(val)
#define hash_long(val, bits) hash_32(val, bits)
#elif BITS_PER_LONG == 64
#define __hash_long(val) __hash_64(val)
#define hash_long(val, bits) hash_64(val, bits)
#define GOLDEN_RATIO_PRIME GOLDEN_RATIO_PRIME_64
#else
#error Wordsize not 32 or 64
#endif

static __always_inline u64 __hash_64(u64 val)
{
	u64 hash = val;

#if defined(CONFIG_ARCH_HAS_FAST_MULTIPLIER) && BITS_PER_LONG == 64
	hash = hash * GOLDEN_RATIO_PRIME_64;
#else
	/*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
	u64 n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;
#endif

	return hash;
}

static __always_inline u64 hash_64(u64 val, unsigned bits)
{
	/* High bits are more random, so use them. */
	return __hash_64(val) >> (64 - bits);
}

static inline u32 __hash_32(u32 val)
{
	/* On some cpus multiply is faster, on others gcc will do shifts */
	return val * GOLDEN_RATIO_PRIME_32;
}

static inline u32 hash_32(u32 val, unsigned bits)
{
	/* High bits are more random, so use them. */
	return __hash_32(val) >> (32 - bits);
}

static inline u32 hash_ptr(const void *ptr, unsigned bits)
{
	return hash_long((unsigned long)ptr, bits);
}

static inline u32 hash32_ptr(const void *ptr)
{
	unsigned long val = (unsigned long)ptr;

#if BITS_PER_LONG == 64
	val ^= (val >> 32);
#endif
	return (u32)val;
}

#endif /* _LINUX_HASH_H */
