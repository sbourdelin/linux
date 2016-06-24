/*
 * ECDH helper functions - KPP wrappings
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 * CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS,
 * COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS
 * SOFTWARE IS DISCLAIMED.
 */
#include "ecdh_helper.h"

#include <linux/random.h>
#include <linux/scatterlist.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>

struct ecdh_completion {
	struct completion completion;
	int err;
};

static void ecdh_complete(struct crypto_async_request *req, int err)
{
	struct ecdh_completion *res = req->data;

	if (err == -EINPROGRESS)
		return;

	res->err = err;
	complete(&res->completion);
}

static inline void swap_digits(u64 *in, u64 *out, unsigned int ndigits)
{
	int i;

	for (i = 0; i < ndigits; i++)
		out[i] = __swab64(in[ndigits - 1 - i]);
}

bool compute_ecdh_secret(const u8 public_key[64], const u8 private_key[32],
			 u8 secret[32])
{
	struct crypto_kpp *tfm;
	struct kpp_request *req;
	struct ecdh p;
	struct ecdh_completion result;
	struct scatterlist src, dst;
	char *buf;
	unsigned int len = 0;
	u8 tmp[64];
	int err = -ENOMEM;

	tfm = crypto_alloc_kpp("ecdh", CRYPTO_ALG_INTERNAL, 0);
	if (IS_ERR(tfm)) {
		pr_err("alg: kpp: Failed to load tfm for kpp: %ld\n",
		       PTR_ERR(tfm));
		return false;
	}

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		goto free_kpp;

	init_completion(&result.completion);

	/* Set curve_id */
	p.curve_id = ECC_CURVE_NIST_P256;
	/* Security Manager Protocol holds digits in litte-endian order
	 * while ECC API expect big-endian data
	 */
	swap_digits((u64 *)private_key, (u64 *)tmp, 4);
	p.key = tmp;
	p.key_size = 32;
	len = crypto_ecdh_key_len(&p);
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		goto free_req;

	/* Set A private Key */
	crypto_ecdh_encode_key(buf, len, &p);
	err = crypto_kpp_set_secret(tfm, buf, len);
	if (err)
		goto free_buf;

	swap_digits((u64 *)public_key, (u64 *)tmp, 4); /* x */
	swap_digits((u64 *)&public_key[32], (u64 *)&tmp[32], 4); /* y */

	sg_init_one(&src, tmp, 64);
	sg_init_one(&dst, secret, 32);
	kpp_request_set_input(req, &src, 64);
	kpp_request_set_output(req, &dst, 32);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 ecdh_complete, &result);
	err = crypto_kpp_compute_shared_secret(req);
	if (err == -EINPROGRESS) {
		wait_for_completion(&result.completion);
		err = result.err;
	}
	if (err < 0) {
		pr_err("alg: ecdh: compute shard secret test failed. err %d\n",
		       err);
		goto free_buf;
	}

	swap_digits((u64 *)secret, (u64 *)tmp, 4);
	memcpy(secret, tmp, 32);

free_buf:
	kfree(buf);
free_req:
	kpp_request_free(req);
free_kpp:
	crypto_free_kpp(tfm);
	return (err == 0);
}

bool generate_ecdh_keys(u8 public_key[64], u8 private_key[32])
{
	struct crypto_kpp *tfm;
	struct kpp_request *req;
	struct ecdh p;
	struct ecdh_completion result;
	struct scatterlist dst;
	char *buf;
	unsigned int len;
	u8 tmp[64];
	int err = -ENOMEM;
	const unsigned short max_tries = 16;
	unsigned short tries = 0;

	tfm = crypto_alloc_kpp("ecdh", CRYPTO_ALG_INTERNAL, 0);
	if (IS_ERR(tfm)) {
		pr_err("alg: kpp: Failed to load tfm for kpp: %ld\n",
		       PTR_ERR(tfm));
		return false;
	}

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		goto free_tfm;

	init_completion(&result.completion);

	p.curve_id = ECC_CURVE_NIST_P256;
	get_random_bytes(private_key, 32);
	p.key = private_key;
	p.key_size = 32;
	len = crypto_ecdh_key_len(&p);
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		goto free_req;

	do {
		crypto_ecdh_encode_key(buf, len, &p);
		err = crypto_kpp_set_secret(tfm, (void *)private_key, 32);
		/* Private key is not valid. Regenerate */
		if (err == -EINVAL) {
			if (tries++ >= max_tries)
				goto free_buf;

			get_random_bytes(private_key, 32);
			p.key = private_key;
			continue;
		}

		if (err)
			goto free_buf;
		else
			break;

	} while (true);

	sg_init_one(&dst, tmp, 64);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dst, 64);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 ecdh_complete, &result);

	err = crypto_kpp_generate_public_key(req);
	if (err == -EINPROGRESS) {
		wait_for_completion(&result.completion);
		err = result.err;
	}
	if (err < 0)
		goto free_buf;

	/* Keys are handed back in little endian as expected by Security
	 * Manager Protocol
	 */
	swap_digits((u64 *)tmp, (u64 *)public_key, 4); /* x */
	swap_digits((u64 *)&tmp[32], (u64 *)&public_key[32], 4); /* y */
	swap_digits((u64 *)private_key, (u64 *)tmp, 4);
	memcpy(private_key, tmp, 32);

free_buf:
	kfree(buf);
free_req:
	kpp_request_free(req);
free_tfm:
	crypto_free_kpp(tfm);
	return (err == 0);
}
