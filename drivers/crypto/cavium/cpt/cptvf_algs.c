
/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/crypto.h>
#include <crypto/algapi.h>
#include <crypto/cryptd.h>
#include <crypto/crypto_wq.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <crypto/aes.h>
#include <crypto/authenc.h>
#include <crypto/des.h>

#include "request_manager.h"
#include "cptvf.h"
#include "cptvf_algs.h"

struct cpt_device_handle {
	void *cdev[MAX_DEVICES];
	u32 dev_count;
};

static struct cpt_device_handle dev_handle;

static void cvm_callback(u32 status, void *arg)
{
	struct crypto_async_request *req = (struct crypto_async_request *)arg;

	req->complete(req, !status);
}

static inline void update_input_iv(struct cpt_request_info *req_info,
				   u8 *iv, u32 enc_iv_len,
				   u32 *argcnt)
{
	/* Setting the iv information */
	req_info->in[*argcnt].vptr = (void *)iv;
	req_info->in[*argcnt].size = enc_iv_len;
	req_info->req.dlen += enc_iv_len;

	++(*argcnt);
}

static inline void update_output_iv(struct cpt_request_info *req_info,
				    u8 *iv, u32 enc_iv_len,
				    u32 *argcnt)
{
	/* Setting the iv information */
	req_info->out[*argcnt].vptr = (void *)iv;
	req_info->out[*argcnt].size = enc_iv_len;
	req_info->rlen += enc_iv_len;

	++(*argcnt);
}

static inline void update_input_data(struct cpt_request_info *req_info,
				     struct scatterlist *inp_sg,
				     u32 nbytes, u32 *argcnt)
{
	req_info->req.dlen += nbytes;

	while (nbytes) {
		u32 len = min(nbytes, inp_sg->length);
		u8 *ptr = page_address(sg_page(inp_sg)) + inp_sg->offset;

		req_info->in[*argcnt].vptr = (void *)ptr;
		req_info->in[*argcnt].size = len;
		nbytes -= len;

		++(*argcnt);
		++inp_sg;
	}
}

static inline void update_output_data(struct cpt_request_info *req_info,
				      struct scatterlist *outp_sg,
				      u32 nbytes, u32 *argcnt)
{
	req_info->rlen += nbytes;

	while (nbytes) {
		u32 len = min(nbytes, outp_sg->length);
		u8 *ptr = page_address(sg_page(outp_sg)) +
					    outp_sg->offset;

		req_info->out[*argcnt].vptr = (void *)ptr;
		req_info->out[*argcnt].size = len;
		nbytes -= len;
		++(*argcnt);
		++outp_sg;
	}
}

static inline u32 create_ctx_hdr(struct ablkcipher_request *req, u32 enc,
				 u32 cipher_type, u32 aes_key_type,
				 u32 *argcnt)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct cvm_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct fc_context *fctx = &rctx->fctx;
	u64 *offset_control = &rctx->control_word;
	u32 enc_iv_len = crypto_ablkcipher_ivsize(tfm);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	u64 *ctrl_flags = NULL;

	req_info->ctrl.s.grp = 0;
	req_info->ctrl.s.dma_mode = DMA_GATHER_SCATTER;
	req_info->ctrl.s.se_req = SE_CORE_REQ;

	req_info->req.opcode.s.major = MAJOR_OP_FC |
					DMA_MODE_FLAG(DMA_GATHER_SCATTER);
	if (enc)
		req_info->req.opcode.s.minor = 2;
	else
		req_info->req.opcode.s.minor = 3;

	req_info->req.param1 = req->nbytes; /* Encryption Data length */
	req_info->req.param2 = 0; /*Auth data length */

	fctx->enc.enc_ctrl.e.enc_cipher = cipher_type;
	fctx->enc.enc_ctrl.e.aes_key = aes_key_type;
	fctx->enc.enc_ctrl.e.iv_source = FROM_DPTR;

	memcpy(fctx->enc.encr_key, ctx->enc_key, ctx->key_len);
	ctrl_flags = (u64 *)&fctx->enc.enc_ctrl.flags;
	*ctrl_flags = cpu_to_be64(*ctrl_flags);

	*offset_control = cpu_to_be64(((u64)(enc_iv_len) << 16));
	/* Storing  Packet Data Information in offset
	 * Control Word First 8 bytes
	 */
	req_info->in[*argcnt].vptr = (u8 *)offset_control;
	req_info->in[*argcnt].size = CONTROL_WORD_LEN;
	req_info->req.dlen += CONTROL_WORD_LEN;

	++(*argcnt);

	req_info->in[*argcnt].vptr = (u8 *)fctx;
	req_info->in[*argcnt].size = sizeof(struct fc_context);
	req_info->req.dlen += sizeof(struct fc_context);

	++(*argcnt);

	return 0;
}

