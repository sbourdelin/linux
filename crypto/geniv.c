// SPDX-License-Identifier: GPL-2.0
/*
 * geniv.c - crypto template for generating IV
 *
 * Copyright (C) 2018, Linaro
 *
 * This file adds a crypto template to generate IV, so the dm-crypt can rely
 * on it and remove the existing generating IV code.
 */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/ctype.h>
#include <asm/page.h>
#include <asm/unaligned.h>
#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/algapi.h>
#include <crypto/skcipher.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <crypto/geniv.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <linux/rtnetlink.h> /* for struct rtattr and RTA macros only */
#include <keys/user-type.h>
#include <linux/backing-dev.h>
#include <linux/device-mapper.h>
#include <linux/log2.h>

#define DM_MSG_PREFIX		"crypt"
#define MIN_IOS		64
#define IV_TYPE_NUM 8
#define SECTOR_MASK ((1 << SECTOR_SHIFT) - 1)

struct geniv_ctx;
struct geniv_req_ctx;

/* Sub request for each of the skcipher_request's for a segment */
struct geniv_subreq {
	struct scatterlist sg_in[4];
	struct scatterlist sg_out[4];
	sector_t iv_sector;
	struct geniv_req_ctx *rctx;
	union {
		struct skcipher_request req;
		struct aead_request req_aead;
	} r CRYPTO_MINALIGN_ATTR;
};

/* used to iter the src scatterlist of the input parent request */
struct scatterlist_iter {
	/* current segment to be processed */
	unsigned int seg_no;
	/* bytes had been processed in current segment */
	unsigned int done;
	/* bytes to be processed in the next request */
	unsigned int len;
};

/* contex of the input parent request */
struct geniv_req_ctx {
	struct geniv_subreq *subreq;
	bool is_write;
	bool is_aead_request;
	sector_t cc_sector;
	/* array size of src scatterlist of parent request */
	unsigned int nents;
	struct scatterlist_iter iter;
	struct completion restart;
	atomic_t req_pending;
	u8 *integrity_metadata;
	/* point to the input parent request */
	union {
		struct skcipher_request *req;
		struct aead_request *req_aead;
	} r;
};

struct crypt_iv_operations {
	int (*ctr)(struct geniv_ctx *ctx);
	void (*dtr)(struct geniv_ctx *ctx);
	int (*init)(struct geniv_ctx *ctx);
	int (*wipe)(struct geniv_ctx *ctx);
	int (*generator)(struct geniv_ctx *ctx,
			struct geniv_req_ctx *rctx,
			struct geniv_subreq *subreq, u8 *iv);
	int (*post)(struct geniv_ctx *ctx,
			struct geniv_req_ctx *rctx,
			struct geniv_subreq *subreq, u8 *iv);
};

struct geniv_essiv_private {
	struct crypto_ahash *hash_tfm;
	u8 *salt;
};

struct geniv_benbi_private {
	int shift;
};

#define LMK_SEED_SIZE 64 /* hash + 0 */
struct geniv_lmk_private {
	struct crypto_shash *hash_tfm;
	u8 *seed;
};

#define TCW_WHITENING_SIZE 16
struct geniv_tcw_private {
	struct crypto_shash *crc32_tfm;
	u8 *iv_seed;
	u8 *whitening;
};

/* context of geniv tfm */
struct geniv_ctx {
	unsigned int tfms_count;
	union {
		struct crypto_skcipher *tfm;
		struct crypto_aead *tfm_aead;
	} tfm_child;
	union {
		struct crypto_skcipher **tfms;
		struct crypto_aead **tfms_aead;
	} tfms;

	char *ivmode;
	unsigned int iv_size;
	unsigned int iv_start;
	unsigned int rctx_start;
	sector_t iv_offset;
	unsigned short int sector_size;
	unsigned char sector_shift;
	char *algname;
	char *ivopts;
	char *cipher;
	char *ciphermode;
	unsigned long cipher_flags;

	const struct crypt_iv_operations *iv_gen_ops;
	union {
		struct geniv_essiv_private essiv;
		struct geniv_benbi_private benbi;
		struct geniv_lmk_private lmk;
		struct geniv_tcw_private tcw;
	} iv_gen_private;
	void *iv_private;

	mempool_t *subreq_pool;
	unsigned int key_size;
	unsigned int key_parts;      /* independent parts in key buffer */
	unsigned int key_extra_size; /* additional keys length */
	unsigned int key_mac_size;

	unsigned int integrity_tag_size;
	unsigned int integrity_iv_size;
	unsigned int on_disk_tag_size;

	char *msg;
	u8 *authenc_key; /* space for keys in authenc() format (if used) */
	u8 *key;
};

static struct scatterlist *crypt_get_sg_data(struct geniv_ctx *ctx,
					     struct scatterlist *sg);

static bool geniv_integrity_aead(struct geniv_ctx *ctx)
{
	return test_bit(CRYPT_MODE_INTEGRITY_AEAD, &ctx->cipher_flags);
}

static bool geniv_integrity_hmac(struct geniv_ctx *ctx)
{
	return geniv_integrity_aead(ctx) && ctx->key_mac_size;
}

static struct geniv_req_ctx *geniv_skcipher_req_ctx(struct skcipher_request *req)
{
	return (void *)PTR_ALIGN((u8 *)skcipher_request_ctx(req),  __alignof__(struct geniv_req_ctx));
}

static struct geniv_req_ctx *geniv_aead_req_ctx(struct aead_request *req)
{
	return (void *)PTR_ALIGN((u8 *)aead_request_ctx(req), __alignof__(struct geniv_req_ctx));
}

static u8 *iv_of_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	if (geniv_integrity_aead(ctx))
		return (u8 *)ALIGN((unsigned long)((char *)subreq + ctx->iv_start),
			crypto_aead_alignmask(crypto_aead_reqtfm(subreq->rctx->r.req_aead)) + 1);
	else
		return (u8 *)ALIGN((unsigned long)((char *)subreq + ctx->iv_start),
			crypto_skcipher_alignmask(crypto_skcipher_reqtfm(subreq->rctx->r.req)) + 1);
}

/* Get sg containing data */
static struct scatterlist *crypt_get_sg_data(struct geniv_ctx *ctx,
					     struct scatterlist *sg)
{
	if (unlikely(geniv_integrity_aead(ctx)))
		return &sg[2];

	return sg;
}

/*
 * Different IV generation algorithms:
 *
 * plain: the initial vector is the 32-bit little-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * plain64: the initial vector is the 64-bit little-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * plain64be: the initial vector is the 64-bit big-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * essiv: "encrypted sector|salt initial vector", the sector number is
 *        encrypted with the bulk cipher using a salt as key. The salt
 *        should be derived from the bulk cipher's key via hashing.
 *
 * benbi: the 64-bit "big-endian 'narrow block'-count", starting at 1
 *        (needed for LRW-32-AES and possible other narrow block modes)
 *
 * null: the initial vector is always zero.  Provides compatibility with
 *       obsolete loop_fish2 devices.  Do not use for new devices.
 *
 * lmk:  Compatible implementation of the block chaining mode used
 *       by the Loop-AES block device encryption system
 *       designed by Jari Ruusu. See http://loop-aes.sourceforge.net/
 *       It operates on full 512 byte sectors and uses CBC
 *       with an IV derived from the sector number, the data and
 *       optionally extra IV seed.
 *       This means that after decryption the first block
 *       of sector must be tweaked according to decrypted data.
 *       Loop-AES can use three encryption schemes:
 *         version 1: is plain aes-cbc mode
 *         version 2: uses 64 multikey scheme with lmk IV generator
 *         version 3: the same as version 2 with additional IV seed
 *                   (it uses 65 keys, last key is used as IV seed)
 *
 * tcw:  Compatible implementation of the block chaining mode used
 *       by the TrueCrypt device encryption system (prior to version 4.1).
 *       For more info see: https://gitlab.com/cryptsetup/cryptsetup/wikis/TrueCryptOnDiskFormat
 *       It operates on full 512 byte sectors and uses CBC
 *       with an IV derived from initial key and the sector number.
 *       In addition, whitening value is applied on every sector, whitening
 *       is calculated from initial key, sector number and mixed using CRC32.
 *       Note that this encryption scheme is vulnerable to watermarking attacks
 *       and should be used for old compatible containers access only.
 *
 * plumb: unimplemented, see:
 * http://article.gmane.org/gmane.linux.kernel.device-mapper.dm-crypt/454
 */

static int crypt_iv_plain_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	memset(iv, 0, ctx->iv_size);
	*(__le32 *)iv = cpu_to_le32(subreq->iv_sector & 0xffffffff);

	return 0;
}

static int crypt_iv_plain64_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	memset(iv, 0, ctx->iv_size);
	*(__le64 *)iv = cpu_to_le64(subreq->iv_sector);

	return 0;
}

static int crypt_iv_plain64be_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	memset(iv, 0, ctx->iv_size);
	/* iv_size is at least of size u64; usually it is 16 bytes */
	*(__be64 *)&iv[ctx->iv_size - sizeof(u64)] = cpu_to_be64(subreq->iv_sector);

	return 0;
}

/* Initialise ESSIV - compute salt but no local memory allocations */
static int crypt_iv_essiv_init(struct geniv_ctx *ctx)
{
	struct geniv_essiv_private *essiv = &ctx->iv_gen_private.essiv;
	AHASH_REQUEST_ON_STACK(req, essiv->hash_tfm);
	struct scatterlist sg;
	struct crypto_cipher *essiv_tfm;
	int err;

	sg_init_one(&sg, ctx->key, ctx->key_size);
	ahash_request_set_tfm(req, essiv->hash_tfm);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP, NULL, NULL);
	ahash_request_set_crypt(req, &sg, essiv->salt, ctx->key_size);

	err = crypto_ahash_digest(req);
	ahash_request_zero(req);
	if (err)
		return err;

	essiv_tfm = ctx->iv_private;

	return crypto_cipher_setkey(essiv_tfm, essiv->salt,
			    crypto_ahash_digestsize(essiv->hash_tfm));
}

