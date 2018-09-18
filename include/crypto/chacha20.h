/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values for the ChaCha20 algorithm
 */

#ifndef _CRYPTO_CHACHA20_H
#define _CRYPTO_CHACHA20_H

#define CHACHA20_IV_SIZE	16
#define CHACHA20_KEY_SIZE	32
#define CHACHA20_BLOCK_SIZE	64
#define CHACHA20_BLOCK_WORDS	(CHACHA20_BLOCK_SIZE / sizeof(u32))

void chacha20_block(u32 *state, u32 *stream);

#endif
