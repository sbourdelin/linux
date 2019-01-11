// SPDX-License-Identifier: GPL-2.0

/*
 * SP800-108 Key-derivation function
 *
 * Copyright (C) 2019, Stephan Mueller <smueller@chronox.de>
 */

/*
 * For performing a KDF operation, the following input is required
 * from the caller:
 *
 *	* Keying material to be used to derive the new keys from
 *	  (denoted as Ko in SP800-108)
 *	* Label -- a free form binary string
 *	* Context -- a free form binary string
 *
 * The KDF is implemented as a random number generator.
 *
 * The Ko keying material is to be provided with the initialization of the KDF
 * "random number generator", i.e. with the crypto_rng_reset function.
 *
 * The Label and Context concatenated string is provided when obtaining random
 * numbers, i.e. with the crypto_rng_generate function. The caller must format
 * the free-form Label || Context input as deemed necessary for the given
 * purpose. Note, SP800-108 mandates that the Label and Context are separated
 * by a 0x00 byte, i.e. the caller shall provide the input as
 * Label || 0x00 || Context when trying to be compliant to SP800-108. For
 * the feedback KDF, an IV is required as documented below.
 *
 * Example without proper error handling:
 *	char *keying_material = "\x00\x11\x22\x33\x44\x55\x66\x77";
 *	char *label_context = "\xde\xad\xbe\xef\x00\xde\xad\xbe\xef";
 *	kdf = crypto_alloc_rng(name, 0, 0);
 *	crypto_rng_reset(kdf, keying_material, 8);
 *	crypto_rng_generate(kdf, label_context, 9, outbuf, outbuflen);
 *
 * NOTE: In-place cipher operations are not supported.
 */

#include <linux/module.h>
#include <crypto/rng.h>
#include <crypto/internal/rng.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>

struct crypto_kdf_ctx {
	struct crypto_shash *kmd;
};

#define CRYPTO_KDF_MAX_DIGESTSIZE	64
#define CRYPTO_KDF_MAX_ALIGNMASK	0x3f

static inline void crypto_kdf_init_desc(struct shash_desc *desc,
					struct crypto_shash *kmd)
{
	desc->tfm = kmd;
	desc->flags = crypto_shash_get_flags(kmd) & CRYPTO_TFM_REQ_MAY_SLEEP;
}

/*
 * Implementation of the KDF in double pipeline iteration mode according with
 * counter to SP800-108 section 5.3.
 *
 * The caller must provide Label || 0x00 || Context in src. This src pointer
 * may also be NULL if the caller wishes not to provide anything.
 */
static int crypto_kdf_dpi_random(struct crypto_rng *rng,
				 const u8 *src, unsigned int slen,
				 u8 *dst, unsigned int dlen)
{
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	struct crypto_shash *kmd = ctx->kmd;
	SHASH_DESC_ON_STACK(desc, kmd);
	__be32 counter = cpu_to_be32(1);
	unsigned int h = crypto_shash_digestsize(kmd);
	unsigned int alignmask = crypto_shash_alignmask(kmd);
	int err = 0;
	u8 *dst_orig = dst;
	u8 Aiblock[CRYPTO_KDF_MAX_DIGESTSIZE + CRYPTO_KDF_MAX_ALIGNMASK];
	u8 *Ai = PTR_ALIGN((u8 *)Aiblock, alignmask + 1);

	crypto_kdf_init_desc(desc, kmd);

	memset(Ai, 0, h);

	while (dlen) {
		/* Calculate A(i) */
		if (dst == dst_orig && src && slen)
			/* 5.3 step 4 and 5.a */
			err = crypto_shash_digest(desc, src, slen, Ai);
		else
			/* 5.3 step 5.a */
			err = crypto_shash_digest(desc, Ai, h, Ai);
		if (err)
			goto out;

		/* Calculate K(i) -- step 5.b */
		err = crypto_shash_init(desc);
		if (err)
			goto out;

		err = crypto_shash_update(desc, Ai, h);
		if (err)
			goto out;

		err = crypto_shash_update(desc, (u8 *)&counter, sizeof(__be32));
		if (err)
			goto out;
		if (src && slen) {
			err = crypto_shash_update(desc, src, slen);
			if (err)
				goto out;
		}

		if (dlen < h) {
			u8 tmpbuffer[CRYPTO_KDF_MAX_DIGESTSIZE];

			err = crypto_shash_final(desc, tmpbuffer);
			if (err)
				goto out;
			memcpy(dst, tmpbuffer, dlen);
			memzero_explicit(tmpbuffer, h);
			goto out;
		} else {
			err = crypto_shash_final(desc, dst);
			if (err)
				goto out;
			dlen -= h;
			dst += h;
			counter = cpu_to_be32(be32_to_cpu(counter) + 1);
		}
	}

out:
	if (err)
		memzero_explicit(dst_orig, dlen);
	shash_desc_zero(desc);
	memzero_explicit(Ai, h);
	return err;
}

