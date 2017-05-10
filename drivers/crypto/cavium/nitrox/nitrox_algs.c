#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/ctr.h>
#include <crypto/des.h>
#include <crypto/xts.h>

#include "nitrox_dev.h"
#include "nitrox_common.h"
#include "nitrox_req.h"

struct nitrox_cipher {
	const char *name;
	enum flexi_cipher value;
};

/**
 * supported cipher list
 */
static const struct nitrox_cipher flexi_cipher_table[] = {
	{ "null",		CIPHER_NULL },
	{ "cbc(des3_ede)",	CIPHER_3DES_CBC },
	{ "ecb(des3_ede)",	CIPHER_3DES_ECB },
	{ "cbc(aes)",		CIPHER_AES_CBC },
	{ "ecb(aes)",		CIPHER_AES_ECB },
	{ "cfb(aes)",		CIPHER_AES_CFB },
	{ "rfc3686(ctr(aes))",	CIPHER_AES_CTR },
	{ "xts(aes)",		CIPHER_AES_XTS },
	{ "cts(cbc(aes))",	CIPHER_AES_CBC_CTS },
	{ NULL,			CIPHER_INVALID }
};

static enum flexi_cipher flexi_cipher_type(const char *name)
{
	const struct nitrox_cipher *cipher = flexi_cipher_table;

	while (cipher->name) {
		if (!strcmp(cipher->name, name))
			break;
		cipher++;
	}
	return cipher->value;
}

static int flexi_aes_keylen(int keylen)
{
	int aes_keylen;

	switch (keylen) {
	case AES_KEYSIZE_128:
		aes_keylen = 1;
		break;
	case AES_KEYSIZE_192:
		aes_keylen = 2;
		break;
	case AES_KEYSIZE_256:
		aes_keylen = 3;
		break;
	default:
		aes_keylen = -EINVAL;
		break;
	}
	return aes_keylen;
}

static inline void create_io_list(struct scatterlist *src,
				  struct io_sglist *io)
{
	struct scatterlist *sg;
	int cnt, sgcount, i;

	cnt = io->cnt;
	sgcount = sg_nents(src);

	for_each_sg(src, sg, sgcount, i) {
		if (!sg->length)
			continue;
		io->bufs[cnt].addr = sg_virt(sg);
		io->bufs[cnt].len = sg->length;
		cnt++;
	}
	io->cnt = cnt;
}

static int nitrox_ablkcipher_init(struct crypto_tfm *tfm)
{
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);
	void *ctx;

	tfm->crt_ablkcipher.reqsize = sizeof(struct nitrox_crypto_request);
	/* get the first device */
	inst->ndev = nitrox_get_first_device();
	if (!inst->ndev)
		return -ENODEV;

	/* allocate crypto context */
	ctx = crypto_alloc_context(inst->ndev);
	if (!ctx) {
		nitrox_put_device(inst->ndev);
		return -ENOMEM;
	}
	inst->u.ctx_handle = (uintptr_t)ctx;

	return 0;
}

static void nitrox_ablkcipher_exit(struct crypto_tfm *tfm)
{
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);

	/* free the crypto context */
	if (inst->u.ctx_handle)
		crypto_free_context((void *)inst->u.ctx_handle);

	nitrox_put_device(inst->ndev);

	inst->u.ctx_handle = 0;
	inst->ndev = NULL;
}

static inline int nitrox_ablkcipher_setkey(struct crypto_ablkcipher *cipher,
					   int aes_keylen, const u8 *key,
					   unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);
	struct flexi_crypto_context *fctx;
	enum flexi_cipher cipher_type;
	const char *name;

	name = crypto_tfm_alg_name(tfm);
	cipher_type = flexi_cipher_type(name);
	if (cipher_type == CIPHER_INVALID) {
		pr_err("unsupported cipher: %s\n", name);
		return -EINVAL;
	}

	/* fill crypto context */
	fctx = inst->u.fctx;
	fctx->flags = 0;
	fctx->w0.cipher_type = cipher_type;
	fctx->w0.aes_keylen = aes_keylen;
	fctx->w0.iv_source = IV_FROM_DPTR;
	fctx->flags = cpu_to_be64(*(u64 *)&fctx->w0);
	/* copy the key to context */
	memcpy(fctx->crypto.u.key, key, keylen);

	return 0;
}

static int nitrox_aes_setkey(struct crypto_ablkcipher *cipher, const u8 *key,
			     unsigned int keylen)
{
	int aes_keylen;

	aes_keylen = flexi_aes_keylen(keylen);
	if (aes_keylen < 0) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}
	return nitrox_ablkcipher_setkey(cipher, aes_keylen, key, keylen);
}

