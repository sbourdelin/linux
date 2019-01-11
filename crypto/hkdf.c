// SPDX-License-Identifier: GPL-2.0

/*
 * RFC 5869 Key-derivation function
 *
 * Copyright (C) 2019, Stephan Mueller <smueller@chronox.de>
 */

/*
 * The HKDF extract phase is applied with crypto_rng_reset().
 * The HKDF expand phase is applied with crypto_rng_generate().
 *
 * NOTE: In-place cipher operations are not supported.
 */

#include <linux/module.h>
#include <crypto/rng.h>
#include <crypto/internal/rng.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <linux/rtnetlink.h>

struct crypto_hkdf_ctx {
	struct crypto_shash *extract_kmd;
	struct crypto_shash *expand_kmd;
};

#define CRYPTO_HKDF_MAX_DIGESTSIZE	64

/*
 * HKDF expand phase
 */
static int crypto_hkdf_random(struct crypto_rng *rng,
			      const u8 *info, unsigned int infolen,
			      u8 *dst, unsigned int dlen)
{
	struct crypto_hkdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	struct crypto_shash *expand_kmd = ctx->expand_kmd;
	SHASH_DESC_ON_STACK(desc, expand_kmd);
	unsigned int h = crypto_shash_digestsize(expand_kmd);
	int err = 0;
	u8 *dst_orig = dst;
	const u8 *prev = NULL;
	uint8_t ctr = 0x01;

	if (dlen > h * 255)
		return -EINVAL;

	desc->tfm = expand_kmd;
	desc->flags = crypto_shash_get_flags(expand_kmd) &
		      CRYPTO_TFM_REQ_MAY_SLEEP;

	/* T(1) and following */
	while (dlen) {
		err = crypto_shash_init(desc);
		if (err)
			goto out;

		if (prev) {
			err = crypto_shash_update(desc, prev, h);
			if (err)
				goto out;
		}

		if (info) {
			err = crypto_shash_update(desc, info, infolen);
			if (err)
				goto out;
		}

		if (dlen < h) {
			u8 tmpbuffer[CRYPTO_HKDF_MAX_DIGESTSIZE];

			err = crypto_shash_finup(desc, &ctr, 1, tmpbuffer);
			if (err)
				goto out;
			memcpy(dst, tmpbuffer, dlen);
			memzero_explicit(tmpbuffer, h);
			goto out;
		} else {
			err = crypto_shash_finup(desc, &ctr, 1, dst);
			if (err)
				goto out;

			prev = dst;
			dst += h;
			dlen -= h;
			ctr++;
		}
	}

out:
	if (err)
		memzero_explicit(dst_orig, dlen);
	shash_desc_zero(desc);
	return err;
}

/*
 * HKDF extract phase.
 *
 * The seed is defined to be a concatenation of the salt and the IKM.
 * The data buffer is pre-pended by an rtattr which provides an u32 value
 * with the length of the salt. Thus, the buffer length - salt length is the
 * IKM length.
 */