/*
 * Implementation of the KDF in feedback mode with a non-NULL IV and with
 * counter according to SP800-108 section 5.2. The IV is supplied with src
 * and must be equal to the digestsize of the used cipher.
 *
 * In addition, the caller must provide Label || 0x00 || Context in src. This
 * src pointer must not be NULL as the IV is required. The ultimate format of
 * the src pointer is IV || Label || 0x00 || Context where the length of the
 * IV is equal to the output size of the PRF.
 */
static int crypto_kdf_fb_random(struct crypto_rng *rng,
				const u8 *src, unsigned int slen,
				u8 *dst, unsigned int dlen)
{
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	struct crypto_shash *kmd = ctx->kmd;
	SHASH_DESC_ON_STACK(desc, kmd);
	__be32 counter = cpu_to_be32(1);
	unsigned int h = crypto_shash_digestsize(kmd), labellen = 0;
	int err = 0;
	u8 *dst_orig = dst;
	const u8 *label;

	/* require the presence of an IV */
	if (!src || slen < h)
		return -EINVAL;

	crypto_kdf_init_desc(desc, kmd);

	/* calculate the offset of the label / context data */
	label = src + h;
	labellen = slen - h;

	while (dlen) {
		err = crypto_shash_init(desc);
		if (err)
			goto out;

		/*
		 * Feedback mode applies to all rounds except first which uses
		 * the IV.
		 */
		if (dst_orig == dst)
			err = crypto_shash_update(desc, src, h);
		else
			err = crypto_shash_update(desc, dst - h, h);
		if (err)
			goto out;

		err = crypto_shash_update(desc, (u8 *)&counter, sizeof(__be32));
		if (err)
			goto out;
		if (labellen) {
			err = crypto_shash_update(desc, label, labellen);
			if (err)
				goto out;
		}

		if (dlen < h) {
			u8 tmpbuffer[CRYPTO_KDF_MAX_DIGESTSIZE];

			err = crypto_shash_final(desc, tmpbuffer);
			if (err)
				goto out;
			memcpy(dst, tmpbuffer, dlen);
			memzero_explicit(tmpbuffer, h);
			goto out;
		} else {
			err = crypto_shash_final(desc, dst);
			if (err)
				goto out;
			dlen -= h;
			dst += h;
			counter = cpu_to_be32(be32_to_cpu(counter) + 1);
		}
	}

out:
	if (err)
		memzero_explicit(dst_orig, dlen);
	return err;
}

/*
 * Implementation of the KDF in counter mode according to SP800-108 section 5.1
 * as well as SP800-56A section 5.8.1 (Single-step KDF).
 *
 * SP800-108:
 * The caller must provide Label || 0x00 || Context in src. This src pointer
 * may also be NULL if the caller wishes not to provide anything.
 *
 * SP800-56A:
 * The key provided for the HMAC during the crypto_rng_reset shall NOT be the
 * shared secret from the DH operation, but an independently generated key.
 * The src pointer is defined as Z || other info where Z is the shared secret
 * from DH and other info is an arbitrary string (see SP800-56A section
 * 5.8.1.2).
 */
