// SPDX-License-Identifier: GPL-2.0
/*
 * HPolyC: length-preserving encryption for entry-level processors
 *
 * Reference: https://eprint.iacr.org/2018/720.pdf
 *
 * Copyright (c) 2018 Google LLC
 */

#include <crypto/algapi.h>
#include <crypto/chacha.h>
#include <crypto/internal/hash.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/module.h>

#include "internal.h"

/* Poly1305 and block cipher block size */
#define HPOLYC_BLOCK_SIZE		16

/* Key sizes in bytes */
#define HPOLYC_STREAM_KEY_SIZE		32	/* XChaCha stream key (K_S) */
#define HPOLYC_HASH_KEY_SIZE		16	/* Poly1305 hash key (K_H) */
#define HPOLYC_BLKCIPHER_KEY_SIZE	32	/* Block cipher key (K_E) */

/*
 * The HPolyC specification allows any tweak (IV) length <= UINT32_MAX bits, but
 * Linux's crypto API currently only allows algorithms to support a single IV
 * length.  We choose 12 bytes, which is the longest tweak that fits into a
 * single 16-byte Poly1305 block (as HPolyC reserves 4 bytes for the tweak
 * length), for the fastest performance.  And it's good enough for disk
 * encryption which really only needs an 8-byte tweak anyway.
 */
#define HPOLYC_IV_SIZE		12

struct hpolyc_instance_ctx {
	struct crypto_ahash_spawn poly1305_spawn;
	struct crypto_skcipher_spawn xchacha_spawn;
	struct crypto_spawn blkcipher_spawn;
};

struct hpolyc_tfm_ctx {
	struct crypto_ahash *poly1305;
	struct crypto_skcipher *xchacha;
	struct crypto_cipher *blkcipher;
	u8 poly1305_key[HPOLYC_HASH_KEY_SIZE];	/* K_H (unclamped) */
};

struct hpolyc_request_ctx {

	/* First part of data passed to the two Poly1305 hash steps */
	struct {
		u8 rkey[HPOLYC_BLOCK_SIZE];
		u8 skey[HPOLYC_BLOCK_SIZE];
		__le32 tweak_len;
		u8 tweak[HPOLYC_IV_SIZE];
	} hash_head;
	struct scatterlist hash_sg[2];

	/*
	 * Buffer for rightmost portion of data, i.e. the last 16-byte block
	 *
	 *    P_L => P_M => C_M => C_R when encrypting, or
	 *    C_R => C_M => P_M => P_L when decrypting.
	 *
	 * Also used to build the XChaCha IV as C_M || 1 || 0^63 || 0^64.
	 */
	u8 rbuf[XCHACHA_IV_SIZE];

	bool enc; /* true if encrypting, false if decrypting */

	/* Sub-requests, must be last */
	union {
		struct ahash_request poly1305_req;
		struct skcipher_request xchacha_req;
	} u;
};

/*
 * Given the 256-bit XChaCha stream key K_S, derive the 128-bit Poly1305 hash
 * key K_H and the 256-bit block cipher key K_E as follows:
 *
 *     K_H || K_E || ... = XChaCha(key=K_S, nonce=1||0^191)
 *
 * Note that this denotes using bits from the XChaCha keystream, which here we
 * get indirectly by encrypting a buffer containing all 0's.
 */
