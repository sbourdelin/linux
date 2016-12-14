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
	SIPHASH24_KEY_LEN = 16
};

u64 siphash24(const u8 *data, size_t len, const u8 key[SIPHASH24_KEY_LEN]);

static inline u64 siphash24_1word(const u32 a, const u8 key[SIPHASH24_KEY_LEN])
{
	return siphash24((u8 *)&a, sizeof(a), key);
}

static inline u64 siphash24_2words(const u32 a, const u32 b, const u8 key[SIPHASH24_KEY_LEN])
{
	const struct {
		u32 a;
		u32 b;
	} __packed combined = {
		.a = a,
		.b = b
	};

	return siphash24((const u8 *)&combined, sizeof(combined), key);
}

static inline u64 siphash24_3words(const u32 a, const u32 b, const u32 c, const u8 key[SIPHASH24_KEY_LEN])
{
	const struct {
		u32 a;
		u32 b;
		u32 c;
	} __packed combined = {
		.a = a,
		.b = b,
		.c = c
	};

	return siphash24((const u8 *)&combined, sizeof(combined), key);
}

#endif /* _LINUX_SIPHASH_H */
