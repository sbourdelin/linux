#ifndef _CRYPTO_MD5_H
#define _CRYPTO_MD5_H

#include <linux/types.h>

#define MD5_DIGEST_SIZE		16
#define MD5_HMAC_BLOCK_SIZE	64
#define MD5_BLOCK_WORDS		16
#define MD5_HASH_WORDS		4

#define MD5_H0	0x67452301UL
#define MD5_H1	0xefcdab89UL
#define MD5_H2	0x98badcfeUL
#define MD5_H3	0x10325476UL

static const u8 md5_zero_message_hash[MD5_DIGEST_SIZE] = {
	0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
	0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e,
};

struct md5_state {
	u32 hash[MD5_HASH_WORDS];
	u32 block[MD5_BLOCK_WORDS];
	u64 byte_count;
};

#endif
