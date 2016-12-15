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

#endif /* _LINUX_SIPHASH_H */
