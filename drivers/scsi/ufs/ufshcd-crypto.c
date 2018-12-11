// SPDX-License-Identifier: GPL-2.0
/*
 * UFS Host controller crypto driver
 *
 * Copyright (C) 2018 Cadence Design Systems, Inc.
 *
 * Authors:
 *	Parshuram Thombare <pthombar@cadence.com>
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <crypto/aes.h>
#include <linux/device-mapper.h>
#include "ufshcd.h"
#include "ufshcd-crypto.h"
#include "scsi/scsi_device.h"
#include "scsi/scsi_host.h"

struct ufshcd_dm_ctx {
	struct dm_dev *dev;
	sector_t start;
	unsigned short int sector_size;
	unsigned char sector_shift;
	int cci;
	int cap_idx;
	char key[AES_MAX_KEY_SIZE];
	struct ufs_hba *hba;
};

static int dm_registered;

static inline int
ufshcd_key_id_to_len(int key_id)
{
	int key_len = -1;

	switch (key_id) {
	case UFS_CRYPTO_KEY_ID_128BITS:
		key_len = AES_KEYSIZE_128;
		break;
	case UFS_CRYPTO_KEY_ID_192BITS:
		key_len = AES_KEYSIZE_192;
		break;
	case UFS_CRYPTO_KEY_ID_256BITS:
		key_len = AES_KEYSIZE_256;
		break;
	default:
		break;
	}
	return key_len;
}

static inline int
ufshcd_key_len_to_id(int key_len)
{
	int key_id = -1;

	switch (key_len) {
	case AES_KEYSIZE_128:
		key_id = UFS_CRYPTO_KEY_ID_128BITS;
		break;
	case AES_KEYSIZE_192:
		key_id = UFS_CRYPTO_KEY_ID_192BITS;
		break;
	case AES_KEYSIZE_256:
		key_id = UFS_CRYPTO_KEY_ID_256BITS;
		break;
	default:
		break;
	}
	return key_id;
}

static void
ufshcd_read_crypto_capabilities(struct ufs_hba *hba)
{
	u32 tmp, i;
	struct ufshcd_crypto_ctx *cctx = hba->cctx;

	for (i = 0; i < cctx->cap_cnt; i++) {
		tmp = ufshcd_readl(hba, REG_UFS_CRYPTOCAP + i);
		cctx->ccaps[i].key_id = (tmp & CRYPTO_CAPS_KS_MASK) >>
						CRYPTO_CAPS_KS_SHIFT;
		cctx->ccaps[i].sdusb = (tmp & CRYPTO_CAPS_SDUSB_MASK) >>
						CRYPTO_CAPS_SDUSB_SHIFT;
		cctx->ccaps[i].alg_id = (tmp & CRYPTO_CAPS_ALG_ID_MASK) >>
						CRYPTO_CAPS_ALG_ID_SHIFT;
	}
}

static inline int
ufshcd_get_cap_idx(struct ufshcd_crypto_ctx *cctx, int alg_id,
	int key_id)
{
	int cap_idx;

	for (cap_idx = 0; cap_idx < cctx->cap_cnt; cap_idx++) {
		if (((cctx->ccaps[cap_idx].alg_id == alg_id) &&
			cctx->ccaps[cap_idx].key_id == key_id))
			break;
	}
	return ((cap_idx < cctx->cap_cnt) ? cap_idx : -1);
}

static inline int
ufshcd_get_cci_slot(struct ufshcd_crypto_ctx *cctx)
{
	int  cci;

	for (cci = 0; cci < cctx->config_cnt; cci++) {
		if (!cctx->cconfigs[cci].cfge) {
			cctx->cconfigs[cci].cfge = 1;
			break;
		}
	}
	return ((cci < cctx->config_cnt) ? cci : -1);
}

static void
ufshcd_aes_ecb_set_key(struct ufshcd_dm_ctx *ctx)
{
	int i, key_size;
	u32 val, cconfig16, crypto_config_addr;
	struct ufshcd_crypto_ctx *cctx;
	struct ufshcd_crypto_config *cconfig;
	struct ufshcd_crypto_cap ccap;

	cctx = ctx->hba->cctx;
	if (ctx->cci <= 0)
		ctx->cci = ufshcd_get_cci_slot(cctx);
	/* If no slot is available, slot 0 is shared */
	ctx->cci = ctx->cci < 0 ? 0 : ctx->cci;
	cconfig = &(cctx->cconfigs[ctx->cci]);
	ccap = cctx->ccaps[ctx->cap_idx];
	key_size = ufshcd_key_id_to_len(ccap.key_id);

	if ((cconfig->cap_idx != ctx->cap_idx) ||
		((key_size > 0) &&
		memcmp(cconfig->key, ctx->key, key_size))) {
		cconfig->cap_idx = ctx->cap_idx;
		memcpy(cconfig->key, ctx->key, key_size);
		crypto_config_addr = cctx->crypto_config_base_addr +
				ctx->cci * CRYPTO_CONFIG_SIZE;
		cconfig16 = ccap.sdusb | (1 << CRYPTO_CCONFIG16_CFGE_SHIFT);
		cconfig16 |= ((ctx->cap_idx << CRYPTO_CCONFIG16_CAP_IDX_SHIFT) &
				CRYPTO_CCONFIG16_CAP_IDX_MASK);
		spin_lock(&cctx->crypto_lock);
		for (i = 0; i < key_size; i += 4) {
			val = (ctx->key[i] | (ctx->key[i + 1] << 8) |
				(ctx->key[i + 2] << 16) |
				(ctx->key[i + 3] << 24));
			ufshcd_writel(ctx->hba, val, crypto_config_addr + i);
		}
		ufshcd_writel(ctx->hba, cpu_to_le32(cconfig16),
				crypto_config_addr + (4 * 16));
		spin_unlock(&cctx->crypto_lock);
		/* Make sure keys are programmed */
		mb();
	}
}


