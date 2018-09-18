/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <asm/unaligned.h>
#include <crypto/algapi.h>
#include <crypto/internal/skcipher.h>
#include <zinc/chacha20.h>
#include <linux/module.h>

struct chacha20_key_ctx {
	u32 key[8];
};

static int crypto_chacha20_setkey(struct crypto_skcipher *tfm, const u8 *key,
				  unsigned int keysize)
{
	struct chacha20_key_ctx *key_ctx = crypto_skcipher_ctx(tfm);
	int i;

	if (keysize != CHACHA20_KEY_SIZE)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(key_ctx->key); ++i)
		key_ctx->key[i] = get_unaligned_le32(key + i * sizeof(u32));

	return 0;
}

static int crypto_chacha20_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct chacha20_key_ctx *key_ctx = crypto_skcipher_ctx(tfm);
	struct chacha20_ctx ctx;
	struct skcipher_walk walk;
	simd_context_t simd_context;
	int err, i;

	err = skcipher_walk_virt(&walk, req, true);
	if (unlikely(err))
		return err;

	memcpy(ctx.key, key_ctx->key, sizeof(ctx.key));
	for (i = 0; i < ARRAY_SIZE(ctx.counter); ++i)
		ctx.counter[i] = get_unaligned_le32(walk.iv + i * sizeof(u32));

	simd_get(&simd_context);
	while (walk.nbytes > 0) {
		unsigned int nbytes = walk.nbytes;

		if (nbytes < walk.total)
			nbytes = round_down(nbytes, walk.stride);

		chacha20(&ctx, walk.dst.virt.addr, walk.src.virt.addr, nbytes,
			 &simd_context);

		err = skcipher_walk_done(&walk, walk.nbytes - nbytes);
		simd_relax(&simd_context);
	}
	simd_put(&simd_context);

	return err;
}

static struct skcipher_alg alg = {
	.base.cra_name		= "chacha20",
	.base.cra_driver_name	= "chacha20-software",
	.base.cra_priority	= 100,
	.base.cra_blocksize	= 1,
	.base.cra_ctxsize	= sizeof(struct chacha20_key_ctx),
	.base.cra_module	= THIS_MODULE,

	.min_keysize		= CHACHA20_KEY_SIZE,
	.max_keysize		= CHACHA20_KEY_SIZE,
	.ivsize			= CHACHA20_IV_SIZE,
	.chunksize		= CHACHA20_BLOCK_SIZE,
	.setkey			= crypto_chacha20_setkey,
	.encrypt		= crypto_chacha20_crypt,
	.decrypt		= crypto_chacha20_crypt,
};

static int __init chacha20_mod_init(void)
{
	return crypto_register_skcipher(&alg);
}

static void __exit chacha20_mod_exit(void)
{
	crypto_unregister_skcipher(&alg);
}

module_init(chacha20_mod_init);
module_exit(chacha20_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>");
MODULE_DESCRIPTION("ChaCha20 stream cipher");
MODULE_ALIAS_CRYPTO("chacha20");
MODULE_ALIAS_CRYPTO("chacha20-software");
