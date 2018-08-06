// SPDX-License-Identifier: GPL-2.0
/*
 * Poly1305 authenticator, NEON-accelerated -
 * glue code for OpenSSL implementation
 *
 * Copyright (c) 2018 Google LLC
 */

#include <asm/hwcap.h>
#include <asm/neon.h>
#include <asm/simd.h>
#include <asm/unaligned.h>
#include <crypto/cryptd.h>
#include <crypto/internal/hash.h>
#include <crypto/poly1305.h>
#include <linux/cpufeature.h>
#include <linux/crypto.h>
#include <linux/module.h>

asmlinkage void poly1305_init_arm(void *ctx, const u8 key[16]);
asmlinkage void poly1305_blocks_neon(void *ctx, const u8 *inp, size_t len,
				     u32 padbit);
asmlinkage void poly1305_emit_neon(void *ctx, u8 mac[16], const u32 nonce[4]);

struct poly1305_neon_desc_ctx {
	u8 buf[POLY1305_BLOCK_SIZE];
	unsigned int buflen;
	bool key_set;
	bool nonce_set;
	u32 nonce[4];
	u8 neon_ctx[192] __aligned(16);
} __aligned(16);

static int poly1305_neon_init(struct shash_desc *desc)
{
	struct poly1305_neon_desc_ctx *dctx = shash_desc_ctx(desc);

	dctx->buflen = 0;
	dctx->key_set = false;
	dctx->nonce_set = false;
	return 0;
}

static void poly1305_neon_blocks(struct poly1305_neon_desc_ctx *dctx,
				 const u8 *src, unsigned int srclen, u32 padbit)
{
	if (!dctx->key_set) {
		poly1305_init_arm(dctx->neon_ctx, src);
		dctx->key_set = true;
		src += POLY1305_BLOCK_SIZE;
		srclen -= POLY1305_BLOCK_SIZE;
		if (!srclen)
			return;
	}

	if (!dctx->nonce_set) {
		dctx->nonce[0] = get_unaligned_le32(src +  0);
		dctx->nonce[1] = get_unaligned_le32(src +  4);
		dctx->nonce[2] = get_unaligned_le32(src +  8);
		dctx->nonce[3] = get_unaligned_le32(src + 12);
		dctx->nonce_set = true;
		src += POLY1305_BLOCK_SIZE;
		srclen -= POLY1305_BLOCK_SIZE;
		if (!srclen)
			return;
	}

	kernel_neon_begin();
	poly1305_blocks_neon(dctx->neon_ctx, src, srclen, padbit);
	kernel_neon_end();
}

static int poly1305_neon_update(struct shash_desc *desc,
				const u8 *src, unsigned int srclen)
{
	struct poly1305_neon_desc_ctx *dctx = shash_desc_ctx(desc);
	unsigned int bytes;

	if (dctx->buflen) {
		bytes = min(srclen, POLY1305_BLOCK_SIZE - dctx->buflen);
		memcpy(&dctx->buf[dctx->buflen], src, bytes);
		dctx->buflen += bytes;
		src += bytes;
		srclen -= bytes;

		if (dctx->buflen == POLY1305_BLOCK_SIZE) {
			poly1305_neon_blocks(dctx, dctx->buf,
					     POLY1305_BLOCK_SIZE, 1);
			dctx->buflen = 0;
		}
	}

	if (srclen >= POLY1305_BLOCK_SIZE) {
		bytes = round_down(srclen, POLY1305_BLOCK_SIZE);
		poly1305_neon_blocks(dctx, src, bytes, 1);
		src += bytes;
		srclen -= bytes;
	}

	if (srclen) {
		memcpy(dctx->buf, src, srclen);
		dctx->buflen = srclen;
	}

	return 0;
}

static int poly1305_neon_final(struct shash_desc *desc, u8 *dst)
{
	struct poly1305_neon_desc_ctx *dctx = shash_desc_ctx(desc);

	if (!dctx->nonce_set)
		return -ENOKEY;

	if (dctx->buflen) {
		dctx->buf[dctx->buflen++] = 1;
		memset(&dctx->buf[dctx->buflen], 0,
		       POLY1305_BLOCK_SIZE - dctx->buflen);
		poly1305_neon_blocks(dctx, dctx->buf, POLY1305_BLOCK_SIZE, 0);
	}

	/*
	 * This part doesn't actually use NEON instructions, so no need for
	 * kernel_neon_begin().
	 */
	poly1305_emit_neon(dctx->neon_ctx, dst, dctx->nonce);
	return 0;
}

static struct shash_alg poly1305_alg = {
	.digestsize		= POLY1305_DIGEST_SIZE,
	.init			= poly1305_neon_init,
	.update			= poly1305_neon_update,
	.final			= poly1305_neon_final,
	.descsize		= sizeof(struct poly1305_desc_ctx),
	.base			= {
		.cra_name	= "__poly1305",
		.cra_driver_name = "__driver-poly1305-neon",
		.cra_priority	= 0,
		.cra_flags	= CRYPTO_ALG_INTERNAL,
		.cra_blocksize	= POLY1305_BLOCK_SIZE,
		.cra_module	= THIS_MODULE,
	},
};

/* Boilerplate to wrap the use of kernel_neon_begin() */

struct poly1305_async_ctx {
	struct cryptd_ahash *cryptd_tfm;
};

static int poly1305_async_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct poly1305_async_ctx *ctx = crypto_ahash_ctx(tfm);
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct cryptd_ahash *cryptd_tfm = ctx->cryptd_tfm;
	struct shash_desc *desc = cryptd_shash_desc(cryptd_req);
	struct crypto_shash *child = cryptd_ahash_child(cryptd_tfm);

	desc->tfm = child;
	desc->flags = req->base.flags;
	return crypto_shash_init(desc);
}