/*
 * ufshcd_prepare_for_crypto - UFS HCD preparation before submitting UTP
 * transfer request desc. Get crypto config index from block cipher contex
 * which was set in set_key.
 * @hba: host bus adapter instance per UFS HC
 * @lrbp: UFS HCD local reference block
 */
void
ufshcd_prepare_for_crypto(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	struct bio *bio = lrbp->cmd->request->bio;
	struct ufshcd_dm_ctx *ctx = NULL;
	struct ufshcd_crypto_ctx *cctx;
	int alg_id;

#ifdef CONFIG_BLK_DEV_HW_RT_ENCRYPTION
	if (bio)
		ctx = (struct ufshcd_dm_ctx *)(bio->bi_crypto_ctx);
#endif
	if (unlikely(!bio) || !ctx)
		return;

	switch (lrbp->cmd->cmnd[0]) {
	case READ_6:
	case READ_10:
	case READ_16:
	case WRITE_6:
	case WRITE_10:
	case WRITE_16:
		cctx = ctx->hba->cctx;
		alg_id = cctx->ccaps[ctx->cap_idx].alg_id;
		switch (alg_id) {
		case UFS_CRYPTO_ALG_ID_AES_ECB:
		ufshcd_aes_ecb_set_key(ctx);
		lrbp->cci = ctx->cci;
		break;
		default:
		break;
		}
	break;

	default:
	break;
	}
}
EXPORT_SYMBOL_GPL(ufshcd_prepare_for_crypto);

static int
crypt_ctr_cipher(struct dm_target *ti, char *cipher_in, char *key)
{
	struct ufshcd_dm_ctx *ctx = ti->private;
	int alg_id;
	int ret = 0;

	if (!memcmp(cipher_in, "aes-ecb", 7))
		alg_id = UFS_CRYPTO_ALG_ID_AES_ECB;
	else
		alg_id = -1;
	ctx->cap_idx = ufshcd_get_cap_idx(ctx->hba->cctx, alg_id,
			ufshcd_key_len_to_id(strlen(key) >> 1));
	if (ctx->cap_idx >= 0) {
		switch (alg_id) {
		case UFS_CRYPTO_ALG_ID_AES_ECB:
		ret = hex2bin(ctx->key, key, (strlen(key) >> 1));
		if (!ret)
			ufshcd_aes_ecb_set_key(ctx);
		break;
		default:
		ret = -EINVAL;
		break;
		}
	} else
		ret = -EINVAL;
	return ret;
}