static int crypto_kdf_ctr_random(struct crypto_rng *rng,
				 const u8 *src, unsigned int slen,
				 u8 *dst, unsigned int dlen)
{
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	struct crypto_shash *kmd = ctx->kmd;
	SHASH_DESC_ON_STACK(desc, kmd);
	__be32 counter = cpu_to_be32(1);
	unsigned int h = crypto_shash_digestsize(kmd);
	int err = 0;
	u8 *dst_orig = dst;

	crypto_kdf_init_desc(desc, kmd);

	while (dlen) {
		err = crypto_shash_init(desc);
		if (err)
			goto out;

		err = crypto_shash_update(desc, (u8 *)&counter, sizeof(__be32));
		if (err)
			goto out;

		if (src && slen) {
			err = crypto_shash_update(desc, src, slen);
			if (err)
				goto out;
		}

		if (dlen < h) {
			u8 tmpbuffer[CRYPTO_KDF_MAX_DIGESTSIZE];

			err = crypto_shash_final(desc, tmpbuffer);
			if (err)
				goto out;
			memcpy(dst, tmpbuffer, dlen);
			memzero_explicit(tmpbuffer, h);
			return 0;
		} else {
			err = crypto_shash_final(desc, dst);
			if (err)
				goto out;

			dlen -= h;
			dst += h;
			counter = cpu_to_be32(be32_to_cpu(counter) + 1);
		}
	}

out:
	if (err)
		memzero_explicit(dst_orig, dlen);
	shash_desc_zero(desc);
	return err;
}

/*
 * The seeding of the KDF allows to set a key which must be at least
 * digestsize long.
 */
static int crypto_kdf_seed(struct crypto_rng *rng,
			   const u8 *seed, unsigned int slen)
{
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	unsigned int ds = crypto_shash_digestsize(ctx->kmd);

	/* Check according to SP800-108 section 7.2 */
	if (ds > slen)
		return -EINVAL;

	/*
	 * We require that we operate on a MAC -- if we do not operate on a
	 * MAC, this function returns an error.
	 */
	return crypto_shash_setkey(ctx->kmd, seed, slen);
}

static int crypto_kdf_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_instance *inst = crypto_tfm_alg_instance(tfm);
	struct crypto_shash_spawn *spawn = crypto_instance_ctx(inst);
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_shash *kmd;
	unsigned int ds;

	kmd = crypto_spawn_shash(spawn);
	if (IS_ERR(kmd))
		return PTR_ERR(kmd);

	ds = crypto_shash_digestsize(kmd);
	/* Hashes with no digest size are not allowed for KDFs. */
	if (!ds || ds > CRYPTO_KDF_MAX_DIGESTSIZE ||
	    CRYPTO_KDF_MAX_ALIGNMASK < crypto_shash_alignmask(kmd)) {
		crypto_free_shash(kmd);
		return -EOPNOTSUPP;
	}

	ctx->kmd = kmd;

	return 0;
}

static void crypto_kdf_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_kdf_ctx *ctx = crypto_tfm_ctx(tfm);

	crypto_free_shash(ctx->kmd);
}

static void crypto_kdf_free(struct rng_instance *inst)
{
	crypto_drop_spawn(rng_instance_ctx(inst));
	kfree(inst);
}

