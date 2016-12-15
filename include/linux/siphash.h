/* Copyright (C) 2016 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * This file is provided under a dual BSD/GPLv2 license.
 *
 * SipHash: a fast short-input PRF
 * https://131002.net/siphash/
 *
 * This implementation is specifically for SipHash2-4.
 */

#ifndef _LINUX_SIPHASH_H
#define _LINUX_SIPHASH_H

#include <linux/types.h>

#define SIPHASH_KEY_LEN 16
#define SIPHASH_ALIGNMENT 8

u64 siphash(const u8 *data, size_t len, const u8 key[SIPHASH_KEY_LEN]);

#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
static inline u64 siphash_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH_KEY_LEN])
{
	return siphash(data, len, key);
}
#else
u64 siphash_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH_KEY_LEN]);
#endif

u64 siphash_1qword(const u64 a, const u8 key[SIPHASH_KEY_LEN]);
u64 siphash_2qwords(const u64 a, const u64 b, const u8 key[SIPHASH_KEY_LEN]);
u64 siphash_3qwords(const u64 a, const u64 b, const u64 c, const u8 key[SIPHASH_KEY_LEN]);
u64 siphash_4qwords(const u64 a, const u64 b, const u64 c, const u64 d, const u8 key[SIPHASH_KEY_LEN]);

static inline u64 siphash_2dwords(const u32 a, const u32 b, const u8 key[SIPHASH_KEY_LEN])
{
	return siphash_1qword((u64)b << 32 | a, key);
}

static inline u64 siphash_4dwords(const u32 a, const u32 b, const u32 c, const u32 d,
				  const u8 key[SIPHASH_KEY_LEN])
{
	return siphash_2qwords((u64)b << 32 | a, (u64)d << 32 | c, key);
}

static inline u64 siphash_6dwords(const u32 a, const u32 b, const u32 c, const u32 d,
				  const u32 e, const u32 f, const u8 key[SIPHASH_KEY_LEN])
{
	return siphash_3qwords((u64)b << 32 | a, (u64)d << 32 | c, (u64)f << 32 | e,
			       key);
}

static inline u64 siphash_8dwords(const u32 a, const u32 b, const u32 c, const u32 d,
				  const u32 e, const u32 f, const u32 g, const u32 h,
				  const u8 key[SIPHASH_KEY_LEN])
{
	return siphash_4qwords((u64)b << 32 | a, (u64)d << 32 | c, (u64)f << 32 | e,
			       (u64)h << 32 | g, key);
}

#endif /* _LINUX_SIPHASH_H */