static int
ufshcd_crypt_map(struct dm_target *ti, struct bio *bio)
{
	struct ufshcd_dm_ctx *ctx = ti->private;

	/*
	 * If bio is REQ_PREFLUSH or REQ_OP_DISCARD, just bypass crypt queues.
	 * - for REQ_PREFLUSH device-mapper core ensures that no IO is in-flight
	 * - for REQ_OP_DISCARD caller must use flush if IO ordering matters
	 */
	if (unlikely(bio->bi_opf & REQ_PREFLUSH ||
	    bio_op(bio) == REQ_OP_DISCARD)) {
		bio_set_dev(bio, ctx->dev->bdev);
		if (bio_sectors(bio))
			bio->bi_iter.bi_sector = ctx->start +
				dm_target_offset(ti, bio->bi_iter.bi_sector);
		return DM_MAPIO_REMAPPED;
	}

	/*
	 * Check if bio is too large, split as needed.
	 */
	if (unlikely(bio->bi_iter.bi_size > (BIO_MAX_PAGES << PAGE_SHIFT)) &&
	    (bio_data_dir(bio) == WRITE))
		dm_accept_partial_bio(bio,
			((BIO_MAX_PAGES << PAGE_SHIFT) >> SECTOR_SHIFT));
	/*
	 * Ensure that bio is a multiple of internal sector encryption size
	 * and is aligned to this size as defined in IO hints.
	 */
	if (unlikely((bio->bi_iter.bi_sector &
			((ctx->sector_size >> SECTOR_SHIFT) - 1)) != 0))
		return DM_MAPIO_KILL;

	if (unlikely(bio->bi_iter.bi_size & (ctx->sector_size - 1)))
		return DM_MAPIO_KILL;
#ifdef CONFIG_BLK_DEV_HW_RT_ENCRYPTION
	bio->bi_crypto_ctx = ctx;
	bio_set_dev(bio, ctx->dev->bdev);
	if (bio_sectors(bio))
		bio->bi_iter.bi_sector = ctx->start +
			dm_target_offset(ti, bio->bi_iter.bi_sector);
	generic_make_request(bio);
#endif
	return DM_MAPIO_SUBMITTED;

}

static int
ufshcd_crypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct ufshcd_dm_ctx *ctx;
	int ret;
	unsigned long long tmpll;
	char dummy;
	struct block_device *bdev = NULL;
	struct device *device = NULL;
	struct Scsi_Host *shost = NULL;

	if (argc != 5) {
		ti->error = "Invalid no of arguments";
		ret = -EINVAL;
		goto err1;
	}

	ctx = kzalloc(sizeof(struct ufshcd_dm_ctx), GFP_KERNEL);
	if (!ctx) {
		ti->error = "Cannot allocate encryption context";
		ret = -ENOMEM;
		goto err1;
	}
	ti->private = ctx;

	if (sscanf(argv[4], "%llu%c", &tmpll, &dummy) != 1) {
		ti->error = "Invalid device sector";
		ret = -EINVAL;
		goto err2;
	}
	ctx->start = tmpll;
	ctx->sector_size = (1 << SECTOR_SHIFT);
	ctx->sector_shift = 0;

	ret = dm_get_device(ti, argv[3],
		dm_table_get_mode(ti->table), &ctx->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		ret = -ENOMEM;
		goto err2;
	}
	bdev = ctx->dev->bdev;
	ret = -EINVAL;
	if (bdev) {
		device = part_to_dev(bdev->bd_part);
		if (device) {
			shost = dev_to_shost(device);
			if (shost && !memcmp(shost->hostt->name, "ufshcd", 6)) {
				ctx->hba = shost_priv(shost);
				ret = crypt_ctr_cipher(ti, argv[0], argv[1]);
			}
		}
	}
	if (ret)
		goto err3;
	return 0;
err3:
	dm_put_device(ti, ctx->dev);
