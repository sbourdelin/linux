/*
 * RNG: Random Number Generator  algorithms under the crypto API
 *
 * Copyright (c) 2008 Neil Horman <nhorman@tuxdriver.com>
 * Copyright (c) 2015 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#ifndef _CRYPTO_INTERNAL_RNG_H
#define _CRYPTO_INTERNAL_RNG_H

#include <crypto/algapi.h>
#include <crypto/rng.h>

int crypto_register_rng(struct rng_alg *alg);
void crypto_unregister_rng(struct rng_alg *alg);
int crypto_register_rngs(struct rng_alg *algs, int count);
void crypto_unregister_rngs(struct rng_alg *algs, int count);

#if defined(CONFIG_CRYPTO_RNG) || defined(CONFIG_CRYPTO_RNG_MODULE)
int crypto_del_default_rng(void);
#else
static inline int crypto_del_default_rng(void)
{
	return 0;
}
#endif

static inline void *crypto_rng_ctx(struct crypto_rng *tfm)
{
	return crypto_tfm_ctx(&tfm->base);
}

static inline void crypto_rng_set_entropy(struct crypto_rng *tfm,
					  const u8 *data, unsigned int len)
{
	crypto_rng_alg(tfm)->set_ent(tfm, data, len);
}

struct rng_instance {
	void (*free)(struct rng_instance *inst);
	struct rng_alg alg;
};

static inline struct rng_instance *rng_alloc_instance(
	const char *name, struct crypto_alg *alg)
{
	return crypto_alloc_instance(name, alg,
			      sizeof(struct rng_instance) - sizeof(*alg));
}

static inline struct crypto_instance *rng_crypto_instance(
	struct rng_instance *inst)
{
	return container_of(&inst->alg.base, struct crypto_instance, alg);
}

static inline void *rng_instance_ctx(struct rng_instance *inst)
{
	return crypto_instance_ctx(rng_crypto_instance(inst));
}

int rng_register_instance(struct crypto_template *tmpl,
			  struct rng_instance *inst);

#endif
