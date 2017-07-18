/*
 * Backend for the LRNG providing the cryptographic primitives using
 * ChaCha20 cipher implementations.
 *
 * Copyright (C) 2016 - 2017, Stephan Mueller <smueller@chronox.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL2
 * are required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cryptohash.h>
#include <crypto/chacha20.h>
#include <linux/random.h>

/******************************* ChaCha20 DRNG *******************************/

/* State according to RFC 7539 section 2.3 */
struct chacha20_block {
	u32 constants[4];
#define CHACHA20_KEY_SIZE_WORDS (CHACHA20_KEY_SIZE / sizeof(u32))
	union {
		u32 u[CHACHA20_KEY_SIZE_WORDS];
		u8  b[CHACHA20_KEY_SIZE];
	} key;
	u32 counter;
	u32 nonce[3];
};

struct chacha20_state {
	struct chacha20_block block;
};

/**
 * Update of the ChaCha20 state by generating one ChaCha20 block which is
 * equal to the state of the ChaCha20. The generated block is XORed into
 * the key part of the state. This shall ensure backtracking resistance as well
 * as a proper mix of the ChaCha20 state once the key is injected.
 */
static void lrng_chacha20_update(struct chacha20_state *chacha20_state)
{
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 tmp[(CHACHA20_BLOCK_SIZE / sizeof(u32))];
	u32 i;

	BUILD_BUG_ON(sizeof(struct chacha20_block) != CHACHA20_BLOCK_SIZE);
	BUILD_BUG_ON(CHACHA20_BLOCK_SIZE != 2 * CHACHA20_KEY_SIZE);

	chacha20_block(&chacha20->constants[0], tmp);
	for (i = 0; i < CHACHA20_KEY_SIZE_WORDS; i++) {
		chacha20->key.u[i] ^= tmp[i];
		chacha20->key.u[i] ^= tmp[i + CHACHA20_KEY_SIZE_WORDS];
	}

	memzero_explicit(tmp, sizeof(tmp));

	/* Deterministic increment of nonce as required in RFC 7539 chapter 4 */
	chacha20->nonce[0]++;
	if (chacha20->nonce[0] == 0)
		chacha20->nonce[1]++;
	if (chacha20->nonce[1] == 0)
		chacha20->nonce[2]++;

	/* Leave counter untouched as it is start value is undefined in RFC */
}

/**
 * Seed the ChaCha20 DRNG by injecting the input data into the key part of
 * the ChaCha20 state. If the input data is longer than the ChaCha20 key size,
 * perform a ChaCha20 operation after processing of key size input data.
 * This operation shall spread out the entropy into the ChaCha20 state before
 * new entropy is injected into the key part.
 */
int lrng_drng_seed_helper(void *drng, const u8 *inbuf, u32 inbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;

	while (inbuflen) {
		u32 i, todo = min_t(u32, inbuflen, CHACHA20_KEY_SIZE);

		for (i = 0; i < todo; i++)
			chacha20->key.b[i] ^= inbuf[i];

		/* Break potential dependencies between the inbuf key blocks */
		lrng_chacha20_update(chacha20_state);
		inbuf += todo;
		inbuflen -= todo;
	}

	return 0;
}

/**
 * Chacha20 DRNG generation of random numbers: the stream output of ChaCha20
 * is the random number. After the completion of the generation of the
 * stream, the entire ChaCha20 state is updated.
 *
 * Note, as the ChaCha20 implements a 32 bit counter, we must ensure
 * that this function is only invoked for at most 2^32 - 1 ChaCha20 blocks
 * before a reseed or an update happens. This is ensured by the variable
 * outbuflen which is a 32 bit integer defining the number of bytes to be
 * generated by the ChaCha20 DRNG. At the end of this function, an update
 * operation is invoked which implies that the 32 bit counter will never be
 * overflown in this implementation.
 */
int lrng_drng_generate_helper(void *drng, u8 *outbuf, u32 outbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 ret = outbuflen;

	while (outbuflen >= CHACHA20_BLOCK_SIZE) {
		chacha20_block(&chacha20->constants[0], outbuf);
		outbuf += CHACHA20_BLOCK_SIZE;
		outbuflen -= CHACHA20_BLOCK_SIZE;
	}

	if (outbuflen) {
		u8 stream[CHACHA20_BLOCK_SIZE];

		chacha20_block(&chacha20->constants[0], stream);
		memcpy(outbuf, stream, outbuflen);
		memzero_explicit(stream, sizeof(stream));
	}

	lrng_chacha20_update(chacha20_state);

	return ret;
}