static int poly1305_async_update(struct ahash_request *req)
{
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct poly1305_async_ctx *ctx = crypto_ahash_ctx(tfm);
	struct cryptd_ahash *cryptd_tfm = ctx->cryptd_tfm;

	if (!may_use_simd() ||
	    (in_atomic() && cryptd_ahash_queued(cryptd_tfm))) {
		memcpy(cryptd_req, req, sizeof(*req));
		ahash_request_set_tfm(cryptd_req, &cryptd_tfm->base);
		return crypto_ahash_update(cryptd_req);
	} else {
		struct shash_desc *desc = cryptd_shash_desc(cryptd_req);
		return shash_ahash_update(req, desc);
	}
}

static int poly1305_async_final(struct ahash_request *req)
{
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct poly1305_async_ctx *ctx = crypto_ahash_ctx(tfm);
	struct cryptd_ahash *cryptd_tfm = ctx->cryptd_tfm;

	if (!may_use_simd() ||
	    (in_atomic() && cryptd_ahash_queued(cryptd_tfm))) {
		memcpy(cryptd_req, req, sizeof(*req));
		ahash_request_set_tfm(cryptd_req, &cryptd_tfm->base);
		return crypto_ahash_final(cryptd_req);
	} else {
		struct shash_desc *desc = cryptd_shash_desc(cryptd_req);
		return crypto_shash_final(desc, req->result);
	}
}

static int poly1305_async_digest(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct poly1305_async_ctx *ctx = crypto_ahash_ctx(tfm);
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct cryptd_ahash *cryptd_tfm = ctx->cryptd_tfm;

	if (!may_use_simd() ||
	    (in_atomic() && cryptd_ahash_queued(cryptd_tfm))) {
		memcpy(cryptd_req, req, sizeof(*req));
		ahash_request_set_tfm(cryptd_req, &cryptd_tfm->base);
		return crypto_ahash_digest(cryptd_req);
	} else {
		struct shash_desc *desc = cryptd_shash_desc(cryptd_req);
		struct crypto_shash *child = cryptd_ahash_child(cryptd_tfm);

		desc->tfm = child;
		desc->flags = req->base.flags;
		return shash_ahash_digest(req, desc);
	}
}

static int poly1305_async_import(struct ahash_request *req, const void *in)
{
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct poly1305_async_ctx *ctx = crypto_ahash_ctx(tfm);
	struct shash_desc *desc = cryptd_shash_desc(cryptd_req);

	desc->tfm = cryptd_ahash_child(ctx->cryptd_tfm);
	desc->flags = req->base.flags;

	return crypto_shash_import(desc, in);
}

static int poly1305_async_export(struct ahash_request *req, void *out)
{
	struct ahash_request *cryptd_req = ahash_request_ctx(req);
	struct shash_desc *desc = cryptd_shash_desc(cryptd_req);

	return crypto_shash_export(desc, out);
}

static int poly1305_async_init_tfm(struct crypto_tfm *tfm)
{
	struct cryptd_ahash *cryptd_tfm;
	struct poly1305_async_ctx *ctx = crypto_tfm_ctx(tfm);

	cryptd_tfm = cryptd_alloc_ahash("__driver-poly1305-neon",
					CRYPTO_ALG_INTERNAL,
					CRYPTO_ALG_INTERNAL);
	if (IS_ERR(cryptd_tfm))
		return PTR_ERR(cryptd_tfm);
	ctx->cryptd_tfm = cryptd_tfm;
	crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
				 sizeof(struct ahash_request) +
				 crypto_ahash_reqsize(&cryptd_tfm->base));

	return 0;
}

static void poly1305_async_exit_tfm(struct crypto_tfm *tfm)
{
	struct poly1305_async_ctx *ctx = crypto_tfm_ctx(tfm);

	cryptd_free_ahash(ctx->cryptd_tfm);
}

static struct ahash_alg poly1305_async_alg = {
	.init			= poly1305_async_init,
	.update			= poly1305_async_update,
	.final			= poly1305_async_final,
	.digest			= poly1305_async_digest,
	.import			= poly1305_async_import,
	.export			= poly1305_async_export,
	.halg.digestsize	= POLY1305_DIGEST_SIZE,
	.halg.statesize		= sizeof(struct poly1305_neon_desc_ctx),
	.halg.base		= {
		.cra_name	= "poly1305",
		.cra_driver_name = "poly1305-neon",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_ASYNC,
		.cra_blocksize	= POLY1305_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct poly1305_async_ctx),
		.cra_module	= THIS_MODULE,
		.cra_init	= poly1305_async_init_tfm,
		.cra_exit	= poly1305_async_exit_tfm,
	},
};

static int __init poly1305_neon_module_init(void)
{
	int err;

	if (!(elf_hwcap & HWCAP_NEON))
		return -ENODEV;

	err = crypto_register_shash(&poly1305_alg);
	if (err)
		return err;
	err = crypto_register_ahash(&poly1305_async_alg);
	if (err)
		goto err_shash;

	return 0;

err_shash:
	crypto_unregister_shash(&poly1305_alg);
	return err;
}

static void __exit poly1305_neon_module_exit(void)
{
	crypto_unregister_shash(&poly1305_alg);
	crypto_unregister_ahash(&poly1305_async_alg);
}

module_init(poly1305_neon_module_init);
module_exit(poly1305_neon_module_exit);

MODULE_DESCRIPTION("Poly1305 authenticator (NEON-accelerated)");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("poly1305");
MODULE_ALIAS_CRYPTO("poly1305-neon");