/* Wipe salt and reset key derived from volume key */
static int crypt_iv_essiv_wipe(struct geniv_ctx *ctx)
{
	struct geniv_essiv_private *essiv = &ctx->iv_gen_private.essiv;
	unsigned int salt_size = crypto_ahash_digestsize(essiv->hash_tfm);
	struct crypto_cipher *essiv_tfm;

	memset(essiv->salt, 0, salt_size);

	essiv_tfm = ctx->iv_private;
	return crypto_cipher_setkey(essiv_tfm, essiv->salt, salt_size);
}

/* Allocate the cipher for ESSIV */
static struct crypto_cipher *alloc_essiv_cipher(struct geniv_ctx *ctx,
					u8 *salt, unsigned int saltsize)
{
	struct crypto_cipher *essiv_tfm;
	int err;

	/* Setup the essiv_tfm with the given salt */
	essiv_tfm = crypto_alloc_cipher(ctx->cipher, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(essiv_tfm)) {
		DMERR("Error allocating crypto tfm for ESSIV\n");
		return essiv_tfm;
	}

	if (crypto_cipher_blocksize(essiv_tfm) != ctx->iv_size) {
		DMERR("Block size of ESSIV cipher does "
			    "not match IV size of block cipher\n");
		crypto_free_cipher(essiv_tfm);
		return ERR_PTR(-EINVAL);
	}

	err = crypto_cipher_setkey(essiv_tfm, salt, saltsize);
	if (err) {
		DMERR("Failed to set key for ESSIV cipher\n");
		crypto_free_cipher(essiv_tfm);
		return ERR_PTR(err);
	}

	return essiv_tfm;
}

static void crypt_iv_essiv_dtr(struct geniv_ctx *ctx)
{
	struct crypto_cipher *essiv_tfm;
	struct geniv_essiv_private *essiv = &ctx->iv_gen_private.essiv;

	crypto_free_ahash(essiv->hash_tfm);
	essiv->hash_tfm = NULL;

	kzfree(essiv->salt);
	essiv->salt = NULL;

	essiv_tfm = ctx->iv_private;

	if (essiv_tfm)
		crypto_free_cipher(essiv_tfm);

	ctx->iv_private = NULL;
}

static int crypt_iv_essiv_ctr(struct geniv_ctx *ctx)
{
	struct crypto_cipher *essiv_tfm = NULL;
	struct crypto_ahash *hash_tfm = NULL;
	u8 *salt = NULL;
	int err;

	if (!ctx->ivopts) {
		DMERR("Digest algorithm missing for ESSIV mode\n");
		return -EINVAL;
	}

	/* Allocate hash algorithm */
	hash_tfm = crypto_alloc_ahash(ctx->ivopts, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(hash_tfm)) {
		DMERR("Error initializing ESSIV hash\n");
		err = PTR_ERR(hash_tfm);
		goto bad;
	}

	salt = kzalloc(crypto_ahash_digestsize(hash_tfm), GFP_KERNEL);
	if (!salt) {
		DMERR("Error kmallocing salt storage in ESSIV\n");
		err = -ENOMEM;
		goto bad;
	}

	ctx->iv_gen_private.essiv.salt = salt;
	ctx->iv_gen_private.essiv.hash_tfm = hash_tfm;

	essiv_tfm = alloc_essiv_cipher(ctx, salt,
				       crypto_ahash_digestsize(hash_tfm));
	if (IS_ERR(essiv_tfm)) {
		crypt_iv_essiv_dtr(ctx);
		return PTR_ERR(essiv_tfm);
	}
	ctx->iv_private = essiv_tfm;

	return 0;

bad:
	if (hash_tfm && !IS_ERR(hash_tfm))
		crypto_free_ahash(hash_tfm);
	kfree(salt);
	return err;
}

static int crypt_iv_essiv_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	struct crypto_cipher *essiv_tfm = ctx->iv_private;

	memset(iv, 0, ctx->iv_size);
	*(__le64 *)iv = cpu_to_le64(subreq->iv_sector);
	crypto_cipher_encrypt_one(essiv_tfm, iv, iv);

	return 0;
}

static int crypt_iv_benbi_ctr(struct geniv_ctx *ctx)
{
	unsigned int bs = crypto_skcipher_blocksize(ctx->tfms.tfms[0]);
	int log = ilog2(bs);

	/* we need to calculate how far we must shift the sector count
	 * to get the cipher block count, we use this shift in _gen */

	if (1 << log != bs) {
		DMERR("cypher blocksize is not a power of 2\n");
		return -EINVAL;
	}

	if (log > 9) {
		DMERR("cypher blocksize is > 512\n");
		return -EINVAL;
	}

	ctx->iv_gen_private.benbi.shift = 9 - log;

	return 0;
}

static void crypt_iv_benbi_dtr(struct geniv_ctx *ctx)
{
}

static int crypt_iv_benbi_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	__be64 val;

	memset(iv, 0, ctx->iv_size - sizeof(u64)); /* rest is cleared below */

	val = cpu_to_be64(((u64)subreq->iv_sector << ctx->iv_gen_private.benbi.shift) + 1);
	put_unaligned(val, (__be64 *)(iv + ctx->iv_size - sizeof(u64)));

	return 0;
}

static int crypt_iv_null_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	memset(iv, 0, ctx->iv_size);

	return 0;
}

static void crypt_iv_lmk_dtr(struct geniv_ctx *ctx)
{
	struct geniv_lmk_private *lmk = &ctx->iv_gen_private.lmk;

	if (lmk->hash_tfm && !IS_ERR(lmk->hash_tfm))
		crypto_free_shash(lmk->hash_tfm);
	lmk->hash_tfm = NULL;

	kzfree(lmk->seed);
	lmk->seed = NULL;
}

static int crypt_iv_lmk_ctr(struct geniv_ctx *ctx)
{
	struct geniv_lmk_private *lmk = &ctx->iv_gen_private.lmk;

	if (ctx->sector_size != (1 << SECTOR_SHIFT)) {
		DMERR("Unsupported sector size for LMK\n");
		return -EINVAL;
	}

	lmk->hash_tfm = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(lmk->hash_tfm)) {
		DMERR("Error initializing LMK hash, err=%ld\n",
			PTR_ERR(lmk->hash_tfm));
		return PTR_ERR(lmk->hash_tfm);
	}

	/* No seed in LMK version 2 */
	if (ctx->key_parts == ctx->tfms_count) {
		lmk->seed = NULL;
		return 0;
	}

	lmk->seed = kzalloc(LMK_SEED_SIZE, GFP_KERNEL);
	if (!lmk->seed) {
		crypt_iv_lmk_dtr(ctx);
		DMERR("Error kmallocing seed storage in LMK\n");
		return -ENOMEM;
	}

	return 0;
}

static int crypt_iv_lmk_init(struct geniv_ctx *ctx)
{
	struct geniv_lmk_private *lmk = &ctx->iv_gen_private.lmk;
	int subkey_size = ctx->key_size / ctx->key_parts;

	/* LMK seed is on the position of LMK_KEYS + 1 key */
	if (lmk->seed)
		memcpy(lmk->seed, ctx->key + (ctx->tfms_count * subkey_size),
		       crypto_shash_digestsize(lmk->hash_tfm));

	return 0;
}

static int crypt_iv_lmk_wipe(struct geniv_ctx *ctx)
{
	struct geniv_lmk_private *lmk = &ctx->iv_gen_private.lmk;

	if (lmk->seed)
		memset(lmk->seed, 0, LMK_SEED_SIZE);

	return 0;
}

static int crypt_iv_lmk_one(struct geniv_ctx *ctx, u8 *iv,
				struct geniv_subreq *subreq, u8 *data)
{
	struct geniv_lmk_private *lmk = &ctx->iv_gen_private.lmk;
	SHASH_DESC_ON_STACK(desc, lmk->hash_tfm);
	struct md5_state md5state;
	__le32 buf[4];
	int i, r;

	desc->tfm = lmk->hash_tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;

	r = crypto_shash_init(desc);
	if (r)
		return r;

	if (lmk->seed) {
		r = crypto_shash_update(desc, lmk->seed, LMK_SEED_SIZE);
		if (r)
			return r;
	}

	/* Sector is always 512B, block size 16, add data of blocks 1-31 */
	r = crypto_shash_update(desc, data + 16, 16 * 31);
	if (r)
		return r;

	/* Sector is cropped to 56 bits here */
	buf[0] = cpu_to_le32(subreq->iv_sector & 0xFFFFFFFF);
	buf[1] = cpu_to_le32((((u64)subreq->iv_sector >> 32) & 0x00FFFFFF) | 0x80000000);
	buf[2] = cpu_to_le32(4024);
	buf[3] = 0;
	r = crypto_shash_update(desc, (u8 *)buf, sizeof(buf));
	if (r)
		return r;

	/* No MD5 padding here */
	r = crypto_shash_export(desc, &md5state);
	if (r)
		return r;

	for (i = 0; i < MD5_HASH_WORDS; i++)
		__cpu_to_le32s(&md5state.hash[i]);
	memcpy(iv, &md5state.hash, ctx->iv_size);

	return 0;
}

static int crypt_iv_lmk_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	struct scatterlist *sg;
	u8 *src;
	int r = 0;

	if (rctx->is_write) {
		sg = crypt_get_sg_data(ctx, subreq->sg_in);
		src = kmap_atomic(sg_page(sg));
		r = crypt_iv_lmk_one(ctx, iv, subreq, src + sg->offset);
		kunmap_atomic(src);
	} else
		memset(iv, 0, ctx->iv_size);

	return r;
}