err2:
	kfree(ctx);
err1:
	return ret;
}

static void
ufshcd_crypt_dtr(struct dm_target *ti)
{
	struct ufshcd_dm_ctx *ctx = ti->private;

	if (ctx) {
		if (ctx->cci > 0 && ctx->hba)
			ctx->hba->cctx->cconfigs[ctx->cci].cfge = 0;
		dm_put_device(ti, ctx->dev);
		kfree(ctx);
		ti->private = NULL;
	}
}

static struct target_type crypt_target = {
	.name   = "crypt-ufs",
	.version = {0, 0, 1},
	.module = THIS_MODULE,
	.ctr    = ufshcd_crypt_ctr,
	.dtr    = ufshcd_crypt_dtr,
	.map    = ufshcd_crypt_map,
};

/*
 * ufshcd_crypto_init - UFS HCD crypto service initialization
 * @hba: host bus adapter instance per UFS HC
 */
int ufshcd_crypto_init(struct ufs_hba *hba)
{
	int ret = 0;
	unsigned int tmp;

	hba->cctx = kzalloc(sizeof(struct ufshcd_crypto_ctx), GFP_KERNEL);
	if (!hba->cctx) {
		ret = -ENOMEM;
		goto err1;
	}

	tmp = ufshcd_readl(hba, REG_CONTROLLER_ENABLE);
	ufshcd_writel(hba, CRYPTO_GENERAL_ENABLE | tmp, REG_CONTROLLER_ENABLE);
	tmp = ufshcd_readl(hba, REG_UFS_CCAP);
	hba->cctx->crypto_config_base_addr =
		((tmp & CRYPTO_CFGPTR_MASK) >> CRYPTO_CFGPTR_SHIFT) * 0x100;
	hba->cctx->config_cnt =
		(tmp & CRYPTO_CONFIG_CNT_MASK) >> CRYPTO_CONFIG_CNT_SHIFT;
	hba->cctx->cap_cnt =
		(tmp & CRYPTO_CAP_CNT_MASK) >> CRYPTO_CAP_CNT_SHIFT;
	hba->cctx->ccaps = kcalloc(hba->cctx->cap_cnt,
			sizeof(struct ufshcd_crypto_cap), GFP_KERNEL);
	if (!hba->cctx->ccaps) {
		ret = -ENOMEM;
		goto err2;
	}
	hba->cctx->cconfigs = kcalloc(hba->cctx->config_cnt,
			sizeof(struct ufshcd_crypto_config), GFP_KERNEL);
	if (!hba->cctx->cconfigs) {
		ret = -ENOMEM;
		goto err3;
	}
	ufshcd_read_crypto_capabilities(hba);
	spin_lock_init(&hba->cctx->crypto_lock);
	if (!dm_registered) {
		ret = dm_register_target(&crypt_target);
		if (ret < 0) {
			dev_err(hba->dev, "UFS DM register failed %d", ret);
			goto err3;
		}
		dm_registered = 1;
	}

	return 0;
err3:
	kfree(hba->cctx->ccaps);
	hba->cctx->ccaps = NULL;
err2:
	kfree(hba->cctx);
	hba->cctx = NULL;
err1:
	dev_err(hba->dev, "AES ECB algo registration failed.\n");
	return ret;
}
EXPORT_SYMBOL_GPL(ufshcd_crypto_init);

/*
 * ufshcd_crypto_remove - UFS HCD crypto service cleanup
 * @hba: host bus adapter instance per UFS HC
 */
void ufshcd_crypto_remove(struct ufs_hba *hba)
{
	dm_unregister_target(&crypt_target);
	dm_registered = 0;
	kfree(hba->cctx->ccaps);
	kfree(hba->cctx->cconfigs);
	kfree(hba->cctx);
	hba->cctx = NULL;
}
EXPORT_SYMBOL_GPL(ufshcd_crypto_remove);

MODULE_AUTHOR("Parshuram Thombare <pthombar@cadence.com>");
MODULE_DESCRIPTION("UFS host controller crypto driver");
MODULE_LICENSE("GPL v2");
