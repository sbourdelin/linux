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

#define SIPROUND \
	do { \
	v0 += v1; v1 = rol64(v1, 13); v1 ^= v0; v0 = rol64(v0, 32); \
	v2 += v3; v3 = rol64(v3, 16); v3 ^= v2; \
	v0 += v3; v3 = rol64(v3, 21); v3 ^= v0; \
	v2 += v1; v1 = rol64(v1, 17); v1 ^= v2; v2 = rol64(v2, 32); \
	} while(0)

#define PREAMBLE(len) \
	u64 v0 = 0x736f6d6570736575ULL; \
	u64 v1 = 0x646f72616e646f6dULL; \
	u64 v2 = 0x6c7967656e657261ULL; \
	u64 v3 = 0x7465646279746573ULL; \
	u64 b = ((u64)len) << 56; \
	u64 k0 = le64_to_cpuvp(key); \
	u64 k1 = le64_to_cpuvp(key + sizeof(u64)); \
	v3 ^= k1; \
	v2 ^= k0; \
	v1 ^= k1; \
	v0 ^= k0;

#define POSTAMBLE \
	v3 ^= b; \
	SIPROUND; \
	SIPROUND; \
	v0 ^= b; \
	v2 ^= 0xff; \
	SIPROUND; \
	SIPROUND; \
	SIPROUND; \
	SIPROUND; \
	return (v0 ^ v1) ^ (v2 ^ v3);

/**
 * siphash - compute 64-bit siphash PRF value
 * @data: buffer to hash, must be aligned to SIPHASH_ALIGNMENT
 * @size: size of @data
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash(const u8 *data, size_t len, const u8 key[SIPHASH_KEY_LEN])
{
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	u64 m;
	PREAMBLE(len)
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
	POSTAMBLE
}
EXPORT_SYMBOL(siphash);

#ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
/**
 * siphash - compute 64-bit siphash PRF value, without alignment requirements
 * @data: buffer to hash
 * @size: size of @data
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH_KEY_LEN])
{
	const u8 *end = data + len - (len % sizeof(u64));
	const u8 left = len & (sizeof(u64) - 1);
	u64 m;
	PREAMBLE(len)
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
	POSTAMBLE
}
EXPORT_SYMBOL(siphash_unaligned);
#endif

/**
 * siphash_1qword - compute 64-bit siphash PRF value of 1 quad-word
 * @first: first quadword
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash_1qword(const u64 first, const u8 key[SIPHASH_KEY_LEN])
{
	PREAMBLE(8)
	v3 ^= first;
	SIPROUND;
	SIPROUND;
	v0 ^= first;
	POSTAMBLE
}
EXPORT_SYMBOL(siphash_1qword);

/**
 * siphash_2qwords - compute 64-bit siphash PRF value of 2 quad-words
 * @first: first quadword
 * @second: second quadword
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash_2qwords(const u64 first, const u64 second, const u8 key[SIPHASH_KEY_LEN])
{
	PREAMBLE(16)
	v3 ^= first;
	SIPROUND;
	SIPROUND;
	v0 ^= first;
	v3 ^= second;
	SIPROUND;
	SIPROUND;
	v0 ^= second;
	POSTAMBLE
}
EXPORT_SYMBOL(siphash_2qwords);

/**
 * siphash_3qwords - compute 64-bit siphash PRF value of 3 quad-words
 * @first: first quadword
 * @second: second quadword
 * @third: third quadword
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash_3qwords(const u64 first, const u64 second, const u64 third, const u8 key[SIPHASH_KEY_LEN])
{
	PREAMBLE(24)
	v3 ^= first;
	SIPROUND;
	SIPROUND;
	v0 ^= first;
	v3 ^= second;
	SIPROUND;
	SIPROUND;
	v0 ^= second;
	v3 ^= third;
	SIPROUND;
	SIPROUND;
	v0 ^= third;
	POSTAMBLE
}
EXPORT_SYMBOL(siphash_3qwords);

/**
 * siphash_4qwords - compute 64-bit siphash PRF value of 4 quad-words
 * @first: first quadword
 * @second: second quadword
 * @third: third quadword
 * @forth: forth quadword
 * @key: key buffer of size SIPHASH_KEY_LEN, must be aligned to SIPHASH_ALIGNMENT
 */
u64 siphash_4qwords(const u64 first, const u64 second, const u64 third, const u64 forth, const u8 key[SIPHASH_KEY_LEN])
{
	PREAMBLE(32)
	v3 ^= first;
	SIPROUND;
	SIPROUND;
	v0 ^= first;
	v3 ^= second;
	SIPROUND;
	SIPROUND;
	v0 ^= second;
	v3 ^= third;
	SIPROUND;
	SIPROUND;
	v0 ^= third;
	v3 ^= forth;
	SIPROUND;
	SIPROUND;
	v0 ^= forth;
	POSTAMBLE
}
EXPORT_SYMBOL(siphash_4qwords);
