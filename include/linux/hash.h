#ifndef _LINUX_HASH_H
#define _LINUX_HASH_H
/*
 * Fast hashing routine for ints, longs and pointers.
 * (C) 2002 Nadia Yvette Chambers, IBM
 *
 * These are used for small in-memory hash tables, where speed is a
 * primary concern.  If you want something a little bit stronger, see
 * <linux/jhash.h>, especially functions like jhash_3words().  If your
 * hash table is subject to a hash collision denial of service attack,
 * use something cryptographic.
 *
 * Note that the algorithms used are not guaranteed stable across kernel
 * versions or architectures!  In particular, hash_64() is implemented
 * differently on 32- and 64-bit machines.  Do not let external behavior
 * depend on the hash values.
 *
 * The algorithm used is straight from Knuth: multiply a w-bit word by
 * a suitable large constant, and take the high bits of the w-bit result.
 *
 * Chuck Lever verified the effectiveness of this technique:
 * http://www.citi.umich.edu/techreports/reports/citi-tr-00-1.pdf
 *
 * A good reference is Mikkel Thorup, "High Speed Hashing for
 * Integers and Strings" at http://arxiv.org/abs/1504.06804 and
 * https://www.youtube.com/watch?v=cB85UZKJQTc
 *
 * Because the current algorithm is linear (hash(a+b) = hash(a) + hash(b)),
 * adding or subtracting hash values is just as likely to cause collisions
 * as adding or subtracting the keys themselves.
 */
#include <asm/types.h>
#include <linux/compiler.h>

/*
 * Although a random odd number will do, it turns out that the golden ratio
 * phi = (sqrt(5)-1)/2, or its negative, has particularly nice properties.
 *
 * These are actually the negative, (1 - phi) = (phi^2) = (3 - sqrt(5))/2.
 * (See Knuth vol 3, section 6.4, exercise 9.)
 */
#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

#if BITS_PER_LONG == 64

#define GOLDEN_RATIO_PRIME GOLDEN_RATIO_64	/* Used in fs/inode.c */
#define __hash_long(val) __hash_64(val)
#define hash_long(val, bits) hash_64(val, bits)

static __always_inline u64 __hash_64(u64 val)
{
	return val * GOLDEN_RATIO_64;
}

static __always_inline u64 hash_64(u64 val, unsigned bits)
{
	/* High bits are more random, so use them. */
	return __hash_64(val) >> (64 - bits);
}

#elif BITS_PER_LONG == 32

#define GOLDEN_RATIO_PRIME GOLDEN_RATIO_32
#define __hash_long(val) __hash_32(val)
#define hash_long(val, bits) hash_32(val, bits)

/*
 * Because 64-bit multiplications are very expensive on 32-bit machines,
 * provide a completely separate implementation for them.
 *
 * This is mostly used via the hash_long() and hash_ptr() wrappers,
 * which use hash_32() on 32-bit platforms, but there are some direct
 * users of hash_64() in 32-bit kernels.
 *
 * Note that there is no __hash_64 function at all; that exists
 * only to implement __hash_long().
 *
 * The algorithm is somewhat ad-hoc, but achieves decent mixing.
 */
static __always_inline u32 hash_64(u64 val, unsigned bits)
{
	u32 hash = (uint32)(val >> 32) * GOLDEN_RATIO_32;
	hash += (uint32)val;
	hash *= GOLDEN_RATIO_32;
	return hash >> (32 - bits);
}

#else /* BITS_PER_LONG is something else */
#error Wordsize not 32 or 64
#endif


/*
 * This is the old bastard constant: a low-bit-weight
 * prime close to 2^32 * phi = 0x9E3779B9.
 *
 * The purpose of the low bit weight is to make the shift-and-add
 * code faster on processors like ARMv2 without hardware multiply.
 * The downside is that the high bits of the input are hashed very weakly.
 * In particular, the high 16 bits of input are just shifted up and added,
 * so if you ask for b < 16 bits of output, bits 16..31-b of the input
 * barely affect the output.
 *
 * Annoyingly, GCC compiles this into 6 shifts and adds, which
 * is enough to multiply by the full GOLDEN_RATIO_32 using a
 * cleverer algorithm:
 *
 * unsigned hash_32(unsigned x)
 * {
 * 	unsigned y, z;
 *
 * 	y = (x << 19) + x;
 * 	z = (x << 9) + y;
 * 	x = (x << 23) + z;
 * 	z = (z << 8) + y;
 * 	x = (x << 6) - x;
 * 	return (z << 3) + x;
 * }
 *
 * (Found by Yevgen Voronenko's Hcub algorithm, from
 * http://spiral.ece.cmu.edu/mcm/gen.html)
 *
 * Unfortunately, figuring out which version to compile requires
 * replicating the compiler's logic in Kconfig or the preprocessor.
 */

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

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

/* This really should be called "fold32_ptr"; it barely hashes at all. */
static inline u32 hash32_ptr(const void *ptr)
{
	unsigned long val = (unsigned long)ptr;

#if BITS_PER_LONG == 64
	val ^= (val >> 32);
#endif
	return (u32)val;
}

#endif /* _LINUX_HASH_H */
