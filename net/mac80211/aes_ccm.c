/*
 * Copyright 2003-2004, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 *
 * Rewrite: Copyright (C) 2013 Linaro Ltd <ard.biesheuvel@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/err.h>
#include <crypto/aead.h>

#include <net/mac80211.h>
#include "key.h"
#include "aes_ccm.h"

int ieee80211_aes_ccm_encrypt(struct ieee80211_ccmp_aead *ccmp, u8 *b_0,
			      u8 *aad, u8 *data, size_t data_len, u8 *mic,
			      size_t mic_len)
{
	struct scatterlist sg[3];
	struct aead_request *aead_req;
	int reqsize = sizeof(*aead_req) + crypto_aead_reqsize(ccmp->tfm);
	u8 *__aad;

	aead_req = *this_cpu_ptr(ccmp->reqs);
	if (!aead_req) {
		aead_req = kzalloc(reqsize + CCM_AAD_LEN, GFP_ATOMIC);
		if (!aead_req)
			return -ENOMEM;
		*this_cpu_ptr(ccmp->reqs) = aead_req;
		aead_request_set_tfm(aead_req, ccmp->tfm);
	}

	__aad = (u8 *)aead_req + reqsize;
	memcpy(__aad, aad, CCM_AAD_LEN);

	sg_init_table(sg, 3);
	sg_set_buf(&sg[0], &__aad[2], be16_to_cpup((__be16 *)__aad));
	sg_set_buf(&sg[1], data, data_len);
	sg_set_buf(&sg[2], mic, mic_len);

	aead_request_set_crypt(aead_req, sg, sg, data_len, b_0);
	aead_request_set_ad(aead_req, sg[0].length);

	crypto_aead_encrypt(aead_req);

	return 0;
}

int ieee80211_aes_ccm_decrypt(struct ieee80211_ccmp_aead *ccmp, u8 *b_0,
			      u8 *aad, u8 *data, size_t data_len, u8 *mic,
			      size_t mic_len)
{
	struct scatterlist sg[3];
	struct aead_request *aead_req;
	int reqsize = sizeof(*aead_req) + crypto_aead_reqsize(ccmp->tfm);
	u8 *__aad;

	if (data_len == 0)
		return -EINVAL;

	aead_req = *this_cpu_ptr(ccmp->reqs);
	if (!aead_req) {
		aead_req = kzalloc(reqsize + CCM_AAD_LEN, GFP_ATOMIC);
		if (!aead_req)
			return -ENOMEM;
		*this_cpu_ptr(ccmp->reqs) = aead_req;
		aead_request_set_tfm(aead_req, ccmp->tfm);
	}

	__aad = (u8 *)aead_req + reqsize;
	memcpy(__aad, aad, CCM_AAD_LEN);

	sg_init_table(sg, 3);
	sg_set_buf(&sg[0], &__aad[2], be16_to_cpup((__be16 *)__aad));
	sg_set_buf(&sg[1], data, data_len);
	sg_set_buf(&sg[2], mic, mic_len);

	aead_request_set_crypt(aead_req, sg, sg, data_len + mic_len, b_0);
	aead_request_set_ad(aead_req, sg[0].length);

	return crypto_aead_decrypt(aead_req);
}

int ieee80211_aes_key_setup_encrypt(struct ieee80211_ccmp_aead *ccmp,
				    const u8 key[],
				    size_t key_len,
				    size_t mic_len)
{
	struct crypto_aead *tfm;
	struct aead_request **r;
	int err;

	tfm = crypto_alloc_aead("ccm(aes)", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	err = crypto_aead_setkey(tfm, key, key_len);
	if (err)
		goto free_aead;
	err = crypto_aead_setauthsize(tfm, mic_len);
	if (err)
		goto free_aead;

	/* allow a struct aead_request to be cached per cpu */
	r = alloc_percpu(struct aead_request *);
	if (!r) {
		err = -ENOMEM;
		goto free_aead;
	}

	ccmp->reqs = r;
	ccmp->tfm = tfm;
	return 0;

free_aead:
	crypto_free_aead(tfm);
	return err;
}

void ieee80211_aes_key_free(struct ieee80211_ccmp_aead *ccmp)
{
	struct aead_request *req;
	int cpu;

	for_each_possible_cpu(cpu) {
		req = *per_cpu_ptr(ccmp->reqs, cpu);
		if (req)
			kzfree(req);
	}
	free_percpu(ccmp->reqs);
	crypto_free_aead(ccmp->tfm);
}
