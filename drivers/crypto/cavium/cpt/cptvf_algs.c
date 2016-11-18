
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
#include <crypto/internal/aead.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <crypto/aes.h>
#include <crypto/des.h>
#include "request_manager.h"
#include "cptvf.h"
#include "cptvf_algs.h"

struct cpt_device_handle {
	void *cdev[MAX_DEVICES];
	uint32_t dev_count;
};

static struct cpt_device_handle dev_handle;

static void cvm_callback(uint32_t status, void *arg)
{
	struct crypto_async_request *req = (struct crypto_async_request *)arg;

	req->complete(req, !status);
}

static inline void update_input_iv(struct cpt_request_info *req_info,
				   uint8_t *iv, uint32_t enc_iv_len,
				   uint32_t *argcnt)
{
	/* Setting the iv information */
	req_info->in[*argcnt].ptr.addr = (void *)iv;
	req_info->in[*argcnt].size = enc_iv_len;
	req_info->in[*argcnt].offset = enc_iv_len;
	req_info->in[*argcnt].type = UNIT_8_BIT;
	req_info->req.dlen += enc_iv_len;

	++(*argcnt);
}

static inline void update_output_iv(struct cpt_request_info *req_info,
				    uint8_t *iv, uint32_t enc_iv_len,
				    uint32_t *argcnt)
{
	/* Setting the iv information */
	req_info->out[*argcnt].ptr.addr = (void *)iv;
	req_info->out[*argcnt].size = enc_iv_len;
	req_info->out[*argcnt].offset = enc_iv_len;
	req_info->out[*argcnt].type = UNIT_8_BIT;

	req_info->rlen += enc_iv_len;

	++(*argcnt);
}

static inline void update_input_data(struct cpt_request_info *req_info,
				     struct scatterlist *inp_sg,
				     uint32_t nbytes, uint32_t *argcnt)
{
	req_info->req.dlen += nbytes;

	while (nbytes) {
		uint32_t len = min(nbytes, inp_sg->length);
		uint8_t *ptr = page_address(sg_page(inp_sg)) + inp_sg->offset;

		req_info->in[*argcnt].ptr.addr = (void *)ptr;
		req_info->in[*argcnt].size = len;
		req_info->in[*argcnt].offset = len;
		req_info->in[*argcnt].type = UNIT_8_BIT;
		nbytes -= len;

		++(*argcnt);
		++inp_sg;
	}
}

static inline void update_output_data(struct cpt_request_info *req_info,
				      struct scatterlist *outp_sg,
				      uint32_t nbytes, uint32_t *argcnt)
{
	req_info->rlen += nbytes;

	while (nbytes) {
		uint32_t len = min(nbytes, outp_sg->length);
		uint8_t *ptr = page_address(sg_page(outp_sg)) +
					    outp_sg->offset;

		req_info->out[*argcnt].ptr.addr = (void *)ptr;
		req_info->out[*argcnt].size = len;
		req_info->out[*argcnt].offset = len;
		req_info->out[*argcnt].type = UNIT_8_BIT;
		nbytes -= len;
		++(*argcnt);
		++outp_sg;
	}
}