static int crypt_iv_lmk_post(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	struct scatterlist *sg;
	u8 *dst;
	int r;

	if (rctx->is_write)
		return 0;

	sg = crypt_get_sg_data(ctx, subreq->sg_out);
	dst = kmap_atomic(sg_page(sg));
	r = crypt_iv_lmk_one(ctx, iv, subreq, dst + sg->offset);

	/* Tweak the first block of plaintext sector */
	if (!r)
		crypto_xor(dst + sg->offset, iv, ctx->iv_size);

	kunmap_atomic(dst);
	return r;
}

static void crypt_iv_tcw_dtr(struct geniv_ctx *ctx)
{
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;

	kzfree(tcw->iv_seed);
	tcw->iv_seed = NULL;
	kzfree(tcw->whitening);
	tcw->whitening = NULL;

	if (tcw->crc32_tfm && !IS_ERR(tcw->crc32_tfm))
		crypto_free_shash(tcw->crc32_tfm);
	tcw->crc32_tfm = NULL;
}

static int crypt_iv_tcw_ctr(struct geniv_ctx *ctx)
{
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;

	if (ctx->sector_size != (1 << SECTOR_SHIFT)) {
		DMERR("Unsupported sector size for TCW\n");
		return -EINVAL;
	}

	if (ctx->key_size <= (ctx->iv_size + TCW_WHITENING_SIZE)) {
		DMERR("Wrong key size (%d) for TCW. Choose a value > %d bytes\n",
			ctx->key_size, ctx->iv_size + TCW_WHITENING_SIZE);
		return -EINVAL;
	}

	tcw->crc32_tfm = crypto_alloc_shash("crc32", 0, 0);
	if (IS_ERR(tcw->crc32_tfm)) {
		DMERR("Error initializing CRC32 in TCW; err=%ld\n",
			PTR_ERR(tcw->crc32_tfm));
		return PTR_ERR(tcw->crc32_tfm);
	}

	tcw->iv_seed = kzalloc(ctx->iv_size, GFP_KERNEL);
	tcw->whitening = kzalloc(TCW_WHITENING_SIZE, GFP_KERNEL);
	if (!tcw->iv_seed || !tcw->whitening) {
		crypt_iv_tcw_dtr(ctx);
		DMERR("Error allocating seed storage in TCW\n");
		return -ENOMEM;
	}

	return 0;
}

static int crypt_iv_tcw_init(struct geniv_ctx *ctx)
{
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;
	int key_offset = ctx->key_size - ctx->iv_size - TCW_WHITENING_SIZE;

	memcpy(tcw->iv_seed, &ctx->key[key_offset], ctx->iv_size);
	memcpy(tcw->whitening, &ctx->key[key_offset + ctx->iv_size],
	       TCW_WHITENING_SIZE);

	return 0;
}

static int crypt_iv_tcw_wipe(struct geniv_ctx *ctx)
{
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;

	memset(tcw->iv_seed, 0, ctx->iv_size);
	memset(tcw->whitening, 0, TCW_WHITENING_SIZE);

	return 0;
}

static int crypt_iv_tcw_whitening(struct geniv_ctx *ctx,
				struct geniv_subreq *subreq, u8 *data)
{
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;
	__le64 sector = cpu_to_le64(subreq->iv_sector);
	u8 buf[TCW_WHITENING_SIZE];
	SHASH_DESC_ON_STACK(desc, tcw->crc32_tfm);
	int i, r;

	/* xor whitening with sector number */
	crypto_xor_cpy(buf, tcw->whitening, (u8 *)&sector, 8);
	crypto_xor_cpy(&buf[8], tcw->whitening + 8, (u8 *)&sector, 8);

	/* calculate crc32 for every 32bit part and xor it */
	desc->tfm = tcw->crc32_tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	for (i = 0; i < 4; i++) {
		r = crypto_shash_init(desc);
		if (r)
			goto out;
		r = crypto_shash_update(desc, &buf[i * 4], 4);
		if (r)
			goto out;
		r = crypto_shash_final(desc, &buf[i * 4]);
		if (r)
			goto out;
	}
	crypto_xor(&buf[0], &buf[12], 4);
	crypto_xor(&buf[4], &buf[8], 4);

	/* apply whitening (8 bytes) to whole sector */
	for (i = 0; i < ((1 << SECTOR_SHIFT) / 8); i++)
		crypto_xor(data + i * 8, buf, 8);
out:
	memzero_explicit(buf, sizeof(buf));
	return r;
}

static int crypt_iv_tcw_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	struct scatterlist *sg;
	struct geniv_tcw_private *tcw = &ctx->iv_gen_private.tcw;
	__le64 sector = cpu_to_le64(subreq->iv_sector);
	u8 *src;
	int r = 0;

	/* Remove whitening from ciphertext */
	if (!rctx->is_write) {
		sg = crypt_get_sg_data(ctx, subreq->sg_in);
		src = kmap_atomic(sg_page(sg));
		r = crypt_iv_tcw_whitening(ctx, subreq, src + sg->offset);
		kunmap_atomic(src);
	}

	/* Calculate IV */
	crypto_xor_cpy(iv, tcw->iv_seed, (u8 *)&sector, 8);
	if (ctx->iv_size > 8)
		crypto_xor_cpy(&iv[8], tcw->iv_seed + 8, (u8 *)&sector,
			       ctx->iv_size - 8);

	return r;
}

static int crypt_iv_tcw_post(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	struct scatterlist *sg;
	u8 *dst;
	int r;

	if (!rctx->is_write)
		return 0;

	/* Apply whitening on ciphertext */
	sg = crypt_get_sg_data(ctx, subreq->sg_out);
	dst = kmap_atomic(sg_page(sg));
	r = crypt_iv_tcw_whitening(ctx, subreq, dst + sg->offset);
	kunmap_atomic(dst);

	return r;
}

static int crypt_iv_random_gen(struct geniv_ctx *ctx,
				struct geniv_req_ctx *rctx,
				struct geniv_subreq *subreq, u8 *iv)
{
	/* Used only for writes, there must be an additional space to store IV */
	get_random_bytes(iv, ctx->iv_size);
	return 0;
}

static const struct crypt_iv_operations crypt_iv_plain_ops = {
	.generator = crypt_iv_plain_gen
};

static const struct crypt_iv_operations crypt_iv_plain64_ops = {
	.generator = crypt_iv_plain64_gen
};

static const struct crypt_iv_operations crypt_iv_plain64be_ops = {
	.generator = crypt_iv_plain64be_gen
};

static const struct crypt_iv_operations crypt_iv_essiv_ops = {
	.ctr       = crypt_iv_essiv_ctr,
	.dtr       = crypt_iv_essiv_dtr,
	.init      = crypt_iv_essiv_init,
	.wipe      = crypt_iv_essiv_wipe,
	.generator = crypt_iv_essiv_gen
};

static const struct crypt_iv_operations crypt_iv_benbi_ops = {
	.ctr	   = crypt_iv_benbi_ctr,
	.dtr	   = crypt_iv_benbi_dtr,
	.generator = crypt_iv_benbi_gen
};

static const struct crypt_iv_operations crypt_iv_null_ops = {
	.generator = crypt_iv_null_gen
};

static const struct crypt_iv_operations crypt_iv_lmk_ops = {
	.ctr	   = crypt_iv_lmk_ctr,
	.dtr	   = crypt_iv_lmk_dtr,
	.init	   = crypt_iv_lmk_init,
	.wipe	   = crypt_iv_lmk_wipe,
	.generator = crypt_iv_lmk_gen,
	.post	   = crypt_iv_lmk_post
};

static const struct crypt_iv_operations crypt_iv_tcw_ops = {
	.ctr	   = crypt_iv_tcw_ctr,
	.dtr	   = crypt_iv_tcw_dtr,
	.init	   = crypt_iv_tcw_init,
	.wipe	   = crypt_iv_tcw_wipe,
	.generator = crypt_iv_tcw_gen,
	.post	   = crypt_iv_tcw_post
};

static struct crypt_iv_operations crypt_iv_random_ops = {
	.generator = crypt_iv_random_gen
};

static int geniv_init_iv(struct geniv_ctx *ctx)
{
	int ret;

	DMDEBUG("IV Generation algorithm : %s\n", ctx->ivmode);

	if (ctx->ivmode == NULL)
		ctx->iv_gen_ops = NULL;
	else if (strcmp(ctx->ivmode, "plain") == 0)
		ctx->iv_gen_ops = &crypt_iv_plain_ops;
	else if (strcmp(ctx->ivmode, "plain64") == 0)
		ctx->iv_gen_ops = &crypt_iv_plain64_ops;
	else if (strcmp(ctx->ivmode, "essiv") == 0)
		ctx->iv_gen_ops = &crypt_iv_essiv_ops;
	else if (strcmp(ctx->ivmode, "benbi") == 0)
		ctx->iv_gen_ops = &crypt_iv_benbi_ops;
	else if (strcmp(ctx->ivmode, "null") == 0)
		ctx->iv_gen_ops = &crypt_iv_null_ops;
	else if (strcmp(ctx->ivmode, "lmk") == 0) {
		ctx->iv_gen_ops = &crypt_iv_lmk_ops;
		/*
		 * Version 2 and 3 is recognised according
		 * to length of provided multi-key string.
		 * If present (version 3), last key is used as IV seed.
		 * All keys (including IV seed) are always the same size.
		 */
		if (ctx->key_size % ctx->key_parts) {
			ctx->key_parts++;
			ctx->key_extra_size = ctx->key_size / ctx->key_parts;
		}
	} else if (strcmp(ctx->ivmode, "tcw") == 0) {
		ctx->iv_gen_ops = &crypt_iv_tcw_ops;
		ctx->key_parts += 2; /* IV + whitening */
		ctx->key_extra_size = ctx->iv_size + TCW_WHITENING_SIZE;
	} else if (strcmp(ctx->ivmode, "random") == 0) {
		ctx->iv_gen_ops = &crypt_iv_random_ops;
		/* Need storage space in integrity fields. */
		ctx->integrity_iv_size = ctx->iv_size;
	} else {
		DMERR("Invalid IV mode %s\n", ctx->ivmode);
		return -EINVAL;
	}

	/* Allocate IV */
	if (ctx->iv_gen_ops && ctx->iv_gen_ops->ctr) {
		ret = ctx->iv_gen_ops->ctr(ctx);
		if (ret < 0) {
			DMERR("Error creating IV for %s\n", ctx->ivmode);
			return ret;
		}
	}

	/* Initialize IV (set keys for ESSIV etc) */
	if (ctx->iv_gen_ops && ctx->iv_gen_ops->init) {
		ret = ctx->iv_gen_ops->init(ctx);
		if (ret < 0) {
			DMERR("Error creating IV for %s\n", ctx->ivmode);
			return ret;
		}
	}

	return 0;
}