static void nitrox_ablkcipher_alg_callback(int status, void *arg)
{
	struct nitrox_crypto_request *nkreq = arg;
	struct ablkcipher_request *areq = nkreq->abreq;
	struct crypto_ablkcipher *cipher = crypto_ablkcipher_reqtfm(areq);
	int ivsize = crypto_ablkcipher_ivsize(cipher);
	struct crypto_request *creq = &nkreq->creq;

	/* copy the iv back */
	memcpy(areq->info, creq->out->bufs[0].addr, ivsize);

	kfree(creq->in->bufs[0].addr);
	kfree(creq->in);
	kfree(creq->out->bufs[0].addr);
	kfree(creq->out);

	if (status) {
		pr_err_ratelimited("request failed status 0x%0x\n", status);
		status = -EINVAL;
	}

	areq->base.complete(&areq->base, status);
}

static inline int create_crypt_input_list(struct ablkcipher_request *areq,
					  struct crypto_request *creq)
{
	struct crypto_ablkcipher *cipher = crypto_ablkcipher_reqtfm(areq);
	int ivsize = crypto_ablkcipher_ivsize(cipher);
	struct io_sglist *in;
	size_t sz;
	gfp_t gfp;

	/* one extra entry for IV */
	sz = sizeof(*in) +
		(1 + sg_nents(areq->src)) * sizeof(struct nitrox_buffer);

	gfp = (areq->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
		GFP_KERNEL : GFP_ATOMIC;

	in = kzalloc(sz, gfp);
	if (!in)
		return -ENOMEM;

	in->bufs[0].addr = kmalloc(ivsize, gfp);
	if (!in->bufs[0].addr) {
		kfree(in);
		return -ENOMEM;
	}
	creq->in = in;
	/* copy iv */
	memcpy(in->bufs[0].addr, areq->info, ivsize);
	in->bufs[0].len = ivsize;
	in->cnt++;

	create_io_list(areq->src, in);
	return 0;
}

static inline int create_crypt_output_list(struct ablkcipher_request *areq,
					   struct crypto_request *creq)
{
	struct crypto_ablkcipher *cipher = crypto_ablkcipher_reqtfm(areq);
	int ivsize = crypto_ablkcipher_ivsize(cipher);
	struct io_sglist *out;
	size_t sz;
	gfp_t gfp;

	/* one extra entry for IV */
	sz = sizeof(*out) +
		(1 + sg_nents(areq->dst)) * sizeof(struct nitrox_buffer);

	gfp = (areq->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
		GFP_KERNEL : GFP_ATOMIC;

	out = kzalloc(sz, gfp);
	if (!out)
		return -ENOMEM;

	/* place for iv */
	out->bufs[0].addr = kzalloc(ivsize, gfp);
	if (!out->bufs[0].addr) {
		kfree(out);
		return -ENOMEM;
	}
	creq->out = out;
	out->bufs[0].len = ivsize;
	out->cnt++;

	create_io_list(areq->dst, out);
	return 0;
}

static int nitrox_ablkcipher_crypt(struct ablkcipher_request *areq, bool enc)
{
	struct crypto_ablkcipher *cipher = crypto_ablkcipher_reqtfm(areq);
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);
	struct nitrox_crypto_request *nkreq = ablkcipher_request_ctx(areq);
	int ivsize = crypto_ablkcipher_ivsize(cipher);
	struct crypto_request *creq;
	int ret;

	creq = &nkreq->creq;
	/* fill the request */
	creq->ctrl.value = 0;
	creq->opcode = FLEXI_CRYPTO_ENCRYPT_HMAC;
	creq->ctrl.s.arg = (enc ? ENCRYPT : DECRYPT);
	/* param0: length of the data to be encrypted */
	creq->gph.param0 = cpu_to_be16(areq->nbytes);
	creq->gph.param1 = 0;
	/* param2: encryption data offset */
	creq->gph.param2 = cpu_to_be16(ivsize);
	creq->gph.param3 = 0;

	creq->ctx_handle = inst->u.ctx_handle;
	creq->ctrl.s.ctxl = sizeof(struct flexi_crypto_context);

	ret = create_crypt_input_list(areq, creq);
	if (ret)
		return ret;

	ret = create_crypt_output_list(areq, creq);
	if (ret) {
		kfree(creq->in);
		return ret;
	}

	nkreq->inst = inst;
	nkreq->abreq = areq;
	creq->callback = nitrox_ablkcipher_alg_callback;
	creq->cb_arg = nkreq;
	creq->flags = areq->base.flags;

	/* send the crypto request */
	ret = nitrox_se_request(inst->ndev, creq);
	if (ret)
		return ret;

	return -EINPROGRESS;
}

static int nitrox_aes_encrypt(struct ablkcipher_request *areq)
{
	return nitrox_ablkcipher_crypt(areq, true);
}

static int nitrox_aes_decrypt(struct ablkcipher_request *areq)
{
	return nitrox_ablkcipher_crypt(areq, false);
}

static int nitrox_3des_setkey(struct crypto_ablkcipher *cipher, const u8 *key,
			      unsigned int keylen)
{
	if (keylen != DES3_EDE_KEY_SIZE) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}