static inline uint32_t create_ctx_hdr(struct ablkcipher_request *req,
				      uint32_t enc, uint32_t cipher_type,
				      uint32_t aes_key_type, uint32_t *argcnt)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct cvm_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct fc_context *fctx = &rctx->fctx;
	uint64_t *offset_control = &rctx->control_word;
	uint32_t enc_iv_len = crypto_ablkcipher_ivsize(tfm);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	uint64_t *ctrl_flags = NULL;
	uint8_t iv_inp = FROM_DPTR;
	uint8_t dma_mode = DMA_GATHER_SCATTER;

	req_info->ctrl.s.grp = 0;
	req_info->ctrl.s.dma_mode = dma_mode;
	req_info->ctrl.s.req_mode = NON_BLOCKING;
	req_info->ctrl.s.se_req = SE_CORE_REQ;

	req_info->ctxl = sizeof(struct fc_context);
	req_info->handle = 0;

	req_info->req.opcode.s.major = MAJOR_OP_FC | DMA_MODE_FLAG(dma_mode);
	if (enc)
		req_info->req.opcode.s.minor = 2;
	else
		req_info->req.opcode.s.minor = 3;

	req_info->req.param1 = req->nbytes; /* Encryption Data length */
	req_info->req.param2 = 0; /*Auth data length */

	fctx->enc.enc_ctrl.e.enc_cipher = cipher_type;
	fctx->enc.enc_ctrl.e.aes_key = aes_key_type;
	fctx->enc.enc_ctrl.e.iv_source = iv_inp;

	memcpy(fctx->enc.encr_key, ctx->enc_key, ctx->key_len);
	ctrl_flags = (uint64_t *)&fctx->enc.enc_ctrl.flags;
	*ctrl_flags = cpu_to_be64(*ctrl_flags);

	*offset_control = cpu_to_be64(((uint64_t)(enc_iv_len) << 16));
	/* Storing  Packet Data Information in offset
	 * Control Word First 8 bytes
	 */
	req_info->in[*argcnt].ptr.addr = (uint8_t *)offset_control;
	req_info->in[*argcnt].size = CONTROL_WORD_LEN;
	req_info->in[*argcnt].offset = CONTROL_WORD_LEN;
	req_info->in[*argcnt].type = UNIT_8_BIT;
	req_info->req.dlen += CONTROL_WORD_LEN;

	++(*argcnt);

	req_info->in[*argcnt].ptr.addr = (uint8_t *)fctx;
	req_info->in[*argcnt].size = sizeof(struct fc_context);
	req_info->in[*argcnt].offset = sizeof(struct fc_context);
	req_info->in[*argcnt].type = UNIT_8_BIT;
	req_info->req.dlen += sizeof(struct fc_context);

	++(*argcnt);

	return 0;
}

static inline uint32_t create_input_list(struct ablkcipher_request  *req,
					 uint32_t enc, uint32_t cipher_type,
					 uint32_t aes_key_type,
					 uint32_t enc_iv_len)
{
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	uint32_t argcnt =  0;

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
				      uint32_t cipher_type,
				      uint32_t enc_iv_len)
{
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	struct cpt_request_info *req_info = &rctx->cpt_req;
	uint32_t argcnt = 0;

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

static inline uint32_t cvm_enc_dec(struct ablkcipher_request *req,
				   uint32_t enc, uint32_t cipher_type)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct cvm_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	uint32_t key_type = AES_128_BIT;
	struct cvm_req_ctx *rctx = ablkcipher_request_ctx(req);
	uint32_t enc_iv_len = crypto_ablkcipher_ivsize(tfm);
	struct fc_context *fctx = &rctx->fctx;
	struct cpt_request_info *req_info = &rctx->cpt_req;
	void *cdev = NULL;
	uint32_t status = -1;

	switch (ctx->key_len) {
	case BYTE_16:
		key_type = AES_128_BIT;
		break;
	case BYTE_24:
		key_type = AES_192_BIT;
		break;
	case BYTE_32:
		key_type = AES_256_BIT;
		break;
	default:
		return ERR_GC_CIPHER_UNSUPPORTED;
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

int cvm_des3_encrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, DES3_CBC);
}

int cvm_des3_decrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, DES3_CBC);
}

int cvm_aes_encrypt_xts(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, AES_XTS);
}

int cvm_aes_decrypt_xts(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, AES_XTS);
}

int cvm_aes_encrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, true, AES_CBC);
}

int cvm_aes_decrypt_cbc(struct ablkcipher_request *req)
{
	return cvm_enc_dec(req, false, AES_CBC);
}

int cvm_enc_dec_setkey(struct crypto_ablkcipher *cipher, const uint8_t *key,
		       uint32_t keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct cvm_enc_ctx *ctx = crypto_tfm_ctx(tfm);

	if ((keylen == BYTE_16) || (keylen == BYTE_24) ||
	    (keylen == BYTE_32)) {
		ctx->key_len = keylen;
		memcpy(ctx->enc_key, key, keylen);
		return 0;
	}
	crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);

	return -EINVAL;
}

int cvm_enc_dec_init(struct crypto_tfm *tfm)
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
	.cra_priority = CAV_PRIORITY,
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
	.cra_priority = CAV_PRIORITY,
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
	.cra_priority = CAV_PRIORITY,
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

static inline int cav_register_algs(void)
{
	int err = 0;

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

int cvm_crypto_init(struct cpt_vf *cptvf)
{
	uint32_t dev_count;

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
	uint32_t dev_count;

	dev_count = --dev_handle.dev_count;
	if (!dev_count)
		cav_unregister_algs();
}
