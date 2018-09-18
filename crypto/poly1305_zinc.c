/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <crypto/algapi.h>
#include <crypto/internal/hash.h>
#include <zinc/poly1305.h>
#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/simd.h>

struct poly1305_desc_ctx {
	struct poly1305_ctx ctx;
	u8 key[POLY1305_KEY_SIZE];
	unsigned int rem_key_bytes;
};

static int crypto_poly1305_init(struct shash_desc *desc)
{
	struct poly1305_desc_ctx *dctx = shash_desc_ctx(desc);
	dctx->rem_key_bytes = POLY1305_KEY_SIZE;
	return 0;
}

static int crypto_poly1305_update(struct shash_desc *desc, const u8 *src,
				  unsigned int srclen)
{
	struct poly1305_desc_ctx *dctx = shash_desc_ctx(desc);
	simd_context_t simd_context;

	if (unlikely(dctx->rem_key_bytes)) {
		unsigned int key_bytes = min(srclen, dctx->rem_key_bytes);
		memcpy(dctx->key + (POLY1305_KEY_SIZE - dctx->rem_key_bytes),
		       src, key_bytes);
		src += key_bytes;
		srclen -= key_bytes;
		dctx->rem_key_bytes -= key_bytes;
		if (!dctx->rem_key_bytes) {
			poly1305_init(&dctx->ctx, dctx->key);
			memzero_explicit(dctx->key, sizeof(dctx->key));
		}
		if (!srclen)
			return 0;
	}

	simd_get(&simd_context);
	poly1305_update(&dctx->ctx, src, srclen, &simd_context);
	simd_put(&simd_context);

	return 0;
}

static int crypto_poly1305_final(struct shash_desc *desc, u8 *dst)
{
	struct poly1305_desc_ctx *dctx = shash_desc_ctx(desc);
	simd_context_t simd_context;

	simd_get(&simd_context);
	poly1305_final(&dctx->ctx, dst, &simd_context);
	simd_put(&simd_context);
	return 0;
}

static struct shash_alg poly1305_alg = {
	.digestsize	= POLY1305_MAC_SIZE,
	.init		= crypto_poly1305_init,
	.update		= crypto_poly1305_update,
	.final		= crypto_poly1305_final,
	.descsize	= sizeof(struct poly1305_desc_ctx),
	.base		= {
		.cra_name		= "poly1305",
		.cra_driver_name	= "poly1305-software",
		.cra_priority		= 100,
		.cra_blocksize		= POLY1305_BLOCK_SIZE,
		.cra_module		= THIS_MODULE,
	},
};

static int __init poly1305_mod_init(void)
{
	return crypto_register_shash(&poly1305_alg);
}

static void __exit poly1305_mod_exit(void)
{
	crypto_unregister_shash(&poly1305_alg);
}

module_init(poly1305_mod_init);
module_exit(poly1305_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>");
MODULE_DESCRIPTION("Poly1305 authenticator");
MODULE_ALIAS_CRYPTO("poly1305");
MODULE_ALIAS_CRYPTO("poly1305-software");