static int crypto_kdf_alloc_common(struct crypto_template *tmpl,
				   struct rtattr **tb,
				   const u8 *name,
				   int (*generate)(struct crypto_rng *tfm,
						   const u8 *src,
						   unsigned int slen,
						   u8 *dst, unsigned int dlen))
{
	struct rng_instance *inst;
	struct crypto_alg *alg;
	struct shash_alg *salg;
	int err;
	unsigned int ds, ss;

	err = crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_RNG);
	if (err)
		return err;

	salg = shash_attr_alg(tb[1], 0, 0);
	if (IS_ERR(salg))
		return PTR_ERR(salg);

	ds = salg->digestsize;
	ss = salg->statesize;
	alg = &salg->base;

	inst = rng_alloc_instance(name, alg);
	err = PTR_ERR(inst);
	if (IS_ERR(inst))
		goto out_put_alg;

	err = crypto_init_shash_spawn(rng_instance_ctx(inst), salg,
				      rng_crypto_instance(inst));
	if (err)
		goto out_free_inst;

	inst->alg.base.cra_priority	= alg->cra_priority;
	inst->alg.base.cra_blocksize	= alg->cra_blocksize;
	inst->alg.base.cra_alignmask	= alg->cra_alignmask;

	inst->alg.generate		= generate;
	inst->alg.seed			= crypto_kdf_seed;
	inst->alg.seedsize		= ds;

	inst->alg.base.cra_init		= crypto_kdf_init_tfm;
	inst->alg.base.cra_exit		= crypto_kdf_exit_tfm;
	inst->alg.base.cra_ctxsize	= ALIGN(sizeof(struct crypto_kdf_ctx) +
					  ss * 2, crypto_tfm_ctx_alignment());

	inst->free			= crypto_kdf_free;

	err = rng_register_instance(tmpl, inst);

	if (err) {
out_free_inst:
		crypto_kdf_free(inst);
	}

out_put_alg:
	crypto_mod_put(alg);
	return err;
}

static int crypto_kdf_ctr_create(struct crypto_template *tmpl,
				 struct rtattr **tb)
{
	return crypto_kdf_alloc_common(tmpl, tb, "kdf_ctr",
				       crypto_kdf_ctr_random);
}

static struct crypto_template crypto_kdf_ctr_tmpl = {
	.name = "kdf_ctr",
	.create = crypto_kdf_ctr_create,
	.module = THIS_MODULE,
};

static int crypto_kdf_fb_create(struct crypto_template *tmpl,
				struct rtattr **tb) {
	return crypto_kdf_alloc_common(tmpl, tb, "kdf_fb",
				       crypto_kdf_fb_random);
}

static struct crypto_template crypto_kdf_fb_tmpl = {
	.name = "kdf_fb",
	.create = crypto_kdf_fb_create,
	.module = THIS_MODULE,
};

static int crypto_kdf_dpi_create(struct crypto_template *tmpl,
				 struct rtattr **tb) {
	return crypto_kdf_alloc_common(tmpl, tb, "kdf_dpi",
				       crypto_kdf_dpi_random);
}

static struct crypto_template crypto_kdf_dpi_tmpl = {
	.name = "kdf_dpi",
	.create = crypto_kdf_dpi_create,
	.module = THIS_MODULE,
};

static int __init crypto_kdf_init(void)
{
	int err = crypto_register_template(&crypto_kdf_ctr_tmpl);

	if (err)
		return err;

	err = crypto_register_template(&crypto_kdf_fb_tmpl);
	if (err) {
		crypto_unregister_template(&crypto_kdf_ctr_tmpl);
		return err;
	}

	err = crypto_register_template(&crypto_kdf_dpi_tmpl);
	if (err) {
		crypto_unregister_template(&crypto_kdf_ctr_tmpl);
		crypto_unregister_template(&crypto_kdf_fb_tmpl);
	}
	return err;
}

static void __exit crypto_kdf_exit(void)
{
	crypto_unregister_template(&crypto_kdf_ctr_tmpl);
	crypto_unregister_template(&crypto_kdf_fb_tmpl);
	crypto_unregister_template(&crypto_kdf_dpi_tmpl);
}

module_init(crypto_kdf_init);
module_exit(crypto_kdf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Key Derivation Function according to SP800-108");
MODULE_ALIAS_CRYPTO("kdf_ctr");
MODULE_ALIAS_CRYPTO("kdf_fb");
MODULE_ALIAS_CRYPTO("kdf_dpi");