static void geniv_free_tfms_aead(struct geniv_ctx *ctx)
{
	if (!ctx->tfms.tfms_aead)
		return;

	if (ctx->tfms.tfms_aead[0] && IS_ERR(ctx->tfms.tfms_aead[0])) {
		crypto_free_aead(ctx->tfms.tfms_aead[0]);
		ctx->tfms.tfms_aead[0] = NULL;
	}

	kfree(ctx->tfms.tfms_aead);
	ctx->tfms.tfms_aead = NULL;
}

static void geniv_free_tfms_skcipher(struct geniv_ctx *ctx)
{
	unsigned int i;

	if (!ctx->tfms.tfms)
		return;

	for (i = 0; i < ctx->tfms_count; i++)
		if (ctx->tfms.tfms[i] && IS_ERR(ctx->tfms.tfms[i])) {
			crypto_free_skcipher(ctx->tfms.tfms[i]);
			ctx->tfms.tfms[i] = NULL;
		}

	kfree(ctx->tfms.tfms);
	ctx->tfms.tfms = NULL;
}

static void geniv_free_tfms(struct geniv_ctx *ctx)
{
	if (geniv_integrity_aead(ctx))
		geniv_free_tfms_aead(ctx);
	else
		geniv_free_tfms_skcipher(ctx);
}

static int geniv_alloc_tfms_aead(struct crypto_aead *parent,
			    struct geniv_ctx *ctx)
{
	unsigned int reqsize, align;

	ctx->tfms.tfms_aead = kcalloc(1, sizeof(struct crypto_aead *),
			   GFP_KERNEL);
	if (!ctx->tfms.tfms_aead)
		return -ENOMEM;

	/* First instance is already allocated in geniv_init_tfm */
	ctx->tfms.tfms_aead[0] = ctx->tfm_child.tfm_aead;

	/* Setup the current cipher's request structure */
	align = crypto_aead_alignmask(parent);
	align &= ~(crypto_tfm_ctx_alignment() - 1);
	reqsize = align + sizeof(struct geniv_req_ctx) +
		  crypto_aead_reqsize(ctx->tfms.tfms_aead[0]);

	crypto_aead_set_reqsize(parent, reqsize);

	return 0;
}

/* Allocate memory for the underlying cipher algorithm. Ex: cbc(aes)
 */
static int geniv_alloc_tfms_skcipher(struct crypto_skcipher *parent,
			    struct geniv_ctx *ctx)
{
	unsigned int i, reqsize, align, err;

	ctx->tfms.tfms = kcalloc(ctx->tfms_count, sizeof(struct crypto_skcipher *),
			   GFP_KERNEL);
	if (!ctx->tfms.tfms)
		return -ENOMEM;

	/* First instance is already allocated in geniv_init_tfm */
	ctx->tfms.tfms[0] = ctx->tfm_child.tfm;
	for (i = 1; i < ctx->tfms_count; i++) {
		ctx->tfms.tfms[i] = crypto_alloc_skcipher(ctx->ciphermode, 0, 0);
		if (IS_ERR(ctx->tfms.tfms[i])) {
			err = PTR_ERR(ctx->tfms.tfms[i]);
			geniv_free_tfms(ctx);
			return err;
		}

		/* Setup the current cipher's request structure */
		align = crypto_skcipher_alignmask(parent);
		align &= ~(crypto_tfm_ctx_alignment() - 1);
		reqsize = align + sizeof(struct geniv_req_ctx) +
			  crypto_skcipher_reqsize(ctx->tfms.tfms[i]);

		crypto_skcipher_set_reqsize(parent, reqsize);
	}

	return 0;
}

static unsigned int geniv_authenckey_size(struct geniv_ctx *ctx)
{
	return ctx->key_size - ctx->key_extra_size +
		RTA_SPACE(sizeof(struct crypto_authenc_key_param));
}

/* Initialize the cipher's context with the key, ivmode and other parameters.
 * Also allocate IV generation template ciphers and initialize them.
 */
static int geniv_setkey_init(void *parent, struct geniv_key_info *info)
{
	struct geniv_ctx *ctx;
	int ret;

	if (test_bit(CRYPT_MODE_INTEGRITY_AEAD, &info->cipher_flags))
		ctx = crypto_aead_ctx((struct crypto_aead *)parent);
	else
		ctx = crypto_skcipher_ctx((struct crypto_skcipher *)parent);

	ctx->tfms_count = info->tfms_count;
	ctx->key = info->key;
	ctx->cipher_flags = info->cipher_flags;
	ctx->ivopts = info->ivopts;
	ctx->iv_offset = info->iv_offset;
	ctx->sector_size = info->sector_size;
	ctx->sector_shift = __ffs(ctx->sector_size) - SECTOR_SHIFT;

	ctx->key_size = info->key_size;
	ctx->key_parts = info->key_parts;
	ctx->key_mac_size = info->key_mac_size;
	ctx->on_disk_tag_size = info->on_disk_tag_size;

	if (geniv_integrity_hmac(ctx)) {
		ctx->authenc_key = kmalloc(geniv_authenckey_size(ctx), GFP_KERNEL);
		if (!ctx->authenc_key)
			return -ENOMEM;
	}

	if (geniv_integrity_aead(ctx))
		ret = geniv_alloc_tfms_aead((struct crypto_aead *)parent, ctx);
	else
		ret = geniv_alloc_tfms_skcipher((struct crypto_skcipher *)parent, ctx);
	if (ret)
		return ret;

	ret = geniv_init_iv(ctx);

	if (geniv_integrity_aead(ctx))
		ctx->integrity_tag_size = ctx->on_disk_tag_size - ctx->integrity_iv_size;

	return ret;
}

/*
 * If AEAD is composed like authenc(hmac(sha256),xts(aes)),
 * the key must be for some reason in special format.
 * This function converts cc->key to this special format.
 */
static void crypt_copy_authenckey(char *p, const void *key,
			unsigned int enckeylen, unsigned int authkeylen)
{
	struct crypto_authenc_key_param *param;
	struct rtattr *rta;

	rta = (struct rtattr *)p;
	param = RTA_DATA(rta);
	param->enckeylen = cpu_to_be32(enckeylen);
	rta->rta_len = RTA_LENGTH(sizeof(*param));
	rta->rta_type = CRYPTO_AUTHENC_KEYA_PARAM;
	p += RTA_SPACE(sizeof(*param));
	memcpy(p, key + enckeylen, authkeylen);
	p += authkeylen;
	memcpy(p, key, enckeylen);
}

static int geniv_setkey_tfms_aead(struct crypto_aead *parent, struct geniv_ctx *ctx,
			     struct geniv_key_info *info)
{
	unsigned int key_size;
	unsigned int authenc_key_size;
	struct crypto_aead *child_aead;
	int ret = 0;

	/* Ignore extra keys (which are used for IV etc) */
	key_size = ctx->key_size - ctx->key_extra_size;
	authenc_key_size = key_size + RTA_SPACE(sizeof(struct crypto_authenc_key_param));

	child_aead = ctx->tfms.tfms_aead[0];
	crypto_aead_clear_flags(child_aead, CRYPTO_TFM_REQ_MASK);
	crypto_aead_set_flags(child_aead, crypto_aead_get_flags(parent) & CRYPTO_TFM_REQ_MASK);

	if (geniv_integrity_hmac(ctx)) {
		if (key_size < ctx->key_mac_size)
			return -EINVAL;

		crypt_copy_authenckey(ctx->authenc_key, ctx->key, key_size - ctx->key_mac_size,
				      ctx->key_mac_size);
	}

	if (geniv_integrity_hmac(ctx))
		ret = crypto_aead_setkey(child_aead, ctx->authenc_key, authenc_key_size);
	else
		ret = crypto_aead_setkey(child_aead, ctx->key, key_size);
	if (ret) {
		DMERR("Error setting key for tfms[0]\n");
		goto out;
	}

	crypto_aead_set_flags(parent, crypto_aead_get_flags(child_aead) & CRYPTO_TFM_RES_MASK);

out:
	if (geniv_integrity_hmac(ctx))
		memzero_explicit(ctx->authenc_key, authenc_key_size);

	return ret;
}

static int geniv_setkey_tfms_skcipher(struct crypto_skcipher *parent, struct geniv_ctx *ctx,
			     struct geniv_key_info *info)
{
	unsigned int subkey_size;
	char *subkey;
	struct crypto_skcipher *child;
	int ret, i;

	/* Ignore extra keys (which are used for IV etc) */
	subkey_size = (ctx->key_size - ctx->key_extra_size)
		      >> ilog2(ctx->tfms_count);

