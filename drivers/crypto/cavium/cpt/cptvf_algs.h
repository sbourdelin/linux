/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef _CAVIUM_SYM_CRYPTO_H_
#define _CAVIUM_SYM_CRYPTO_H_

#define MAX_DEVICES 16
/* AE opcodes*/
#define MAJOR_OP_MISC         0x01
#define MAJOR_OP_RANDOM       0x02
#define MAJOR_OP_MODEXP       0x03
#define MAJOR_OP_ECDSA        0x04
#define MAJOR_OP_ECC          0x05
#define MAJOR_OP_GENRSAPRIME  0x06
#define MAJOR_OP_AE_RANDOM    0x32
#define MAJOR_OP_AE_PASSTHRU  0x01
#define MINOR_OP_AE_PASSTHRU  0x07

/*SE opcodes*/
#define MAJOR_OP_SE_MISC    0x31
#define MAJOR_OP_SE_RANDOM  0x32
#define MAJOR_OP_FC         0x33
#define MAJOR_OP_HASH       0x34
#define MAJOR_OP_HMAC       0x35
#define MAJOR_OP_DSIV       0x36

#define MAJOR_OP_SSL_FULL    0x10
#define MAJOR_OP_SSL_VERIFY  0x11
#define MAJOR_OP_SSL_RESUME  0x12
#define MAJOR_OP_SSL_FINISH  0x13
#define MAJOR_OP_SSL_ENCREC  0x14
#define MAJOR_OP_SSL_DECREC  0x15

#define MAJOR_OP_WRITESA_OUTBOUND 0x20
#define MAJOR_OP_WRITESA_INBOUND  0x21
#define MAJOR_OP_OUTBOUND         0x23
#define MAJOR_OP_INBOUND          0x24

#define MAJOR_OP_SE_PASSTHRU  0x01
#define MINOR_OP_SE_PASSTHRU  0x07

#define  CAV_PRIORITY 1000
#define  MAX_ENC_KEY_SIZE 32
#define  MAX_HASH_KEY_SIZE 64
#define  MAX_KEY_SIZE (MAX_ENC_KEY_SIZE + MAX_HASH_KEY_SIZE)
#define  CONTROL_WORD_LEN 8

#define IV_OFFSET 8   /* Include SPI | SNO 8 Bytes */
#define AES_CBC_ALG_NAME "cbc(aes)"
#define AES_XTS_ALG_NAME "xts(aes)"
#define DES3_ALG_NAME "cbc(des3_ede)"

#define  BYTE_16 16
#define  BYTE_24 24
#define  BYTE_32 32

#define DMA_MODE_FLAG(dma_mode) \
	((dma_mode == DMA_GATHER_SCATTER) ? (1 << 7) : 0)

enum req_type {
	AE_CORE_REQ,
	SE_CORE_REQ,
};

enum cipher_type {
	DES3_CBC = 0x1,
	DES3_ECB = 0x2,
	AES_CBC = 0x3,
	AES_ECB = 0x4,
	AES_CFB = 0x5,
	AES_CTR = 0x6,
	AES_GCM = 0x7,
	AES_XTS = 0x8
};

enum aes_type {
	AES_128_BIT = 0x1,
	AES_192_BIT = 0x2,
	AES_256_BIT = 0x3
};

/*Context length in words*/
#define  FC_CTX_LENGTH       23
#define  ENC_CTX_LENGTH       7
#define  HASH_CTX_LENGTH     34
#define  HMAC_CTX_LENGTH     34

union encr_ctrl {
	uint64_t flags;
	struct {
#if defined(__BIG_ENDIAN_BITFIELD)
		uint64_t enc_cipher:4;
		uint64_t reserved1:1;
		uint64_t aes_key:2;
		uint64_t iv_source:1;
		uint64_t hash_type:4;
		uint64_t reserved2:3;
		uint64_t auth_input_type:1;
		uint64_t mac_len:8;
		uint64_t reserved3:8;
		uint64_t encr_offset:16;
		uint64_t iv_offset:8;
		uint64_t auth_offset:8;
#else
		uint64_t auth_offset:8;
		uint64_t iv_offset:8;
		uint64_t encr_offset:16;
		uint64_t reserved3:8;
		uint64_t mac_len:8;
		uint64_t auth_input_type:1;
		uint64_t reserved2:3;
		uint64_t hash_type:4;
		uint64_t iv_source:1;
		uint64_t aes_key:2;
		uint64_t reserved1:1;
		uint64_t enc_cipher:4;
#endif
	} e;
};

struct enc_context {
	union encr_ctrl enc_ctrl;
	uint8_t  encr_key[32];
	uint8_t  encr_iv[16];
};

struct fchmac_context {
	uint8_t  ipad[64];
	uint8_t  opad[64]; /* or OPAD */
};

struct fc_context {
	struct enc_context enc;
	struct fchmac_context hmac;
};

struct cvm_enc_ctx {
	uint32_t key_len;
	uint8_t enc_key[MAX_KEY_SIZE];
};

struct cvm_des3_ctx {
	uint32_t key_len;
	uint8_t des3_key[MAX_KEY_SIZE];
};

struct cvm_req_ctx {
	struct cpt_request_info cpt_req;
	uint64_t control_word;
	struct fc_context fctx;
};

uint32_t cptvf_do_request(void *cptvf, struct cpt_request_info *);
#endif /*_CAVIUM_SYM_CRYPTO_H_*/