static inline u32 create_input_list(struct ablkcipher_request  *req, u32 enc,
				    u32 cipher_type, u32 aes_key_type,
				    u32 enc_iv_len)
{
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	u32 argcnt =  0;

	create_ctx_hdr(req, enc, cipher_type, aes_key_type, &argcnt);
	update_input_iv(req_info, req->info, enc_iv_len, &argcnt);
	update_input_data(req_info, req->src, req->nbytes, &argcnt);
	req_info->incnt = argcnt;

	return 0;
}

static inline void store_cb_info(struct ablkcipher_request *req,
				 struct cpt_request_info *req_info)
{
	req_info->callback = (void *)cvm_callback;
	req_info->callback_arg = (void *)&req->base;
}

static inline void create_output_list(struct ablkcipher_request *req,
				      u32 cipher_type,
				      u32 enc_iv_len)
{
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	u32 argcnt = 0;

	/* OUTPUT Buffer Processing
	 * AES encryption/decryption output would be
	 * received in the following format
	 *
	 * ------IV--------|------ENCRYPTED/DECRYPTED DATA-----|
	 * [ 16 Bytes/     [   Request Enc/Dec/ DATA Len AES CBC ]
	 */
	/* Reading IV information */
	update_output_iv(req_info, req->info, enc_iv_len, &argcnt);
	update_output_data(req_info, req->dst, req->nbytes, &argcnt);
	req_info->outcnt = argcnt;
}

static inline u32 cvm_enc_dec(struct ablkcipher_request *req, u32 enc,
			      u32 cipher_type)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct cvm_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	u32 key_type = AES_128_BIT;
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	u32 enc_iv_len = crypto_ablkcipher_ivsize(tfm);
	struct fc_context *fctx = &rctx->fctx;
	struct cpt_request_info *req_info = &rctx->cpt_req;
	void *cdev = NULL;
	u32 status = -1;

	switch (ctx->key_len) {
	case 16:
		key_type = AES_128_BIT;
		break;
	case 24:
		key_type = AES_192_BIT;
		break;
	case 32:
		key_type = AES_256_BIT;
		break;
	default:
		return -EINVAL;
	}

	if (cipher_type == DES3_CBC)
		key_type = 0;

	memset(req_info, 0, sizeof(struct cpt_request_info));
	memset(fctx, 0, sizeof(struct fc_context));
	create_input_list(req, enc, cipher_type, key_type, enc_iv_len);
	create_output_list(req, cipher_type, enc_iv_len);
	store_cb_info(req, req_info);
	cdev = dev_handle.cdev[smp_processor_id()];
	status = cptvf_do_request(cdev, req_info);
	/* We perform an asynchronous send and once
	 * the request is completed the driver would
	 * intimate through  registered call back functions
	 */

	if (status)
		return status;
	else
		return -EINPROGRESS;
}

s32 cvm_des3_encrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, DES3_CBC);
}

s32 cvm_des3_decrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, DES3_CBC);
}

s32 cvm_aes_encrypt_xts(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, AES_XTS);
}

s32 cvm_aes_decrypt_xts(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, AES_XTS);
}

s32 cvm_aes_encrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, AES_CBC);
}

s32 cvm_aes_decrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, AES_CBC);
}

s32 cvm_enc_dec_setkey(struct crypto_ablkcipher *cipher, const u8 *key,
		       u32 keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct cvm_enc_ctx *ctx = crypto_tfm_ctx(tfm);

	if ((keylen == 16) || (keylen == 24) || (keylen == 32)) {
		ctx->key_len = keylen;
		memcpy(ctx->enc_key, key, keylen);
		return 0;
	}
	crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);

	return -EINVAL;
}