	for (i = 0; i < ctx->tfms_count; i++) {
		child = ctx->tfms.tfms[i];
		crypto_skcipher_clear_flags(child, CRYPTO_TFM_REQ_MASK);
		crypto_skcipher_set_flags(child,
			crypto_skcipher_get_flags(parent) & CRYPTO_TFM_REQ_MASK);

		subkey = ctx->key + (subkey_size) * i;

		ret = crypto_skcipher_setkey(child, subkey, subkey_size);
		if (ret) {
			DMERR("Error setting key for tfms[%d]\n", i);
			return ret;
		}

		crypto_skcipher_set_flags(parent, crypto_skcipher_get_flags(child) &
					  CRYPTO_TFM_RES_MASK);
	}

	return 0;
}

static int geniv_setkey_set(struct geniv_ctx *ctx)
{
	if (ctx->iv_gen_ops && ctx->iv_gen_ops->init)
		return ctx->iv_gen_ops->init(ctx);
	else
		return 0;
}

static int geniv_setkey_wipe(struct geniv_ctx *ctx)
{
	int ret;

	if (ctx->iv_gen_ops && ctx->iv_gen_ops->wipe) {
		ret = ctx->iv_gen_ops->wipe(ctx);
		if (ret)
			return ret;
	}

	if (geniv_integrity_hmac(ctx))
		kzfree(ctx->authenc_key);

	return 0;
}

static int geniv_setkey(void *parent, const u8 *key, unsigned int keylen)
{
	int err = 0;
	struct geniv_ctx *ctx;
	struct geniv_key_info *info = (struct geniv_key_info *) key;

	if (test_bit(CRYPT_MODE_INTEGRITY_AEAD, &info->cipher_flags))
		ctx = crypto_aead_ctx((struct crypto_aead *)parent);
	else
		ctx = crypto_skcipher_ctx((struct crypto_skcipher *)parent);

	DMDEBUG("SETKEY Operation : %d\n", info->keyop);

	switch (info->keyop) {
	case SETKEY_OP_INIT:
		err = geniv_setkey_init(parent, info);
		break;
	case SETKEY_OP_SET:
		err = geniv_setkey_set(ctx);
		break;
	case SETKEY_OP_WIPE:
		err = geniv_setkey_wipe(ctx);
		break;
	}

	if (err)
		return err;

	if (test_bit(CRYPT_MODE_INTEGRITY_AEAD, &info->cipher_flags))
		return geniv_setkey_tfms_aead((struct crypto_aead *)parent, ctx, info);
	else
		return geniv_setkey_tfms_skcipher((struct crypto_skcipher *)parent, ctx, info);
}

static int geniv_aead_setkey(struct crypto_aead *parent,
				const u8 *key, unsigned int keylen)
{
	return geniv_setkey(parent, key, keylen);
}

static int geniv_skcipher_setkey(struct crypto_skcipher *parent,
				const u8 *key, unsigned int keylen)
{
	return geniv_setkey(parent, key, keylen);
}

static void geniv_async_done(struct crypto_async_request *async_req, int error);

static int geniv_alloc_subreq_aead(struct geniv_ctx *ctx,
					struct geniv_req_ctx *rctx,
					u32 req_flags)
{
	struct aead_request *req;

	if (!rctx->subreq) {
		rctx->subreq = mempool_alloc(ctx->subreq_pool, GFP_NOIO);
		if (!rctx->subreq)
			return -ENOMEM;
	}

	req = &rctx->subreq->r.req_aead;
	rctx->subreq->rctx = rctx;

	aead_request_set_tfm(req, ctx->tfms.tfms_aead[0]);
	aead_request_set_callback(req, req_flags,
					geniv_async_done, rctx->subreq);

	return 0;
}

/* req_flags: flags from parent request */
static int geniv_alloc_subreq_skcipher(struct geniv_ctx *ctx,
					struct geniv_req_ctx *rctx,
					u32 req_flags)
{
	int key_index;
	struct skcipher_request *req;

	if (!rctx->subreq) {
		rctx->subreq = mempool_alloc(ctx->subreq_pool, GFP_NOIO);
		if (!rctx->subreq)
			return -ENOMEM;
	}

	req = &rctx->subreq->r.req;
	rctx->subreq->rctx = rctx;

	key_index = rctx->cc_sector & (ctx->tfms_count - 1);

	skcipher_request_set_tfm(req, ctx->tfms.tfms[key_index]);
	skcipher_request_set_callback(req, req_flags,
					geniv_async_done, rctx->subreq);

	return 0;
}

/* Asynchronous IO completion callback for each sector in a segment. When all
 * pending i/o are completed the parent cipher's async function is called.
 */
static void geniv_async_done(struct crypto_async_request *async_req, int error)
{
	struct geniv_subreq *subreq =
		(struct geniv_subreq *) async_req->data;
	struct geniv_req_ctx *rctx = subreq->rctx;
	struct skcipher_request *req = NULL;
	struct aead_request *req_aead = NULL;
	struct geniv_ctx *ctx;
	u8 *iv;

	if (!rctx->is_aead_request) {
		req = rctx->r.req;
		ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	} else {
		req_aead = rctx->r.req_aead;
		ctx = crypto_aead_ctx(crypto_aead_reqtfm(req_aead));
	}

	/*
	 * A request from crypto driver backlog is going to be processed now,
	 * finish the completion and continue in crypt_convert().
	 * (Callback will be called for the second time for this request.)
	 */
	if (error == -EINPROGRESS) {
		complete(&rctx->restart);
		return;
	}

	iv = iv_of_subreq(ctx, subreq);
	if (!error && ctx->iv_gen_ops && ctx->iv_gen_ops->post)
		error = ctx->iv_gen_ops->post(ctx, rctx, subreq, iv);

	mempool_free(subreq, ctx->subreq_pool);

	/* req_pending needs to be checked before req->base.complete is called
	 * as we need 'req_pending' to be equal to 1 to ensure all subrequests
	 * are processed.
	 */
	if (atomic_dec_and_test(&rctx->req_pending)) {
		/* Call the parent cipher's completion function */
		if (!rctx->is_aead_request)
			skcipher_request_complete(req, error);
		else
			aead_request_complete(req_aead, error);

	}
}

static unsigned int geniv_get_sectors(struct scatterlist *sg1,
				      struct scatterlist *sg2,
				      unsigned int segments)
{
	unsigned int i, n1, n2;

	n1 = n2 = 0;
	for (i = 0; i < segments ; i++) {
		n1 += sg1[i].length >> SECTOR_SHIFT;
		n1 += (sg1[i].length & SECTOR_MASK) ? 1 : 0;
	}

	for (i = 0; i < segments ; i++) {
		n2 += sg2[i].length >> SECTOR_SHIFT;
		n2 += (sg2[i].length & SECTOR_MASK) ? 1 : 0;
	}

	return n1 > n2 ? n1 : n2;
}

/* Iterate scatterlist of segments to retrieve the 512-byte sectors so that
 * unique IVs could be generated for each 512-byte sector. This split may not
 * be necessary e.g. when these ciphers are modelled in hardware, where it can
 * make use of the hardware's IV generation capabilities.
 */
static int geniv_iter_block(void *req_in,
			struct geniv_ctx *ctx, struct geniv_req_ctx *rctx)

{
	unsigned int rem;
	struct scatterlist *src_org, *dst_org;
	struct scatterlist *src1, *dst1;
	struct scatterlist_iter *iter = &rctx->iter;
	struct skcipher_request *req;
	struct aead_request *req_aead;

	if (unlikely(iter->seg_no >= rctx->nents))
		return 0;

	if (geniv_integrity_aead(ctx)) {
		req_aead = (struct aead_request *)req_in;
		src_org = &req_aead->src[0];
		dst_org = &req_aead->dst[0];
	} else {
		req = (struct skcipher_request *)req_in;
		src_org = &req->src[0];
		dst_org = &req->dst[0];
	}

	src1 = &src_org[iter->seg_no];
	dst1 = &dst_org[iter->seg_no];
	iter->done += iter->len;

	if (iter->done >= src1->length) {
		iter->seg_no++;

		if (iter->seg_no >= rctx->nents)
			return 0;

		src1 = &src_org[iter->seg_no];
		dst1 = &dst_org[iter->seg_no];
		iter->done = 0;
	}

	rem = src1->length - iter->done;

	iter->len = rem > ctx->sector_size ? ctx->sector_size : rem;

	DMDEBUG("segment:(%d/%u),  done:%d, rem:%d\n",
		iter->seg_no, rctx->nents, iter->done, rem);

	return iter->len;
}

static u8 *org_iv_of_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	return iv_of_subreq(ctx, subreq) + ctx->iv_size;
}

static uint64_t *org_sector_of_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	u8 *ptr = iv_of_subreq(ctx, subreq) + ctx->iv_size + ctx->iv_size;

	return (uint64_t *) ptr;
}

static unsigned int *org_tag_of_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	u8 *ptr = iv_of_subreq(ctx, subreq) + ctx->iv_size +
		  ctx->iv_size + sizeof(uint64_t);

	return (unsigned int *)ptr;
}

static void *tag_from_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	return &subreq->rctx->integrity_metadata[*org_tag_of_subreq(ctx, subreq) *
		ctx->on_disk_tag_size];
}

static void *iv_tag_from_subreq(struct geniv_ctx *ctx, struct geniv_subreq *subreq)
{
	return tag_from_subreq(ctx, subreq) + ctx->integrity_tag_size;
}

