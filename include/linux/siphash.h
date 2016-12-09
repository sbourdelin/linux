/* Copyright (C) 2016 Jason A. Donenfeld <Jason@zx2c4.com>
 *
 * SipHash: a fast short-input PRF
 * https://131002.net/siphash/
 */

#ifndef _LINUX_SIPHASH_H
#define _LINUX_SIPHASH_H

#include <linux/types.h>

enum siphash24_lengths {
	SIPHASH24_KEY_LEN = 16
};

uint64_t siphash24(const uint8_t *data, size_t len, const uint8_t key[SIPHASH24_KEY_LEN]);

#endif /* _LINUX_SIPHASH_H */
