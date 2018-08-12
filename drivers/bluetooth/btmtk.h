/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 MediaTek Inc.
 *
 * Common operations for MediaTek Bluetooth devices
 * with the UART, USB and SDIO transport
 *
 * Author: Sean Wang <sean.wang@mediatek.com>
 *
 */

#define FIRMWARE_MT7668		"mt7668pr2h.bin"

enum {
	BTMTK_WMT_PATCH_DWNLD = 0x1,
	BTMTK_WMT_FUNC_CTRL = 0x6,
	BTMTK_WMT_RST = 0x7,
	BTMTK_WMT_SEMAPHORE = 0x17,
};

enum {
	BTMTK_WMT_INVALID,
	BTMTK_WMT_PATCH_UNDONE,
	BTMTK_WMT_PATCH_DONE,
	BTMTK_WMT_ON_UNDONE,
	BTMTK_WMT_ON_DONE,
	BTMTK_WMT_ON_PROGRESS,
};

struct btmtk_wmt_hdr {
	u8	dir;
	u8	op;
	__le16	dlen;
	u8	flag;
} __packed;

struct btmtk_hci_wmt_cmd {
	struct btmtk_wmt_hdr hdr;
	u8 data[256];
} __packed;

struct btmtk_hci_wmt_evt_funcc {
	struct btmtk_wmt_hdr hdr;
	__be16 status;
} __packed;

struct btmtk_hci_wmt_params {
	u8 op;
	u8 flag;
	u16 dlen;
	const void *data;
	u32 *status;
};

struct btmtk_func_query {
	struct hci_dev *hdev;
	int (*cmd_sync)(struct hci_dev *hdev,
			struct btmtk_hci_wmt_params *wmt_params);
};

#if IS_ENABLED(CONFIG_BT_MTK)

int
btmtk_hci_wmt_sync(struct hci_dev *hdev, struct btmtk_hci_wmt_params *params);

int
btmtk_enable(struct hci_dev *hdev, const char *fn,
	     int (*cmd_sync)(struct hci_dev *,
			     struct btmtk_hci_wmt_params *));

int
btmtk_disable(struct hci_dev *hdev,
	      int (*cmd_sync)(struct hci_dev *,
			      struct btmtk_hci_wmt_params *));
#else

static int
btmtk_hci_wmt_sync(struct hci_dev *hdev, struct btmtk_hci_wmt_params *params)
{
	return -EOPNOTSUPP;
}

static int
btmtk_enable(struct hci_dev *hdev, const char *fn,
	     int (*cmd_sync)(struct hci_dev *,
			     struct btmtk_hci_wmt_params *))
{
	return -EOPNOTSUPP;
}

static int
btmtk_disable(struct hci_dev *hdev,
	      int (*cmd_sync)(struct hci_dev *,
			      struct btmtk_hci_wmt_params *))
{
	return -EOPNOTSUPP;
}

#endif