	return nitrox_ablkcipher_setkey(cipher, 0, key, keylen);
}

static int nitrox_3des_encrypt(struct ablkcipher_request *areq)
{
	return nitrox_ablkcipher_crypt(areq, true);
}

static int nitrox_3des_decrypt(struct ablkcipher_request *areq)
{
	return nitrox_ablkcipher_crypt(areq, false);
}

static int nitrox_aes_xts_setkey(struct crypto_ablkcipher *cipher,
				 const u8 *key, unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);
	struct flexi_crypto_context *fctx;
	int aes_keylen, ret;

	ret = xts_check_key(tfm, key, keylen);
	if (ret)
		return ret;

	keylen /= 2;

	aes_keylen = flexi_aes_keylen(keylen);
	if (aes_keylen < 0) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}

	fctx = inst->u.fctx;
	/* copy KEY2 */
	memcpy(fctx->auth.u.key2, (key + keylen), keylen);

	return nitrox_ablkcipher_setkey(cipher, aes_keylen, key, keylen);
}

static int nitrox_aes_ctr_rfc3686_setkey(struct crypto_ablkcipher *cipher,
					 const u8 *key, unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct nitrox_crypto_instance *inst = crypto_tfm_ctx(tfm);
	struct flexi_crypto_context *fctx;
	int aes_keylen;

	if (keylen < CTR_RFC3686_NONCE_SIZE)
		return -EINVAL;

	fctx = inst->u.fctx;

	memcpy(fctx->crypto.iv, key + (keylen - CTR_RFC3686_NONCE_SIZE),
	       CTR_RFC3686_NONCE_SIZE);

	keylen -= CTR_RFC3686_NONCE_SIZE;

	aes_keylen = flexi_aes_keylen(keylen);
	if (aes_keylen < 0) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}
	return nitrox_ablkcipher_setkey(cipher, aes_keylen, key, keylen);
}

static struct crypto_alg nitrox_algs[] = { {
	.cra_name = "cbc(aes)",
	.cra_driver_name = "n5_cbc(aes)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "ecb(aes)",
	.cra_driver_name = "n5_ecb(aes)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "cfb(aes)",
	.cra_driver_name = "n5_cfb(aes)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "cbc(des3_ede)",
	.cra_driver_name = "n5_cbc(des3_ede)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = DES3_EDE_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_3des_setkey,
			.decrypt = nitrox_3des_decrypt,
			.encrypt = nitrox_3des_encrypt,
			.min_keysize = DES3_EDE_KEY_SIZE,
			.max_keysize = DES3_EDE_KEY_SIZE,
			.ivsize = DES3_EDE_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "ecb(des3_ede)",
	.cra_driver_name = "n5_ecb(des3_ede)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = DES3_EDE_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_3des_setkey,
			.decrypt = nitrox_3des_decrypt,
			.encrypt = nitrox_3des_encrypt,
			.min_keysize = DES3_EDE_KEY_SIZE,
			.max_keysize = DES3_EDE_KEY_SIZE,
			.ivsize = DES3_EDE_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "xts(aes)",
	.cra_driver_name = "n5_xts(aes)",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_xts_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = 2 * AES_MIN_KEY_SIZE,
			.max_keysize = 2 * AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
		},
	},
}, {
	.cra_name = "rfc3686(ctr(aes))",
	.cra_driver_name = "n5_rfc3686(ctr(aes))",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = 1,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_ctr_rfc3686_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = AES_MIN_KEY_SIZE +
					CTR_RFC3686_NONCE_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE +
					CTR_RFC3686_NONCE_SIZE,
			.ivsize = CTR_RFC3686_IV_SIZE,
		},
	},
}, {
	.cra_name = "cts(cbc(aes))",
	.cra_driver_name = "n5_cts(cbc(aes))",
	.cra_priority = 4001,
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct nitrox_crypto_instance),
	.cra_alignmask = 0,
	.cra_type = &crypto_ablkcipher_type,
	.cra_module = THIS_MODULE,
	.cra_init = nitrox_ablkcipher_init,
	.cra_exit = nitrox_ablkcipher_exit,
	.cra_u = {
		.ablkcipher = {
			.setkey = nitrox_aes_setkey,
			.decrypt = nitrox_aes_decrypt,
			.encrypt = nitrox_aes_encrypt,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
		},
	},
}

};

int nitrox_crypto_register(void)
{
	return crypto_register_algs(nitrox_algs, ARRAY_SIZE(nitrox_algs));
}

void nitrox_crypto_unregister(void)
{
	crypto_unregister_algs(nitrox_algs, ARRAY_SIZE(nitrox_algs));
}