s32 cvm_enc_dec_init(struct crypto_tfm *tfm)
{
	struct cvm_enc_ctx *ctx = crypto_tfm_ctx(tfm);

	memset(ctx, 0, sizeof(*ctx));
	tfm->crt_ablkcipher.reqsize = sizeof(struct cvm_req_ctx) +
					sizeof(struct ablkcipher_request);
	/* Additional memory for ablkcipher_request is
	 * allocated since the cryptd daemon uses
	 * this memory for request_ctx information
	 */

	return 0;
}

void cvm_enc_dec_exit(struct crypto_tfm *tfm)
{
	return;
}

struct crypto_alg algs[] = { {
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct cvm_enc_ctx),
	.cra_alignmask = 7,
	.cra_priority = 4001,
	.cra_name = "xts(aes)",
	.cra_driver_name = "cavium-xts-aes",
	.cra_type = &crypto_ablkcipher_type,
	.cra_u = {
		.ablkcipher = {
			.ivsize = AES_BLOCK_SIZE,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.setkey = cvm_enc_dec_setkey,
			.encrypt = cvm_aes_encrypt_xts,
			.decrypt = cvm_aes_decrypt_xts,
		},
	},
	.cra_init = cvm_enc_dec_init,
	.cra_exit = cvm_enc_dec_exit,
	.cra_module = THIS_MODULE,
}, {
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = AES_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct cvm_enc_ctx),
	.cra_alignmask = 7,
	.cra_priority = 4001,
	.cra_name = "cbc(aes)",
	.cra_driver_name = "cavium-cbc-aes",
	.cra_type = &crypto_ablkcipher_type,
	.cra_u = {
		.ablkcipher = {
			.ivsize = AES_BLOCK_SIZE,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.setkey = cvm_enc_dec_setkey,
			.encrypt = cvm_aes_encrypt_cbc,
			.decrypt = cvm_aes_decrypt_cbc,
		},
	},
	.cra_init = cvm_enc_dec_init,
	.cra_exit = cvm_enc_dec_exit,
	.cra_module = THIS_MODULE,
}, {
	.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize = DES3_EDE_BLOCK_SIZE,
	.cra_ctxsize = sizeof(struct cvm_des3_ctx),
	.cra_alignmask = 7,
	.cra_priority = 4001,
	.cra_name = "cbc(des3_ede)",
	.cra_driver_name = "cavium-cbc-des3_ede",
	.cra_type = &crypto_ablkcipher_type,
	.cra_u = {
		.ablkcipher = {
			.min_keysize = DES3_EDE_KEY_SIZE,
			.max_keysize = DES3_EDE_KEY_SIZE,
			.ivsize = DES_BLOCK_SIZE,
			.setkey = cvm_enc_dec_setkey,
			.encrypt = cvm_des3_encrypt_cbc,
			.decrypt = cvm_des3_decrypt_cbc,
		},
	},
	.cra_init = cvm_enc_dec_init,
	.cra_exit = cvm_enc_dec_exit,
	.cra_module = THIS_MODULE,
} };

static inline s32 cav_register_algs(void)
{
	s32 err = 0;

	err = crypto_register_algs(algs, ARRAY_SIZE(algs));
	if (err) {
		pr_err("Error in aes module init %d\n", err);
		return -1;
	}

	return 0;
}

static inline void cav_unregister_algs(void)
{
	crypto_unregister_algs(algs, ARRAY_SIZE(algs));
}

s32 cvm_crypto_init(struct cpt_vf *cptvf)
{
	u32 dev_count;

	dev_count = dev_handle.dev_count;
	dev_handle.cdev[dev_count] = cptvf;
	dev_handle.dev_count++;

	if (!dev_count) {
		if (cav_register_algs()) {
			pr_err("Error in registering crypto algorithms\n");
			return -EINVAL;
		}
	}

	return 0;
}

void cvm_crypto_exit(void)
{
	u32 dev_count;

	dev_count = --dev_handle.dev_count;
	if (!dev_count)
		cav_unregister_algs();
}