static int geniv_convert_block_aead(struct geniv_ctx *ctx,
				     struct geniv_req_ctx *rctx,
				     struct geniv_subreq *subreq,
				     unsigned int tag_offset)
{
	struct scatterlist *sg_in, *sg_out;
	u8 *iv, *org_iv, *tag_iv, *tag;
	uint64_t *sector;
	int r = 0;
	struct scatterlist_iter *iter = &rctx->iter;
	struct aead_request *req_aead;
	struct aead_request *parent_req = rctx->r.req_aead;

	BUG_ON(ctx->integrity_iv_size && ctx->integrity_iv_size != ctx->iv_size);

	/* Reject unexpected unaligned bio. */
	if (unlikely(iter->len & (ctx->sector_size - 1)))
		return -EIO;

	subreq->iv_sector = rctx->cc_sector;
	if (test_bit(CRYPT_IV_LARGE_SECTORS, &ctx->cipher_flags))
		subreq->iv_sector >>= ctx->sector_shift;

	*org_tag_of_subreq(ctx, subreq) = tag_offset;

	sector = org_sector_of_subreq(ctx, subreq);
	*sector = cpu_to_le64(rctx->cc_sector - ctx->iv_offset);

	iv = iv_of_subreq(ctx, subreq);
	org_iv = org_iv_of_subreq(ctx, subreq);
	tag = tag_from_subreq(ctx, subreq);
	tag_iv = iv_tag_from_subreq(ctx, subreq);

	sg_in = subreq->sg_in;
	sg_out = subreq->sg_out;

	/* AEAD request:
	 *  |----- AAD -------|------ DATA -------|-- AUTH TAG --|
	 *  | (authenticated) | (auth+encryption) |              |
	 *  | sector_LE |  IV |  sector in/out    |  tag in/out  |
	 */
	sg_init_table(sg_in, 4);
	sg_set_buf(&sg_in[0], sector, sizeof(uint64_t));
	sg_set_buf(&sg_in[1], org_iv, ctx->iv_size);
	sg_set_page(&sg_in[2], sg_page(&parent_req->src[iter->seg_no]),
			iter->len, parent_req->src[iter->seg_no].offset + iter->done);
	sg_set_buf(&sg_in[3], tag, ctx->integrity_tag_size);

	sg_init_table(sg_out, 4);
	sg_set_buf(&sg_out[0], sector, sizeof(uint64_t));
	sg_set_buf(&sg_out[1], org_iv, ctx->iv_size);
	sg_set_page(&sg_out[2], sg_page(&parent_req->dst[iter->seg_no]),
			iter->len, parent_req->dst[iter->seg_no].offset + iter->done);
	sg_set_buf(&sg_out[3], tag, ctx->integrity_tag_size);

	if (ctx->iv_gen_ops) {
		/* For READs use IV stored in integrity metadata */
		if (ctx->integrity_iv_size && !rctx->is_write) {
			memcpy(org_iv, tag_iv, ctx->iv_size);
		} else {
			r = ctx->iv_gen_ops->generator(ctx, rctx, subreq, org_iv);
			if (r < 0)
				return r;
			/* Store generated IV in integrity metadata */
			if (ctx->integrity_iv_size)
				memcpy(tag_iv, org_iv, ctx->iv_size);
		}
		/* Working copy of IV, to be modified in crypto API */
		memcpy(iv, org_iv, ctx->iv_size);
	}

	req_aead = &subreq->r.req_aead;
	aead_request_set_ad(req_aead, sizeof(uint64_t) + ctx->iv_size);
	if (rctx->is_write) {
		aead_request_set_crypt(req_aead, subreq->sg_in, subreq->sg_out,
				       ctx->sector_size, iv);
		r = crypto_aead_encrypt(req_aead);
		if (ctx->integrity_tag_size + ctx->integrity_iv_size != ctx->on_disk_tag_size)
			memset(tag + ctx->integrity_tag_size + ctx->integrity_iv_size, 0,
			       ctx->on_disk_tag_size - (ctx->integrity_tag_size + ctx->integrity_iv_size));
	} else {
		aead_request_set_crypt(req_aead, subreq->sg_in, subreq->sg_out,
				       ctx->sector_size + ctx->integrity_tag_size, iv);
		r = crypto_aead_decrypt(req_aead);
	}

	if (r == -EBADMSG)
		DMERR_LIMIT("INTEGRITY AEAD ERROR, sector %llu",
			    (unsigned long long)le64_to_cpu(*sector));

	if (!r && ctx->iv_gen_ops && ctx->iv_gen_ops->post)
		r = ctx->iv_gen_ops->post(ctx, rctx, subreq, org_iv);

	return r;
}

static int geniv_convert_block_skcipher(struct geniv_ctx *ctx,
					struct geniv_req_ctx *rctx,
					struct geniv_subreq *subreq,
					unsigned int tag_offset)
{
	struct scatterlist *sg_in, *sg_out;
	u8 *iv, *org_iv, *tag_iv;
	uint64_t *sector;
	int r = 0;
	struct scatterlist_iter *iter = &rctx->iter;
	struct skcipher_request *req;
	struct skcipher_request *parent_req = rctx->r.req;

	/* Reject unexpected unaligned bio. */
	if (unlikely(iter->len & (ctx->sector_size - 1)))
		return -EIO;

	subreq->iv_sector = rctx->cc_sector;
	if (test_bit(CRYPT_IV_LARGE_SECTORS, &ctx->cipher_flags))
		subreq->iv_sector >>= ctx->sector_shift;

	*org_tag_of_subreq(ctx, subreq) = tag_offset;

	iv = iv_of_subreq(ctx, subreq);
	org_iv = org_iv_of_subreq(ctx, subreq);
	tag_iv = iv_tag_from_subreq(ctx, subreq);

	sector = org_sector_of_subreq(ctx, subreq);
	*sector = cpu_to_le64(rctx->cc_sector - ctx->iv_offset);

	/* For skcipher we use only the first sg item */
	sg_in = subreq->sg_in;
	sg_out = subreq->sg_out;

	sg_init_table(sg_in, 1);
	sg_set_page(sg_in, sg_page(&parent_req->src[iter->seg_no]),
			iter->len, parent_req->src[iter->seg_no].offset + iter->done);

	sg_init_table(sg_out, 1);
	sg_set_page(sg_out, sg_page(&parent_req->dst[iter->seg_no]),
			iter->len, parent_req->dst[iter->seg_no].offset + iter->done);

	if (ctx->iv_gen_ops) {
		/* For READs use IV stored in integrity metadata */
		if (ctx->integrity_iv_size && !rctx->is_write) {
			memcpy(org_iv, tag_iv, ctx->integrity_iv_size);
		} else {
			r = ctx->iv_gen_ops->generator(ctx, rctx, subreq, org_iv);
			if (r < 0)
				return r;
			/* Store generated IV in integrity metadata */
			if (ctx->integrity_iv_size)
				memcpy(tag_iv, org_iv, ctx->integrity_iv_size);
		}
		/* Working copy of IV, to be modified in crypto API */
		memcpy(iv, org_iv, ctx->iv_size);
	}

	req = &subreq->r.req;
	skcipher_request_set_crypt(req, sg_in, sg_out, ctx->sector_size, iv);

	if (rctx->is_write)
		r = crypto_skcipher_encrypt(req);
	else
		r = crypto_skcipher_decrypt(req);

	if (!r && ctx->iv_gen_ops && ctx->iv_gen_ops->post)
		r = ctx->iv_gen_ops->post(ctx, rctx, subreq, org_iv);

	return r;
}

/* Common encryt/decrypt function for geniv template cipher. Before the crypto
 * operation, it splits the memory segments (in the scatterlist) into 512 byte
 * sectors. The initialization vector(IV) used is based on a unique sector
 * number which is generated here.
 */
static int geniv_crypt(struct geniv_ctx *ctx, void *parent_req, bool is_encrypt)
{
	struct skcipher_request *req = NULL;
	struct aead_request *req_aead = NULL;
	struct geniv_req_ctx *rctx;
	struct geniv_req_info *rinfo;
	int i, bytes, cryptlen, ret = 0;
	unsigned int sectors;
	unsigned int tag_offset = 0;
	unsigned int sector_step = ctx->sector_size >> SECTOR_SHIFT;
	char *str __maybe_unused = is_encrypt ? "encrypt" : "decrypt";

	if (geniv_integrity_aead(ctx)) {
		req_aead = (struct aead_request *)parent_req;
		rctx = geniv_aead_req_ctx(req_aead);
		rctx->r.req_aead = req_aead;
		rinfo = (struct geniv_req_info *)req_aead->iv;
	} else {
		req = (struct skcipher_request *)parent_req;
		rctx = geniv_skcipher_req_ctx(req);
		rctx->r.req = req;
		rinfo = (struct geniv_req_info *)req->iv;
	}

	/* Instance of 'struct geniv_req_info' is stored in IV ptr */
	rctx->is_write = is_encrypt;
	rctx->is_aead_request = geniv_integrity_aead(ctx);
	rctx->cc_sector = rinfo->cc_sector;
	rctx->nents = rinfo->nents;
	rctx->integrity_metadata = rinfo->integrity_metadata;
	rctx->subreq = NULL;
	cryptlen = req->cryptlen;

	rctx->iter.seg_no = 0;
	rctx->iter.done = 0;
	rctx->iter.len = 0;

	DMDEBUG("geniv:%s: starting sector=%d, #segments=%u\n", str,
		(unsigned int)rctx->cc_sector, rctx->nents);

	if (geniv_integrity_aead(ctx))
		sectors = geniv_get_sectors(req_aead->src, req_aead->dst, rctx->nents);
	else
		sectors = geniv_get_sectors(req->src, req->dst, rctx->nents);

	init_completion(&rctx->restart);
	atomic_set(&rctx->req_pending, 1);

	for (i = 0; i < sectors; i++) {
		struct geniv_subreq *subreq;

		if (geniv_integrity_aead(ctx))
			ret = geniv_alloc_subreq_aead(ctx, rctx, req_aead->base.flags);
		else
			ret = geniv_alloc_subreq_skcipher(ctx, rctx, req->base.flags);
		if (ret)
			return -ENOMEM;

		subreq = rctx->subreq;

		atomic_inc(&rctx->req_pending);

		if (geniv_integrity_aead(ctx))
			bytes = geniv_iter_block(req_aead, ctx, rctx);
		else
			bytes = geniv_iter_block(req, ctx, rctx);

		if (bytes == 0)
			break;

		cryptlen -= bytes;

		if (geniv_integrity_aead(ctx))
			ret = geniv_convert_block_aead(ctx, rctx, subreq, tag_offset);
		else
			ret = geniv_convert_block_skcipher(ctx, rctx, subreq, tag_offset);

		switch (ret) {
		/*
		 * The request was queued by a crypto driver
		 * but the driver request queue is full, let's wait.
		 */
		case -EBUSY:
			wait_for_completion(&rctx->restart);
			reinit_completion(&rctx->restart);
			/* fall through */
		/*
		 * The request is queued and processed asynchronously,
		 * completion function geniv_async_done() is called.
		 */
		case -EINPROGRESS:
			/* Marking this NULL lets the creation of a new sub-
			 * request when 'geniv_alloc_subreq' is called.
			 */
			rctx->subreq = NULL;
			rctx->cc_sector += sector_step;
			tag_offset++;
			cond_resched();
			break;
		/*
		 * The request was already processed (synchronously).
		 */
		case 0:
			atomic_dec(&rctx->req_pending);
			rctx->cc_sector += sector_step;
			tag_offset++;
			cond_resched();
			continue;

		/* There was an error while processing the request. */
		default:
			atomic_dec(&rctx->req_pending);
			mempool_free(rctx->subreq, ctx->subreq_pool);
			atomic_dec(&rctx->req_pending);
			return ret;
		}
	}

	if (rctx->subreq)
		mempool_free(rctx->subreq, ctx->subreq_pool);

	if (atomic_dec_and_test(&rctx->req_pending))
		return 0;
	else
		return -EINPROGRESS;
}