static int hpolyc_setkey(struct crypto_skcipher *tfm, const u8 *key,
			 unsigned int keylen)
{
	struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);
	struct {
		u8 iv[XCHACHA_IV_SIZE];
		u8 derived_keys[HPOLYC_HASH_KEY_SIZE +
				HPOLYC_BLKCIPHER_KEY_SIZE];
		struct scatterlist sg;
		struct crypto_wait wait;
		struct skcipher_request req; /* must be last */
	} *data;
	int err;

	/* Set XChaCha key */
	crypto_skcipher_clear_flags(tctx->xchacha, CRYPTO_TFM_REQ_MASK);
	crypto_skcipher_set_flags(tctx->xchacha,
				  crypto_skcipher_get_flags(tfm) &
				  CRYPTO_TFM_REQ_MASK);
	err = crypto_skcipher_setkey(tctx->xchacha, key, keylen);
	crypto_skcipher_set_flags(tfm,
				  crypto_skcipher_get_flags(tctx->xchacha) &
				  CRYPTO_TFM_RES_MASK);
	if (err)
		return err;

	/* Derive the Poly1305 and block cipher keys */
	data = kzalloc(sizeof(*data) + crypto_skcipher_reqsize(tctx->xchacha),
		       GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->iv[0] = 1;
	sg_init_one(&data->sg, data->derived_keys, sizeof(data->derived_keys));
	crypto_init_wait(&data->wait);
	skcipher_request_set_tfm(&data->req, tctx->xchacha);
	skcipher_request_set_callback(&data->req, CRYPTO_TFM_REQ_MAY_SLEEP |
						  CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &data->wait);
	skcipher_request_set_crypt(&data->req, &data->sg, &data->sg,
				   sizeof(data->derived_keys), data->iv);
	err = crypto_wait_req(crypto_skcipher_encrypt(&data->req),
			      &data->wait);
	if (err)
		goto out;

	/*
	 * Save the Poly1305 key.  It is not clamped here, since that is handled
	 * by the Poly1305 implementation.
	 */
	memcpy(tctx->poly1305_key, data->derived_keys, HPOLYC_HASH_KEY_SIZE);

	/* Set block cipher key */
	crypto_cipher_clear_flags(tctx->blkcipher, CRYPTO_TFM_REQ_MASK);
	crypto_cipher_set_flags(tctx->blkcipher,
				crypto_skcipher_get_flags(tfm) &
				CRYPTO_TFM_REQ_MASK);
	err = crypto_cipher_setkey(tctx->blkcipher,
				   &data->derived_keys[HPOLYC_HASH_KEY_SIZE],
				   HPOLYC_BLKCIPHER_KEY_SIZE);
	crypto_skcipher_set_flags(tfm,
				  crypto_cipher_get_flags(tctx->blkcipher) &
				  CRYPTO_TFM_RES_MASK);
out:
	kzfree(data);
	return err;
}

static inline void async_done(struct crypto_async_request *areq, int err,
			      int (*next_step)(struct skcipher_request *, u32))
{
	struct skcipher_request *req = areq->data;

	if (err)
		goto out;

	err = next_step(req, req->base.flags & ~CRYPTO_TFM_REQ_MAY_SLEEP);
	if (err == -EINPROGRESS || err == -EBUSY)
		return;
out:
	skcipher_request_complete(req, err);
}

/*
 * Following completion of the second hash step, do the second bitwise inversion
 * to complete the identity a - b = ~(a + ~(b)), then copy the result to the
 * last block of the destination scatterlist.  This completes HPolyC.
 */
static int hpolyc_finish(struct skcipher_request *req, u32 flags)
{
	struct hpolyc_request_ctx *rctx = skcipher_request_ctx(req);
	int i;

	for (i = 0; i < HPOLYC_BLOCK_SIZE; i++)
		rctx->rbuf[i] ^= 0xff;

	scatterwalk_map_and_copy(rctx->rbuf, req->dst,
				 req->cryptlen - HPOLYC_BLOCK_SIZE,
				 HPOLYC_BLOCK_SIZE, 1);
	return 0;
}

static void hpolyc_hash2_done(struct crypto_async_request *areq, int err)
{
	async_done(areq, err, hpolyc_finish);
}

/*
 * Following completion of the XChaCha step, do the second hash step to compute
 * the last output block.  Note that the last block needs to be subtracted
 * rather than added, which isn't compatible with typical Poly1305
 * implementations.  Thus, we use the identity a - b = ~(a + (~b)).
 */
