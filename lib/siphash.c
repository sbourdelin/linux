/* Copyright (C) 2015-2016 Jason A. Donenfeld <Jason@zx2c4.com>
 * Copyright (C) 2012-2014 Jean-Philippe Aumasson <jeanphilippe.aumasson@gmail.com>
 * Copyright (C) 2012-2014 Daniel J. Bernstein <djb@cr.yp.to>
 *
 * This file is provided under a dual BSD/GPLv2 license.
 *
 * SipHash: a fast short-input PRF
 * https://131002.net/siphash/
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

static inline u16 le16_to_cpuvp(const void *p)
{
	return le16_to_cpup(p);
}
static inline u32 le32_to_cpuvp(const void *p)
{
	return le32_to_cpup(p);
}
static inline u64 le64_to_cpuvp(const void *p)
{
	return le64_to_cpup(p);
}

/**
 * siphash24 - compute 64-bit siphash24 PRF value
 * @data: buffer to hash, must be aligned to SIPHASH24_ALIGNMENT
 * @size: size of @data
 * @key: key buffer of size SIPHASH24_KEY_LEN, must be aligned to SIPHASH24_ALIGNMENT
 */
u64 siphash24(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN])
{
	u64 v0 = 0x736f6d6570736575ULL;
	u64 v1 = 0x646f72616e646f6dULL;
	u64 v2 = 0x6c7967656e657261ULL;
	u64 v3 = 0x7465646279746573ULL;
	u64 b = ((u64)len) << 56;
	u64 k0 = le64_to_cpuvp(key);
	u64 k1 = le64_to_cpuvp(key + sizeof(u64));
	u64 m;
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	v3 ^= k1;
	v2 ^= k0;
	v1 ^= k1;
	v0 ^= k0;
	for (; data != end; data += sizeof(u64)) {
		m = le64_to_cpuvp(data);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
#if defined(CONFIG_DCACHE_WORD_ACCESS) && BITS_PER_LONG == 64
	if (left)
		b |= le64_to_cpu((__force __le64)(load_unaligned_zeropad(data) & bytemask_from_count(left)));
#else
	switch (left) {
	case 7: b |= ((u64)data[6]) << 48;
	case 6: b |= ((u64)data[5]) << 40;
	case 5: b |= ((u64)data[4]) << 32;
	case 4: b |= le32_to_cpuvp(data); break;
	case 3: b |= ((u64)data[2]) << 16;
	case 2: b |= le16_to_cpuvp(data); break;
	case 1: b |= data[0];
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
EXPORT_SYMBOL(siphash24);

#ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
/**
 * siphash24 - compute 64-bit siphash24 PRF value, without alignment requirements
 * @data: buffer to hash
 * @size: size of @data
 * @key: key buffer of size SIPHASH24_KEY_LEN
 */
u64 siphash24_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN])
{
	u64 v0 = 0x736f6d6570736575ULL;
	u64 v1 = 0x646f72616e646f6dULL;
	u64 v2 = 0x6c7967656e657261ULL;
	u64 v3 = 0x7465646279746573ULL;
	u64 b = ((u64)len) << 56;
	u64 k0 = get_unaligned_le64(key);
	u64 k1 = get_unaligned_le64(key + sizeof(u64));
	u64 m;
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	v3 ^= k1;
	v2 ^= k0;
	v1 ^= k1;
	v0 ^= k0;
	for (; data != end; data += sizeof(u64)) {
		m = get_unaligned_le64(data);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
#if defined(CONFIG_DCACHE_WORD_ACCESS) && BITS_PER_LONG == 64
	if (left)
		b |= le64_to_cpu((__force __le64)(load_unaligned_zeropad(data) & bytemask_from_count(left)));
#else
	switch (left) {
	case 7: b |= ((u64)data[6]) << 48;
	case 6: b |= ((u64)data[5]) << 40;
	case 5: b |= ((u64)data[4]) << 32;
	case 4: b |= get_unaligned_le32(data); break;
	case 3: b |= ((u64)data[2]) << 16;
	case 2: b |= get_unaligned_le16(data); break;
	case 1: b |= data[0];
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
EXPORT_SYMBOL(siphash24_unaligned);
#endif