static int geniv_skcipher_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct geniv_ctx *ctx = crypto_skcipher_ctx(tfm);

	return geniv_crypt(ctx, req, true);
}

static int geniv_skcipher_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct geniv_ctx *ctx = crypto_skcipher_ctx(tfm);

	return geniv_crypt(ctx, req, false);
}

static int geniv_aead_encrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct geniv_ctx *ctx = crypto_aead_ctx(tfm);

	return geniv_crypt(ctx, req, true);
}

static int geniv_aead_decrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct geniv_ctx *ctx = crypto_aead_ctx(tfm);

	return geniv_crypt(ctx, req, false);
}

/*
 * Workaround to parse cipher algorithm from crypto API spec.
 * The ctx->cipher is currently used only in ESSIV.
 * This should be probably done by crypto-api calls (once available...)
 */
static int geniv_blkdev_cipher(struct geniv_ctx *ctx, bool is_crypto_aead)
{
	const char *alg_name = NULL;
	char *start, *end;

	alg_name = ctx->ciphermode;
	if (!alg_name)
		return -EINVAL;

	if (is_crypto_aead) {
		alg_name = strchr(alg_name, ',');
		if (!alg_name)
			alg_name = ctx->ciphermode;
		alg_name++;
	}

	start = strchr(alg_name, '(');
	end = strchr(alg_name, ')');

	if (!start && !end) {
		ctx->cipher = kstrdup(alg_name, GFP_KERNEL);
		return ctx->cipher ? 0 : -ENOMEM;
	}

	if (!start || !end || ++start >= end)
		return -EINVAL;

	ctx->cipher = kzalloc(end - start + 1, GFP_KERNEL);
	if (!ctx->cipher)
		return -ENOMEM;

	strncpy(ctx->cipher, start, end - start);

	return 0;
}

static int geniv_init_tfm(void *tfm_tmp, bool is_crypto_aead)
{
	struct geniv_ctx *ctx;
	struct crypto_skcipher *tfm;
	struct crypto_aead *tfm_aead;
	unsigned int reqsize;
	size_t iv_size_padding;
	char *algname;
	int psize, ret;

	if (is_crypto_aead) {
		tfm_aead = (struct crypto_aead *)tfm_tmp;
		ctx = crypto_aead_ctx(tfm_aead);
		algname = (char *) crypto_tfm_alg_name(crypto_aead_tfm(tfm_aead));
	} else {
		tfm = (struct crypto_skcipher *)tfm_tmp;
		ctx = crypto_skcipher_ctx(tfm);
		algname = (char *) crypto_tfm_alg_name(crypto_skcipher_tfm(tfm));
	}

	ctx->ciphermode = kmalloc(CRYPTO_MAX_ALG_NAME, GFP_KERNEL);
	if (!ctx->ciphermode)
		return -ENOMEM;

	ctx->algname = kmalloc(CRYPTO_MAX_ALG_NAME, GFP_KERNEL);
	if (!ctx->algname) {
		ret = -ENOMEM;
		goto free_ciphermode;
	}

	strlcpy(ctx->algname, algname, CRYPTO_MAX_ALG_NAME);
	algname = ctx->algname;

	/* Parse the algorithm name 'ivmode(ciphermode)' */
	ctx->ivmode = strsep(&algname, "(");
	strlcpy(ctx->ciphermode, algname, CRYPTO_MAX_ALG_NAME);
	ctx->ciphermode[strlen(algname) - 1] = '\0';

	DMDEBUG("ciphermode=%s, ivmode=%s\n", ctx->ciphermode, ctx->ivmode);

	/*
	 * Usually the underlying cipher instances are spawned here, but since
	 * the value of tfms_count (which is equal to the key_count) is not
	 * known yet, create only one instance and delay the creation of the
	 * rest of the instances of the underlying cipher 'cbc(aes)' until
	 * the setkey operation is invoked.
	 * The first instance created i.e. ctx->child will later be assigned as
	 * the 1st element in the array ctx->tfms. Creation of atleast one
	 * instance of the cipher is necessary to be created here to uncover
	 * any errors earlier than during the setkey operation later where the
	 * remaining instances are created.
	 */
	if (is_crypto_aead)
		ctx->tfm_child.tfm_aead = crypto_alloc_aead(ctx->ciphermode, 0, 0);
	else
		ctx->tfm_child.tfm = crypto_alloc_skcipher(ctx->ciphermode, 0, 0);
	if (IS_ERR(ctx->tfm_child.tfm)) {
		ret = PTR_ERR(ctx->tfm_child.tfm);
		DMERR("Failed to create cipher %s. err %d\n",
		      ctx->ciphermode, ret);
		goto free_algname;
	}

	/* Setup the current cipher's request structure */
	if (is_crypto_aead) {
		reqsize = sizeof(struct geniv_req_ctx) + __alignof__(struct geniv_req_ctx);
		crypto_aead_set_reqsize(tfm_aead, reqsize);

		ctx->iv_start = sizeof(struct geniv_subreq);
		ctx->iv_start += crypto_aead_reqsize(ctx->tfm_child.tfm_aead);

		ctx->iv_size = crypto_aead_ivsize(tfm_aead);
	} else {
		reqsize = sizeof(struct geniv_req_ctx) + __alignof__(struct geniv_req_ctx);
		crypto_skcipher_set_reqsize(tfm, reqsize);

		ctx->iv_start = sizeof(struct geniv_subreq);
		ctx->iv_start += crypto_skcipher_reqsize(ctx->tfm_child.tfm);

		ctx->iv_size = crypto_skcipher_ivsize(tfm);
	}
	/* at least a 64 bit sector number should fit in our buffer */
	if (ctx->iv_size)
		ctx->iv_size = max(ctx->iv_size,
				  (unsigned int)(sizeof(u64) / sizeof(u8)));

	if (is_crypto_aead) {
		if (crypto_aead_alignmask(tfm_aead) < CRYPTO_MINALIGN) {
			/* Allocate the padding exactly */
			iv_size_padding = -ctx->iv_start
					& crypto_aead_alignmask(ctx->tfm_child.tfm_aead);
		} else {
			/*
			 * If the cipher requires greater alignment than kmalloc
			 * alignment, we don't know the exact position of the
			 * initialization vector. We must assume worst case.
			 */
			iv_size_padding = crypto_aead_alignmask(ctx->tfm_child.tfm_aead);
		}
	} else {
		if (crypto_skcipher_alignmask(tfm) < CRYPTO_MINALIGN) {
			iv_size_padding = -ctx->iv_start
					& crypto_skcipher_alignmask(ctx->tfm_child.tfm);
		} else {
			iv_size_padding = crypto_skcipher_alignmask(ctx->tfm_child.tfm);
		}
	}

	/* create memory pool for sub-request structure
	 *  ...| IV + padding | original IV | original sec. number | bio tag offset |
	 */
	psize = ctx->iv_start + iv_size_padding + ctx->iv_size + ctx->iv_size +
		sizeof(uint64_t) + sizeof(unsigned int);

	ctx->subreq_pool = mempool_create_kmalloc_pool(MIN_IOS, psize);
	if (!ctx->subreq_pool) {
		ret = -ENOMEM;
		DMERR("Could not allocate crypt sub-request mempool\n");
		goto free_tfm;
	}

	ret = geniv_blkdev_cipher(ctx, is_crypto_aead);
	if (ret < 0) {
		ret = -ENOMEM;
		DMERR("Cannot allocate cipher string\n");
		goto free_tfm;
	}

	return 0;

free_tfm:
	if (is_crypto_aead)
		crypto_free_aead(ctx->tfm_child.tfm_aead);
	else
		crypto_free_skcipher(ctx->tfm_child.tfm);
free_algname:
	kfree(ctx->algname);
free_ciphermode:
	kfree(ctx->ciphermode);
	return ret;
}