static int hpolyc_hash2_step(struct skcipher_request *req, u32 flags)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	const struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);
	struct hpolyc_request_ctx *rctx = skcipher_request_ctx(req);
	int i;

	/* If decrypting, decrypt C_M with the block cipher to get P_M */
	if (!rctx->enc)
		crypto_cipher_decrypt_one(tctx->blkcipher, rctx->rbuf,
					  rctx->rbuf);

	for (i = 0; i < HPOLYC_BLOCK_SIZE; i++)
		rctx->hash_head.skey[i] = rctx->rbuf[i] ^ 0xff;

	sg_chain(rctx->hash_sg, 2, req->dst);

	ahash_request_set_tfm(&rctx->u.poly1305_req, tctx->poly1305);
	ahash_request_set_crypt(&rctx->u.poly1305_req, rctx->hash_sg,
				rctx->rbuf, sizeof(rctx->hash_head) +
				req->cryptlen - HPOLYC_BLOCK_SIZE);
	ahash_request_set_callback(&rctx->u.poly1305_req, flags,
				   hpolyc_hash2_done, req);
	return crypto_ahash_digest(&rctx->u.poly1305_req) ?:
		hpolyc_finish(req, flags);
}

static void hpolyc_xchacha_done(struct crypto_async_request *areq, int err)
{
	async_done(areq, err, hpolyc_hash2_step);
}

static int hpolyc_xchacha_step(struct skcipher_request *req, u32 flags)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	const struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);
	struct hpolyc_request_ctx *rctx = skcipher_request_ctx(req);
	unsigned int xchacha_len;

	/* If encrypting, encrypt P_M with the block cipher to get C_M */
	if (rctx->enc)
		crypto_cipher_encrypt_one(tctx->blkcipher, rctx->rbuf,
					  rctx->rbuf);

	/* Initialize the rest of the XChaCha IV (first part is C_M) */
	rctx->rbuf[HPOLYC_BLOCK_SIZE] = 1;
	memset(&rctx->rbuf[HPOLYC_BLOCK_SIZE + 1], 0,
	       sizeof(rctx->rbuf) - (HPOLYC_BLOCK_SIZE + 1));

	/*
	 * XChaCha needs to be done on all the data except the last 16 bytes;
	 * for disk encryption that usually means 4080 or 496 bytes.  But ChaCha
	 * implementations tend to be most efficient when passed a whole number
	 * of 64-byte ChaCha blocks, or sometimes even a multiple of 256 bytes.
	 * And here it doesn't matter whether the last 16 bytes are written to,
	 * as the second hash step will overwrite them.  Thus, round the XChaCha
	 * length up to the next 64-byte boundary if possible.
	 */
	xchacha_len = req->cryptlen - HPOLYC_BLOCK_SIZE;
	if (round_up(xchacha_len, CHACHA_BLOCK_SIZE) <= req->cryptlen)
		xchacha_len = round_up(xchacha_len, CHACHA_BLOCK_SIZE);

	skcipher_request_set_tfm(&rctx->u.xchacha_req, tctx->xchacha);
	skcipher_request_set_crypt(&rctx->u.xchacha_req, req->src, req->dst,
				   xchacha_len, rctx->rbuf);
	skcipher_request_set_callback(&rctx->u.xchacha_req, flags,
				      hpolyc_xchacha_done, req);
	return crypto_skcipher_encrypt(&rctx->u.xchacha_req) ?:
		hpolyc_hash2_step(req, flags);
}

static void hpolyc_hash1_done(struct crypto_async_request *areq, int err)
{
	async_done(areq, err, hpolyc_xchacha_step);
}

/*
 * HPolyC encryption/decryption.
 *
 * The first step is to Poly1305-hash the tweak and source data to get P_M (if
 * encrypting) or C_M (if decrypting), storing the result in rctx->rbuf.
 * Linux's Poly1305 doesn't use the usual keying mechanism and instead
 * interprets the data as (rkey, skey, real data), so we pass:
 *
 *    1. rkey = poly1305_key
 *    2. skey = last block of data (P_R or C_R)
 *    3. tweak block (assuming 12-byte tweak, so it fits in one block)
 *    4. rest of the data
 *
 * We put 1-3 in rctx->hash_head and chain it to the rest from req->src.
 *
 * Note: as a future optimization, a keyed version of Poly1305 that is keyed
 * with the 'rkey' could be implemented, allowing vectorized implementations of
 * Poly1305 to precompute powers of the key.  Though, that would be most
 * beneficial on small messages, whereas in the disk/file encryption use case,
 * longer 512-byte or 4096-byte messages are the most performance-critical.
 *
 * Afterwards, we continue on to the XChaCha step.
 */
