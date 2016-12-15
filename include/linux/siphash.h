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

#define SIPHASH_ALIGNMENT 8

typedef u64 siphash_key_t[2];

u64 siphash(const void *data, size_t len, const siphash_key_t key);

#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
static inline u64 siphash_unaligned(const void *data, size_t len,
				    const siphash_key_t key)
{
	return siphash(data, len, key);
}
#else
u64 siphash_unaligned(const void *data, size_t len, const siphash_key_t key);
#endif

u64 siphash_1u64(const u64 a, const siphash_key_t key);
u64 siphash_2u64(const u64 a, const u64 b, const siphash_key_t key);
u64 siphash_3u64(const u64 a, const u64 b, const u64 c,
		 const siphash_key_t key);
u64 siphash_4u64(const u64 a, const u64 b, const u64 c, const u64 d,
		 const siphash_key_t key);

static inline u64 siphash_2u32(const u32 a, const u32 b, const siphash_key_t key)
{
	return siphash_1u64((u64)b << 32 | a, key);
}

static inline u64 siphash_4u32(const u32 a, const u32 b, const u32 c, const u32 d,
			       const siphash_key_t key)
{
	return siphash_2u64((u64)b << 32 | a, (u64)d << 32 | c, key);
}

static inline u64 siphash_6u32(const u32 a, const u32 b, const u32 c, const u32 d,
			       const u32 e, const u32 f, const siphash_key_t key)
{
	return siphash_3u64((u64)b << 32 | a, (u64)d << 32 | c, (u64)f << 32 | e,
			    key);
}

static inline u64 siphash_8u32(const u32 a, const u32 b, const u32 c, const u32 d,
			       const u32 e, const u32 f, const u32 g, const u32 h,
			       const siphash_key_t key)
{
	return siphash_4u64((u64)b << 32 | a, (u64)d << 32 | c, (u64)f << 32 | e,
			    (u64)h << 32 | g, key);
}

#endif /* _LINUX_SIPHASH_H */