static int geniv_skcipher_init_tfm(struct crypto_skcipher *tfm)
{
	return geniv_init_tfm(tfm, 0);
}

static int geniv_aead_init_tfm(struct crypto_aead *tfm)
{
	return geniv_init_tfm(tfm, 1);
}

static void geniv_exit_tfm(struct geniv_ctx *ctx)
{
	if (ctx->iv_gen_ops && ctx->iv_gen_ops->dtr)
		ctx->iv_gen_ops->dtr(ctx);

	mempool_destroy(ctx->subreq_pool);
	geniv_free_tfms(ctx);
	kzfree(ctx->ciphermode);
	kzfree(ctx->algname);
	kzfree(ctx->cipher);
}

static void geniv_skcipher_exit_tfm(struct crypto_skcipher *tfm)
{
	struct geniv_ctx *ctx = crypto_skcipher_ctx(tfm);

	geniv_exit_tfm(ctx);
}

static void geniv_aead_exit_tfm(struct crypto_aead *tfm)
{
	struct geniv_ctx *ctx = crypto_aead_ctx(tfm);

	geniv_exit_tfm(ctx);
}

static void geniv_skcipher_free(struct skcipher_instance *inst)
{
	struct crypto_skcipher_spawn *spawn = skcipher_instance_ctx(inst);

	crypto_drop_skcipher(spawn);
	kfree(inst);
}

static void geniv_aead_free(struct aead_instance *inst)
{
	struct crypto_aead_spawn *spawn = aead_instance_ctx(inst);

	crypto_drop_aead(spawn);
	kfree(inst);
}

static int geniv_skcipher_create(struct crypto_template *tmpl,
			struct rtattr **tb, char *algname)
{
	struct crypto_attr_type *algt;
	struct skcipher_instance *inst;
	struct skcipher_alg *alg;
	struct crypto_skcipher_spawn *spawn;
	const char *cipher_name;
	int err;

	algt = crypto_get_attr_type(tb);

	cipher_name = crypto_attr_alg_name(tb[1]);

	if (IS_ERR(cipher_name))
		return PTR_ERR(cipher_name);

	inst = kzalloc(sizeof(*inst) + sizeof(*spawn), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	spawn = skcipher_instance_ctx(inst);

	crypto_set_skcipher_spawn(spawn, skcipher_crypto_instance(inst));
	err = crypto_grab_skcipher(spawn, cipher_name, 0,
				    crypto_requires_sync(algt->type,
							 algt->mask));

	if (err)
		goto err_free_inst;

	alg = crypto_spawn_skcipher_alg(spawn);

	err = -EINVAL;

	/* Only support blocks of size which is of a power of 2 */
	if (!is_power_of_2(alg->base.cra_blocksize))
		goto err_drop_spawn;

	/* algname: essiv, base.cra_name: cbc(aes) */
	err = -ENAMETOOLONG;
	if (snprintf(inst->alg.base.cra_name, CRYPTO_MAX_ALG_NAME, "%s(%s)",
		     algname, alg->base.cra_name) >= CRYPTO_MAX_ALG_NAME)
		goto err_drop_spawn;
	if (snprintf(inst->alg.base.cra_driver_name, CRYPTO_MAX_ALG_NAME,
		     "%s(%s)", algname, alg->base.cra_driver_name) >=
	    CRYPTO_MAX_ALG_NAME)
		goto err_drop_spawn;

	inst->alg.base.cra_flags = CRYPTO_ALG_TYPE_BLKCIPHER;
	inst->alg.base.cra_priority = alg->base.cra_priority;
	inst->alg.base.cra_blocksize = alg->base.cra_blocksize;
	inst->alg.base.cra_alignmask = alg->base.cra_alignmask;
	inst->alg.base.cra_flags = alg->base.cra_flags & CRYPTO_ALG_ASYNC;
	inst->alg.ivsize = alg->base.cra_blocksize;
	inst->alg.chunksize = crypto_skcipher_alg_chunksize(alg);
	inst->alg.min_keysize = sizeof(struct geniv_key_info);
	inst->alg.max_keysize = sizeof(struct geniv_key_info);

	inst->alg.setkey = geniv_skcipher_setkey;
	inst->alg.encrypt = geniv_skcipher_encrypt;
	inst->alg.decrypt = geniv_skcipher_decrypt;

	inst->alg.base.cra_ctxsize = sizeof(struct geniv_ctx);

	inst->alg.init = geniv_skcipher_init_tfm;
	inst->alg.exit = geniv_skcipher_exit_tfm;

	inst->free = geniv_skcipher_free;

	err = skcipher_register_instance(tmpl, inst);
	if (err)
		goto err_drop_spawn;

out:
	return err;

err_drop_spawn:
	crypto_drop_skcipher(spawn);
err_free_inst:
	kfree(inst);
	goto out;
}


static int geniv_aead_create(struct crypto_template *tmpl,
			struct rtattr **tb, char *algname)
{
	struct crypto_attr_type *algt;
	struct aead_instance *inst;
	struct aead_alg *alg;
	struct crypto_aead_spawn *spawn;
	const char *cipher_name;
	int err;

	algt = crypto_get_attr_type(tb);

	cipher_name = crypto_attr_alg_name(tb[1]);
	if (IS_ERR(cipher_name))
		return PTR_ERR(cipher_name);

	inst = kzalloc(sizeof(*inst) + sizeof(*spawn), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	spawn = aead_instance_ctx(inst);

	crypto_set_aead_spawn(spawn, aead_crypto_instance(inst));
	err = crypto_grab_aead(spawn, cipher_name, 0,
				    crypto_requires_sync(algt->type,
							 algt->mask));
	if (err)
		goto err_free_inst;

	alg = crypto_spawn_aead_alg(spawn);

	/* Only support blocks of size which is of a power of 2 */
	if (!is_power_of_2(alg->base.cra_blocksize)) {
		err = -EINVAL;
		goto err_drop_spawn;
	}

	/* algname: essiv, base.cra_name: cbc(aes) */
	if (snprintf(inst->alg.base.cra_name, CRYPTO_MAX_ALG_NAME, "%s(%s)",
		     algname, alg->base.cra_name) >= CRYPTO_MAX_ALG_NAME) {
		err = -ENAMETOOLONG;
		goto err_drop_spawn;
	}

	if (snprintf(inst->alg.base.cra_driver_name, CRYPTO_MAX_ALG_NAME,
		     "%s(%s)", algname, alg->base.cra_driver_name) >=
	    CRYPTO_MAX_ALG_NAME) {
		err = -ENAMETOOLONG;
		goto err_drop_spawn;
	}

	inst->alg.base.cra_flags = CRYPTO_ALG_TYPE_BLKCIPHER;
	inst->alg.base.cra_priority = alg->base.cra_priority;
	inst->alg.base.cra_blocksize = alg->base.cra_blocksize;
	inst->alg.base.cra_alignmask = alg->base.cra_alignmask;
	inst->alg.base.cra_flags = alg->base.cra_flags & CRYPTO_ALG_ASYNC;
	inst->alg.ivsize = crypto_aead_alg_ivsize(alg);
	inst->alg.chunksize = crypto_aead_alg_chunksize(alg);
	inst->alg.maxauthsize = crypto_aead_alg_maxauthsize(alg);

	inst->alg.setkey = geniv_aead_setkey;
	inst->alg.encrypt = geniv_aead_encrypt;
	inst->alg.decrypt = geniv_aead_decrypt;

	inst->alg.base.cra_ctxsize = sizeof(struct geniv_ctx);

	inst->alg.init = geniv_aead_init_tfm;
	inst->alg.exit = geniv_aead_exit_tfm;

	inst->free = geniv_aead_free;

	err = aead_register_instance(tmpl, inst);
	if (err)
		goto err_drop_spawn;

	return 0;

err_drop_spawn:
	crypto_drop_aead(spawn);
err_free_inst:
	kfree(inst);
	return err;
}

static int geniv_create(struct crypto_template *tmpl,
			struct rtattr **tb, char *algname)
{
	if (!crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_SKCIPHER))
		return geniv_skcipher_create(tmpl, tb, algname);
	else if (!crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_AEAD))
		return geniv_aead_create(tmpl, tb, algname);
	else
		return -EINVAL;
}

static int geniv_template_create(struct crypto_template *tmpl,
			       struct rtattr **tb)
{
	return geniv_create(tmpl, tb, tmpl->name);
}

#define DEFINE_CRYPTO_TEMPLATE(type) \
	{ .name = type, \
	.create = geniv_template_create, \
	.module = THIS_MODULE, },

static struct crypto_template geniv_tmpl[IV_TYPE_NUM] = {
	DEFINE_CRYPTO_TEMPLATE("plain")
	DEFINE_CRYPTO_TEMPLATE("plain64")
	DEFINE_CRYPTO_TEMPLATE("essiv")
	DEFINE_CRYPTO_TEMPLATE("benbi")
	DEFINE_CRYPTO_TEMPLATE("null")
	DEFINE_CRYPTO_TEMPLATE("lmk")
	DEFINE_CRYPTO_TEMPLATE("tcw")
	DEFINE_CRYPTO_TEMPLATE("random")
};

static int __init geniv_init(void)
{
	return crypto_register_template_array(geniv_tmpl, IV_TYPE_NUM);
}

static void __exit geniv_exit(void)
{
	crypto_unregister_template_array(geniv_tmpl, IV_TYPE_NUM);
}

module_init(geniv_init);
module_exit(geniv_exit);

MODULE_AUTHOR("Xiongfeng Wang <xiongfeng.wang@linaro.org>");
MODULE_DESCRIPTION(DM_NAME " IV Generation Template ");
MODULE_LICENSE("GPL");