static int crypto_hkdf_seed(struct crypto_rng *rng,
			    const u8 *seed, unsigned int slen)
{
	struct crypto_hkdf_ctx *ctx = crypto_tfm_ctx(crypto_rng_tfm(rng));
	struct crypto_shash *extract_kmd = ctx->extract_kmd;
	struct crypto_shash *expand_kmd = ctx->expand_kmd;
	struct rtattr *rta = (struct rtattr *)seed;
	SHASH_DESC_ON_STACK(desc, extract_kmd);
	u32 saltlen;
	unsigned int h = crypto_shash_digestsize(extract_kmd);
	int err;
	const uint8_t null_salt[CRYPTO_HKDF_MAX_DIGESTSIZE] = { 0 };
	u8 prk[CRYPTO_HKDF_MAX_DIGESTSIZE] = { 0 };

	/* Require aligned buffer to directly read out saltlen below */
	if (WARN_ON((unsigned long)seed & (sizeof(saltlen) - 1)))
		return -EINVAL;

	if (!RTA_OK(rta, slen))
		return -EINVAL;
	if (rta->rta_type != 1)
		return -EINVAL;
	if (RTA_PAYLOAD(rta) < sizeof(saltlen))
		return -EINVAL;
	saltlen = *((u32 *)RTA_DATA(rta));

	seed += RTA_ALIGN(rta->rta_len);
	slen -= RTA_ALIGN(rta->rta_len);

	if (slen < saltlen)
		return -EINVAL;

	desc->tfm = extract_kmd;

	/* Set the salt as HMAC key */
	if (saltlen)
		err = crypto_shash_setkey(extract_kmd, seed, saltlen);
	else
		err = crypto_shash_setkey(extract_kmd, null_salt, h);
	if (err)
		return err;

	/* Extract the PRK */
	err = crypto_shash_digest(desc, seed + saltlen, slen - saltlen, prk);
	if (err)
		goto err;

	/* Set the PRK for the expand phase */
	err = crypto_shash_setkey(expand_kmd, prk, h);
	if (err)
		goto err;

err:
	shash_desc_zero(desc);
	memzero_explicit(prk, h);
	return err;
}

static int crypto_hkdf_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_instance *inst = crypto_tfm_alg_instance(tfm);
	struct crypto_shash_spawn *spawn = crypto_instance_ctx(inst);
	struct crypto_hkdf_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_shash *extract_kmd = NULL, *expand_kmd = NULL;
	unsigned int ds;

	extract_kmd = crypto_spawn_shash(spawn);
	if (IS_ERR(extract_kmd))
		return PTR_ERR(extract_kmd);

	expand_kmd = crypto_spawn_shash(spawn);
	if (IS_ERR(expand_kmd)) {
		crypto_free_shash(extract_kmd);
		return PTR_ERR(expand_kmd);
	}

	ds = crypto_shash_digestsize(extract_kmd);
	/* Hashes with no digest size are not allowed for KDFs. */
	if (!ds || ds > CRYPTO_HKDF_MAX_DIGESTSIZE) {
		crypto_free_shash(extract_kmd);
		crypto_free_shash(expand_kmd);
		return -EOPNOTSUPP;
	}

	ctx->extract_kmd = extract_kmd;
	ctx->expand_kmd = expand_kmd;

	return 0;
}

static void crypto_hkdf_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_hkdf_ctx *ctx = crypto_tfm_ctx(tfm);

	crypto_free_shash(ctx->extract_kmd);
	crypto_free_shash(ctx->expand_kmd);
}

static void crypto_kdf_free(struct rng_instance *inst)
{
	crypto_drop_spawn(rng_instance_ctx(inst));
	kfree(inst);
}

static int crypto_hkdf_create(struct crypto_template *tmpl, struct rtattr **tb)
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

	inst = rng_alloc_instance("hkdf", alg);
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

	inst->alg.generate		= crypto_hkdf_random;
	inst->alg.seed			= crypto_hkdf_seed;
	inst->alg.seedsize		= ds;

	inst->alg.base.cra_init		= crypto_hkdf_init_tfm;
	inst->alg.base.cra_exit		= crypto_hkdf_exit_tfm;
	inst->alg.base.cra_ctxsize	= ALIGN(sizeof(struct crypto_hkdf_ctx) +
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

static struct crypto_template crypto_hkdf_tmpl = {
	.name = "hkdf",
	.create = crypto_hkdf_create,
	.module = THIS_MODULE,
};

static int __init crypto_hkdf_init(void)
{
	return crypto_register_template(&crypto_hkdf_tmpl);
}

static void __exit crypto_hkdf_exit(void)
{
	crypto_unregister_template(&crypto_hkdf_tmpl);
}

module_init(crypto_hkdf_init);
module_exit(crypto_hkdf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Key Derivation Function according to RFC 5869");
MODULE_ALIAS_CRYPTO("hkdf");