static int hpolyc_crypt(struct skcipher_request *req, bool enc)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	const struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);
	struct hpolyc_request_ctx *rctx = skcipher_request_ctx(req);

	if (req->cryptlen < HPOLYC_BLOCK_SIZE)
		return -EINVAL;

	rctx->enc = enc;

	BUILD_BUG_ON(sizeof(rctx->hash_head) % HPOLYC_BLOCK_SIZE != 0);
	BUILD_BUG_ON(HPOLYC_HASH_KEY_SIZE != HPOLYC_BLOCK_SIZE);
	BUILD_BUG_ON(sizeof(__le32) + HPOLYC_IV_SIZE != HPOLYC_BLOCK_SIZE);
	memcpy(rctx->hash_head.rkey, tctx->poly1305_key, HPOLYC_BLOCK_SIZE);
	scatterwalk_map_and_copy(rctx->hash_head.skey, req->src,
				 req->cryptlen - HPOLYC_BLOCK_SIZE,
				 HPOLYC_BLOCK_SIZE, 0);
	rctx->hash_head.tweak_len = cpu_to_le32(8 * HPOLYC_IV_SIZE);
	memcpy(rctx->hash_head.tweak, req->iv, HPOLYC_IV_SIZE);

	sg_init_table(rctx->hash_sg, 2);
	sg_set_buf(&rctx->hash_sg[0], &rctx->hash_head,
		   sizeof(rctx->hash_head));
	sg_chain(rctx->hash_sg, 2, req->src);

	ahash_request_set_tfm(&rctx->u.poly1305_req, tctx->poly1305);
	ahash_request_set_crypt(&rctx->u.poly1305_req, rctx->hash_sg,
				rctx->rbuf, sizeof(rctx->hash_head) +
				req->cryptlen - HPOLYC_BLOCK_SIZE);
	ahash_request_set_callback(&rctx->u.poly1305_req, req->base.flags,
				   hpolyc_hash1_done, req);
	return crypto_ahash_digest(&rctx->u.poly1305_req) ?:
		hpolyc_xchacha_step(req, req->base.flags);
}

static int hpolyc_encrypt(struct skcipher_request *req)
{
	return hpolyc_crypt(req, true);
}

static int hpolyc_decrypt(struct skcipher_request *req)
{
	return hpolyc_crypt(req, false);
}

static int hpolyc_init_tfm(struct crypto_skcipher *tfm)
{
	struct skcipher_instance *inst = skcipher_alg_instance(tfm);
	struct hpolyc_instance_ctx *ictx = skcipher_instance_ctx(inst);
	struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);
	struct crypto_ahash *poly1305;
	struct crypto_skcipher *xchacha;
	struct crypto_cipher *blkcipher;
	int err;

	poly1305 = crypto_spawn_ahash(&ictx->poly1305_spawn);
	if (IS_ERR(poly1305))
		return PTR_ERR(poly1305);

	xchacha = crypto_spawn_skcipher(&ictx->xchacha_spawn);
	if (IS_ERR(xchacha)) {
		err = PTR_ERR(xchacha);
		goto err_free_poly1305;
	}

	blkcipher = crypto_spawn_cipher(&ictx->blkcipher_spawn);
	if (IS_ERR(blkcipher)) {
		err = PTR_ERR(blkcipher);
		goto err_free_xchacha;
	}

	tctx->poly1305 = poly1305;
	tctx->xchacha = xchacha;
	tctx->blkcipher = blkcipher;

	crypto_skcipher_set_reqsize(tfm,
				    offsetof(struct hpolyc_request_ctx, u) +
				    max(FIELD_SIZEOF(struct hpolyc_request_ctx,
						     u.poly1305_req) +
					crypto_ahash_reqsize(poly1305),
					FIELD_SIZEOF(struct hpolyc_request_ctx,
						     u.xchacha_req) +
					crypto_skcipher_reqsize(xchacha)));
	return 0;

