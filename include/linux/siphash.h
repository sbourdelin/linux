/* Copyright (C) 2016 Jason A. Donenfeld <Jason@zx2c4.com>
 *
 * This file is provided under a dual BSD/GPLv2 license.
 *
 * SipHash: a fast short-input PRF
 * https://131002.net/siphash/
 */

#ifndef _LINUX_SIPHASH_H
#define _LINUX_SIPHASH_H

#include <linux/types.h>

enum siphash_lengths {
	SIPHASH24_KEY_LEN = 16,
	SIPHASH24_ALIGNMENT = 8
};

u64 siphash24(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN]);

#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
static inline u64 siphash24_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN])
{
	return siphash24(data, len, key);
}
#else
u64 siphash24_unaligned(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN]);
#endif

#endif /* _LINUX_SIPHASH_H */