/**
 * ChaCha20 DRNG that provides full strength, i.e. the output is capable
 * of transporting 1 bit of entropy per data bit, provided the DRNG was
 * seeded with 256 bits of entropy. This is achieved by folding the ChaCha20
 * block output of 512 bits in half using XOR.
 *
 * Other than the output handling, the implementation is conceptually
 * identical to lrng_drng_generate_helper.
 */
int lrng_drng_generate_helper_full(void *drng, u8 *outbuf, u32 outbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 ret = outbuflen;
	u8 stream[CHACHA20_BLOCK_SIZE];

	while (outbuflen >= CHACHA20_BLOCK_SIZE) {
		u32 i;

		chacha20_block(&chacha20->constants[0], outbuf);

		/* fold output in half */
		for (i = 0; i < CHACHA20_BLOCK_SIZE / 2; i++)
			outbuf[i] ^= outbuf[i + (CHACHA20_BLOCK_SIZE / 2)];

		outbuf += CHACHA20_BLOCK_SIZE / 2;
		outbuflen -= CHACHA20_BLOCK_SIZE / 2;
	}

	while (outbuflen) {
		u32 i, todo = min_t(u32, CHACHA20_BLOCK_SIZE / 2, outbuflen);

		chacha20_block(&chacha20->constants[0], stream);

		/* fold output in half */
		for (i = 0; i < todo; i++)
			stream[i] ^= stream[i + (CHACHA20_BLOCK_SIZE / 2)];

		memcpy(outbuf, stream, todo);
		outbuflen -= todo;
		outbuf += todo;
	}
	memzero_explicit(stream, sizeof(stream));

	lrng_chacha20_update(chacha20_state);

	return ret;
}

/**
 * Allocation of the DRNG state
 */
void *lrng_drng_alloc(u32 sec_strength)
{
	struct chacha20_state *chacha20_state;
	struct chacha20_block *chacha20;
	unsigned long v;
	u32 i;

	if (sec_strength > CHACHA20_KEY_SIZE) {
		pr_err("Security strength of ChaCha20 DRNG (%u bits) lower "
		       "than requested by LRNG (%u bits)\n",
			CHACHA20_KEY_SIZE * 8, sec_strength * 8);
		return ERR_PTR(-EINVAL);
	}
	if (sec_strength < CHACHA20_KEY_SIZE)
		pr_warn("Security strength of ChaCha20 DRNG (%u bits) higher "
			"than requested by LRNG (%u bits)\n",
			CHACHA20_KEY_SIZE * 8, sec_strength * 8);

	chacha20_state = kzalloc(sizeof(struct chacha20_state), GFP_KERNEL);
	if (!chacha20_state)
		return ERR_PTR(-ENOMEM);

	chacha20 = &chacha20_state->block;

	memcpy(&chacha20->constants[0], "expand 32-byte k", 16);

	for (i = 0; i < CHACHA20_KEY_SIZE_WORDS; i++) {
		chacha20->key.u[i] ^= jiffies;
		chacha20->key.u[i] ^= random_get_entropy();
		if (arch_get_random_long(&v))
			chacha20->key.u[i] ^= v;
	}

	for (i = 0; i < 3; i++) {
		chacha20->nonce[i] ^= jiffies;
		chacha20->nonce[i] ^= random_get_entropy();
		if (arch_get_random_long(&v))
			chacha20->nonce[i] ^= v;
	}

	pr_info("ChaCha20 core allocated\n");

	return chacha20_state;
}

void lrng_drng_dealloc(void *drng)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;

	kzfree(chacha20_state);
}

/******************************* Hash Operation *******************************/

void *lrng_hash_alloc(const u8 *key, u32 keylen)
{
	pr_info("Hash SHA-1 allocated\n");
	return NULL;
}

u32 lrng_hash_digestsize(void *hash)
{
	return (SHA_DIGEST_WORDS * sizeof(u32));
}

int lrng_hash_buffer(void *hash, const u8 *inbuf, u32 inbuflen, u8 *digest)
{
	u32 i;
	u32 workspace[SHA_WORKSPACE_WORDS];

	WARN_ON(inbuflen % SHA_WORKSPACE_WORDS);

	for (i = 0; i < inbuflen; i += (SHA_WORKSPACE_WORDS * sizeof(u32)))
		sha_transform((u32 *)digest, (inbuf + i), workspace);
	memzero_explicit(workspace, sizeof(workspace));

	return 0;
}
