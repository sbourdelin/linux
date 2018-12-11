/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UFS Host controller crypto driver
 *
 * Copyright (C) 2018 Cadence Design Systems, Inc.
 *
 * Authors:
 *	Parshuram Thombare <pthombar@cadence.com>
 *
 */

#ifndef _UFSHCD_CRYPTO_H_
#define _UFSHCD_CRYPTO_H_
#include "crypto/aes.h"

#define CRYPTO_CFGPTR_MASK 0xff000000
#define CRYPTO_CFGPTR_SHIFT 24
#define CRYPTO_CONFIG_CNT_MASK 0xff00
#define CRYPTO_CONFIG_CNT_SHIFT 8
#define CRYPTO_CAP_CNT_MASK 0xff
#define CRYPTO_CAP_CNT_SHIFT 0

#define CRYPTO_CAPS_KS_MASK 0xff0000
#define CRYPTO_CAPS_KS_SHIFT 16
#define CRYPTO_CAPS_SDUSB_MASK 0xff00
#define CRYPTO_CAPS_SDUSB_SHIFT 8
#define CRYPTO_CAPS_ALG_ID_MASK 0xff
#define CRYPTO_CAPS_ALG_ID_SHIFT 0

#define CRYPTO_CCONFIG16_CFGE_MASK 0x80000000
#define CRYPTO_CCONFIG16_CFGE_SHIFT 31
#define CRYPTO_CCONFIG16_CAP_IDX_MASK 0xff00
#define CRYPTO_CCONFIG16_CAP_IDX_SHIFT 8
#define CRYPTO_CONFIG_SIZE 0x80

/* UTP transfer request descriptor DW0 crypto enable */
#define CRYPTO_UTP_REQ_DESC_DWORD0_CE_MASK 0x800000
#define CRYPTO_UTP_REQ_DESC_DWORD0_CE_SHIFT 23
/* UTP transfer request descriptor DW0 crypto config index */
#define CRYPTO_UTP_REQ_DESC_DWORD0_CCI_MASK 0xff
#define CRYPTO_UTP_REQ_DESC_DWORD0_CCI_SHIFT 0

enum key_size_e {
	UFS_CRYPTO_KEY_ID_128BITS = 1,
	UFS_CRYPTO_KEY_ID_192BITS = 2,
	UFS_CRYPTO_KEY_ID_256BITS = 3,
	UFS_CRYPTO_KEY_ID_512BITS = 4,
};

enum alg_id_e {
	UFS_CRYPTO_ALG_ID_AES_XTS = 0,
	UFS_CRYPTO_ALG_ID_BITLOCKER_AES_CBC = 1,
	UFS_CRYPTO_ALG_ID_AES_ECB = 2,
	UFS_CRYPTO_ALG_ID_ESSIV_AES_CBC = 3,
};

/*
 * ufshcd_crypto_config - UFS HC config
 * @cap_idx: index in ccaps array of crypto ctx
 * @cfge: config enable bit
 * @key: crypto key
 */
struct ufshcd_crypto_config {
	u8 cap_idx;
	u8 cfge;
	u8 key[AES_MAX_KEY_SIZE];
};

/*
 * ufshcd_crypto_cap - UFS HC capability structure
 * @alg_id: algo id (alg_id_e) as per UFS spec
 * @sdusb: Supported Data Unit Size Bitmask
 * @key_id: key size id (key_size_e) as per UFS spec
 */
struct ufshcd_crypto_cap {
	u8 alg_id;
	u8 sdusb;
	u8 key_id;
};

/*
 * ufshcd_crypto_ctx - UFSHCD crypto context
 * @ccaps: UFS HC crypto capabilities array
 * @cconfigs: UFS HC configs array
 * @crypto_lock: crypto lock
 * @crypto_config_base_addr: UFS HC crypto config base address
 * @config_cnt: supported configuration count
 * @cap_cnt: supported capabilities count
 */
struct ufshcd_crypto_ctx {
	struct ufshcd_crypto_cap *ccaps;
	struct ufshcd_crypto_config *cconfigs;
	spinlock_t crypto_lock;
	unsigned int crypto_config_base_addr;
	int config_cnt;
	int cap_cnt;
};

int ufshcd_crypto_init(struct ufs_hba *hba);
void ufshcd_crypto_remove(struct ufs_hba *hba);
void ufshcd_prepare_for_crypto(struct ufs_hba *hba, struct ufshcd_lrb *lrbp);
#endif