err_free_xchacha:
	crypto_free_skcipher(xchacha);
err_free_poly1305:
	crypto_free_ahash(poly1305);
	return err;
}

static void hpolyc_exit_tfm(struct crypto_skcipher *tfm)
{
	struct hpolyc_tfm_ctx *tctx = crypto_skcipher_ctx(tfm);

	crypto_free_ahash(tctx->poly1305);
	crypto_free_skcipher(tctx->xchacha);
	crypto_free_cipher(tctx->blkcipher);
}

static void hpolyc_free_instance(struct skcipher_instance *inst)
{
	struct hpolyc_instance_ctx *ictx = skcipher_instance_ctx(inst);

	crypto_drop_ahash(&ictx->poly1305_spawn);
	crypto_drop_skcipher(&ictx->xchacha_spawn);
	crypto_drop_spawn(&ictx->blkcipher_spawn);
	kfree(inst);
}

static int hpolyc_create(struct crypto_template *tmpl, struct rtattr **tb)
{
	struct crypto_attr_type *algt;
	u32 mask;
	const char *xchacha_name;
	const char *blkcipher_name;
	struct skcipher_instance *inst;
	struct hpolyc_instance_ctx *ictx;
	struct crypto_alg *poly1305_alg;
	struct hash_alg_common *poly1305;
	struct crypto_alg *blkcipher_alg;
	struct skcipher_alg *xchacha_alg;
	int err;

	algt = crypto_get_attr_type(tb);
	if (IS_ERR(algt))
		return PTR_ERR(algt);

	if ((algt->type ^ CRYPTO_ALG_TYPE_SKCIPHER) & algt->mask)
		return -EINVAL;

	mask = crypto_requires_sync(algt->type, algt->mask);

	xchacha_name = crypto_attr_alg_name(tb[1]);
	if (IS_ERR(xchacha_name))
		return PTR_ERR(xchacha_name);

	blkcipher_name = crypto_attr_alg_name(tb[2]);
	if (IS_ERR(blkcipher_name))
		return PTR_ERR(blkcipher_name);

	inst = kzalloc(sizeof(*inst) + sizeof(*ictx), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;
	ictx = skcipher_instance_ctx(inst);

	/* Poly1305 */

	poly1305_alg = crypto_find_alg("poly1305", &crypto_ahash_type, 0, mask);
	if (IS_ERR(poly1305_alg)) {
		err = PTR_ERR(poly1305_alg);
		goto out_free_inst;
	}
	poly1305 = __crypto_hash_alg_common(poly1305_alg);
	err = crypto_init_ahash_spawn(&ictx->poly1305_spawn, poly1305,
				      skcipher_crypto_instance(inst));
	if (err) {
		crypto_mod_put(poly1305_alg);
		goto out_free_inst;
	}
	err = -EINVAL;
	if (poly1305->digestsize != HPOLYC_BLOCK_SIZE)
		goto out_drop_poly1305;

	/* XChaCha */

	err = crypto_grab_skcipher(&ictx->xchacha_spawn, xchacha_name, 0, mask);
	if (err)
		goto out_drop_poly1305;
	xchacha_alg = crypto_spawn_skcipher_alg(&ictx->xchacha_spawn);
	err = -EINVAL;
	if (xchacha_alg->min_keysize != HPOLYC_STREAM_KEY_SIZE ||
	    xchacha_alg->max_keysize != HPOLYC_STREAM_KEY_SIZE)
		goto out_drop_xchacha;
	if (xchacha_alg->base.cra_blocksize != 1)
		goto out_drop_xchacha;
	if (crypto_skcipher_alg_ivsize(xchacha_alg) != XCHACHA_IV_SIZE)
		goto out_drop_xchacha;

	/* Block cipher */

	err = crypto_grab_spawn(&ictx->blkcipher_spawn, blkcipher_name,
				CRYPTO_ALG_TYPE_CIPHER, CRYPTO_ALG_TYPE_MASK);
	if (err)
		goto out_drop_xchacha;
	blkcipher_alg = ictx->blkcipher_spawn.alg;
	err = -EINVAL;
	if (blkcipher_alg->cra_blocksize != HPOLYC_BLOCK_SIZE)
		goto out_drop_blkcipher;
	if (blkcipher_alg->cra_cipher.cia_min_keysize >
	    HPOLYC_BLKCIPHER_KEY_SIZE ||
	    blkcipher_alg->cra_cipher.cia_max_keysize <
	    HPOLYC_BLKCIPHER_KEY_SIZE)
		goto out_drop_blkcipher;

	/* Instance fields */

	err = -ENAMETOOLONG;
	if (snprintf(inst->alg.base.cra_name, CRYPTO_MAX_ALG_NAME,
		     "hpolyc(%s,%s)",
		     xchacha_alg->base.cra_name,
		     blkcipher_alg->cra_name) >= CRYPTO_MAX_ALG_NAME)
		goto out_drop_blkcipher;
	if (snprintf(inst->alg.base.cra_driver_name, CRYPTO_MAX_ALG_NAME,
		     "hpolyc(%s,%s,%s)",
		     poly1305_alg->cra_driver_name,
		     xchacha_alg->base.cra_driver_name,
		     blkcipher_alg->cra_driver_name) >= CRYPTO_MAX_ALG_NAME)
		goto out_drop_blkcipher;

	inst->alg.base.cra_blocksize = HPOLYC_BLOCK_SIZE;
	inst->alg.base.cra_ctxsize = sizeof(struct hpolyc_tfm_ctx);
	inst->alg.base.cra_alignmask = xchacha_alg->base.cra_alignmask |
					poly1305_alg->cra_alignmask;
	/*
	 * The block cipher is only invoked once per message, so for long
	 * messages (e.g. sectors for disk encryption) its performance doesn't
	 * matter nearly as much as that of XChaCha and Poly1305.  Thus, weigh
	 * the block cipher's ->cra_priority less.
	 */
	inst->alg.base.cra_priority = (2 * xchacha_alg->base.cra_priority +
				       2 * poly1305_alg->cra_priority +
				       blkcipher_alg->cra_priority) / 5;

	inst->alg.setkey = hpolyc_setkey;
	inst->alg.encrypt = hpolyc_encrypt;
	inst->alg.decrypt = hpolyc_decrypt;
	inst->alg.init = hpolyc_init_tfm;
	inst->alg.exit = hpolyc_exit_tfm;
	inst->alg.min_keysize = HPOLYC_STREAM_KEY_SIZE;
	inst->alg.max_keysize = HPOLYC_STREAM_KEY_SIZE;
	inst->alg.ivsize = HPOLYC_IV_SIZE;

	inst->free = hpolyc_free_instance;

	err = skcipher_register_instance(tmpl, inst);
	if (err)
		goto out_drop_blkcipher;

	return 0;

out_drop_blkcipher:
	crypto_drop_spawn(&ictx->blkcipher_spawn);
out_drop_xchacha:
	crypto_drop_skcipher(&ictx->xchacha_spawn);
out_drop_poly1305:
	crypto_drop_ahash(&ictx->poly1305_spawn);
out_free_inst:
	kfree(inst);
	return err;
}

/* hpolyc(xchacha_name, blkcipher_name) */
static struct crypto_template hpolyc_tmpl = {
	.name = "hpolyc",
	.create = hpolyc_create,
	.module = THIS_MODULE,
};

static int hpolyc_module_init(void)
{
	return crypto_register_template(&hpolyc_tmpl);
}

static void __exit hpolyc_module_exit(void)
{
	crypto_unregister_template(&hpolyc_tmpl);
}

module_init(hpolyc_module_init);
module_exit(hpolyc_module_exit);

MODULE_DESCRIPTION("HPolyC length-preserving encryption mode");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Eric Biggers <ebiggers@google.com>");
MODULE_ALIAS_CRYPTO("hpolyc");
