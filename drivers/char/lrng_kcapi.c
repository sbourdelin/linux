/*
 * Backend for the LRNG providing the cryptographic primitives using the
 * kernel crypto API.
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

#include <crypto/drbg.h>

struct lrng_hash_info {
	struct shash_desc shash;
	char ctx[];
};

int lrng_drng_seed_helper(void *drng, const u8 *inbuf, u32 inbuflen)
{
	struct drbg_state *drbg = (struct drbg_state *)drng;
	LIST_HEAD(seedlist);
	struct drbg_string data;
	int ret;

	drbg_string_fill(&data, inbuf, inbuflen);
	list_add_tail(&data.list, &seedlist);
	ret = drbg->d_ops->update(drbg, &seedlist, drbg->seeded);

	if (ret >= 0)
		drbg->seeded = true;

	return ret;
}

int lrng_drng_generate_helper(void *drng, u8 *outbuf, u32 outbuflen)
{
	struct drbg_state *drbg = (struct drbg_state *)drng;

	return drbg->d_ops->generate(drbg, outbuf, outbuflen, NULL);
}

int lrng_drng_generate_helper_full(void *drng, u8 *outbuf, u32 outbuflen)
{
	struct drbg_state *drbg = (struct drbg_state *)drng;

	return drbg->d_ops->generate(drbg, outbuf, outbuflen, NULL);
}

void *lrng_drng_alloc(const u8 *drng_name, u32 sec_strength)
{
	struct drbg_state *drbg = NULL;
	int coreref = -1;
	bool pr = false;
	int ret;

	drbg_convert_tfm_core(drng_name, &coreref, &pr);
	if (coreref < 0)
		return ERR_PTR(-EFAULT);

	drbg = kzalloc(sizeof(struct drbg_state), GFP_KERNEL);
	if (!drbg)
		return ERR_PTR(-ENOMEM);

	drbg->core = &drbg_cores[coreref];
	drbg->seeded = false;
	ret = drbg_alloc_state(drbg);
	if (ret)
		goto err;

	if (sec_strength > drbg_sec_strength(drbg->core->flags))
		goto dealloc;

	pr_info("DRBG with %s core allocated\n", drbg->core->backend_cra_name);

	return drbg;

dealloc:
	if (drbg->d_ops)
		drbg->d_ops->crypto_fini(drbg);
	drbg_dealloc_state(drbg);
err:
	kfree(drbg);
	return ERR_PTR(-EINVAL);
}

void lrng_drng_dealloc(void *drng)
{
	struct drbg_state *drbg = (struct drbg_state *)drng;

	drbg_dealloc_state(drbg);
	kzfree(drbg);
}

void *lrng_hash_alloc(const u8 *hashname, const u8 *key, u32 keylen)
{
	struct lrng_hash_info *lrng_hash;
	struct crypto_shash *tfm;
	int size, ret;

	tfm = crypto_alloc_shash(hashname, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("could not allocate hash %s\n", hashname);
		return ERR_CAST(tfm);
	}

	size = sizeof(struct lrng_hash_info) + crypto_shash_descsize(tfm);
	lrng_hash = kmalloc(size, GFP_KERNEL);
	if (!lrng_hash) {
		crypto_free_shash(tfm);
		return ERR_PTR(-ENOMEM);
	}

	lrng_hash->shash.tfm = tfm;
	lrng_hash->shash.flags = 0x0;

	/* If the used hash is no MAC, ignore the ENOSYS return code */
	ret = crypto_shash_setkey(tfm, key, keylen);
	if (ret && ret != -ENOSYS) {
		pr_err("could not set the key for MAC\n");
		crypto_free_shash(tfm);
		kfree(lrng_hash);
		return ERR_PTR(ret);
	}

	return lrng_hash;
}

u32 lrng_hash_digestsize(void *hash)
{
	struct lrng_hash_info *lrng_hash = (struct lrng_hash_info *)hash;
	struct shash_desc *shash = &lrng_hash->shash;

	return crypto_shash_digestsize(shash->tfm);
}

int lrng_hash_buffer(void *hash, const u8 *inbuf, u32 inbuflen, u8 *digest)
{
	struct lrng_hash_info *lrng_hash = (struct lrng_hash_info *)hash;
	struct shash_desc *shash = &lrng_hash->shash;

	return crypto_shash_digest(shash, inbuf, inbuflen, digest);
}
