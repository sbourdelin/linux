/* Copyright (C) 2016 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * This file is provided under a dual BSD/GPLv2 license.
 *
 * SipHash: a fast short-input PRF
 * https://131002.net/siphash/
 *
 * This implementation is specifically for SipHash2-4.
 */

#include <linux/siphash.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>

#if defined(CONFIG_DCACHE_WORD_ACCESS) && BITS_PER_LONG == 64
#include <linux/dcache.h>
#include <asm/word-at-a-time.h>
#endif

#define SIPROUND \
	do { \
	v0 += v1; v1 = rol64(v1, 13); v1 ^= v0; v0 = rol64(v0, 32); \
	v2 += v3; v3 = rol64(v3, 16); v3 ^= v2; \
	v0 += v3; v3 = rol64(v3, 21); v3 ^= v0; \
	v2 += v1; v1 = rol64(v1, 17); v1 ^= v2; v2 = rol64(v2, 32); \
	} while(0)

/**
 * siphash - compute 64-bit siphash PRF value
 * @data: buffer to hash, must be aligned to SIPHASH_ALIGNMENT
 * @size: size of @data
 * @key: the siphash key
 */
u64 siphash(const void *data, size_t len, const siphash_key_t key)
{
	u64 v0 = 0x736f6d6570736575ULL;
	u64 v1 = 0x646f72616e646f6dULL;
	u64 v2 = 0x6c7967656e657261ULL;
	u64 v3 = 0x7465646279746573ULL;
	u64 b = ((u64)len) << 56;
	u64 m;
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	v3 ^= key[1];
	v2 ^= key[0];
	v1 ^= key[1];
	v0 ^= key[0];
	for (; data != end; data += sizeof(u64)) {
		m = le64_to_cpup(data);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
#if defined(CONFIG_DCACHE_WORD_ACCESS) && BITS_PER_LONG == 64
	if (left)
		b |= le64_to_cpu((__force __le64)(load_unaligned_zeropad(data) &
						  bytemask_from_count(left)));
#else
	switch (left) {
	case 7: b |= ((u64)end[6]) << 48;
	case 6: b |= ((u64)end[5]) << 40;
	case 5: b |= ((u64)end[4]) << 32;
	case 4: b |= le32_to_cpup(data); break;
	case 3: b |= ((u64)end[2]) << 16;
	case 2: b |= le16_to_cpup(data); break;
	case 1: b |= end[0];
	}
#endif
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return (v0 ^ v1) ^ (v2 ^ v3);
}
EXPORT_SYMBOL(siphash);

#ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
/**
 * siphash - compute 64-bit siphash PRF value, without alignment requirements
 * @data: buffer to hash
 * @size: size of @data
 * @key: the siphash key
 */
u64 siphash_unaligned(const void *data, size_t len, const siphash_key_t key)
{
	u64 v0 = 0x736f6d6570736575ULL;
	u64 v1 = 0x646f72616e646f6dULL;
	u64 v2 = 0x6c7967656e657261ULL;
	u64 v3 = 0x7465646279746573ULL;
	u64 b = ((u64)len) << 56;
	u64 m;
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	v3 ^= key[1];
	v2 ^= key[0];
	v1 ^= key[1];
	v0 ^= key[0];
	for (; data != end; data += sizeof(u64)) {
		m = get_unaligned_le64(data);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
#if defined(CONFIG_DCACHE_WORD_ACCESS) && BITS_PER_LONG == 64
	if (left)
		b |= le64_to_cpu((__force __le64)(load_unaligned_zeropad(data) &
						  bytemask_from_count(left)));
#else
	switch (left) {
	case 7: b |= ((u64)end[6]) << 48;
	case 6: b |= ((u64)end[5]) << 40;
	case 5: b |= ((u64)end[4]) << 32;
	case 4: b |= get_unaligned_le32(end); break;
	case 3: b |= ((u64)end[2]) << 16;
	case 2: b |= get_unaligned_le16(end); break;
	case 1: b |= bytes[0];
	}
#endif
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return (v0 ^ v1) ^ (v2 ^ v3);
}
EXPORT_SYMBOL(siphash_unaligned);
#endif
