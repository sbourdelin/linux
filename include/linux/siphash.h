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

#endif /* _LINUX_SIPHASH_H */
