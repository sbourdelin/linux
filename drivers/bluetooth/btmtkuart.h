// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 MediaTek Inc.

/*
 * Bluetooth support for MediaTek serial devices
 *
 * Author: Sean Wang <sean.wang@mediatek.com>
 *
 */

#define FIRMWARE_MT7622		"mediatek/mt7622_patch_firmware.bin"

#define MTK_STP_HDR_SIZE	4
#define MTK_STP_TLR_SIZE	2
#define MTK_WMT_HDR_SIZE	5
#define MTK_WMT_CMD_SIZE	(MTK_WMT_HDR_SIZE + MTK_STP_HDR_SIZE + \
				 MTK_STP_TLR_SIZE + HCI_ACL_HDR_SIZE)

enum {
	MTK_WMT_PATCH_DWNLD = 0x1,
	MTK_WMT_FUNC_CTRL = 0x6,
	MTK_WMT_RST = 0x7
};

struct mtk_stp_hdr {
	__u8 prefix;
	__u8 dlen1:4;
	__u8 type:4;
	__u8 dlen2:8;
	__u8 cs;
} __packed;

struct mtk_wmt_hdr {
	__u8	dir;
	__u8	op;
	__le16	dlen;
	__u8	flag;
} __packed;

struct mtk_hci_wmt_cmd {
	struct mtk_wmt_hdr hdr;
	__u8 data[16];
} __packed;

struct mtk_stp_splitter {
	u8	pad[6];
	u8	cursor;
	u16	dlen;
};

struct mtk_bt_dev {
	struct clk *clk;
	struct completion wmt_cmd;
	struct mtk_stp_splitter *sp;
};

static inline void mtk_make_stp_hdr(struct mtk_stp_hdr *hdr, u8 type, u32 dlen)
{
	__u8 *p = (__u8 *)hdr;

	hdr->prefix = 0x80;
	hdr->dlen1 = (dlen & 0xf00) >> 8;
	hdr->type = type;
	hdr->dlen2 = dlen & 0xff;
	hdr->cs = p[0] + p[1] + p[2];
}

static inline void mtk_make_wmt_hdr(struct mtk_wmt_hdr *hdr, u8 op, u16 plen,
				    u8 flag)
{
	hdr->dir = 1;
	hdr->op = op;
	hdr->dlen = cpu_to_le16(plen + 1);
	hdr->flag = flag;
}

#if IS_ENABLED(CONFIG_BT_HCIBTUART_MTK)

void *mtk_btuart_init(struct device *dev);
int mtk_btuart_setup(struct hci_dev *hdev);
int mtk_btuart_shutdown(struct hci_dev *hdev);
int mtk_btuart_send(struct hci_dev *hdev, struct sk_buff *skb);
int mtk_btuart_hci_frame(struct hci_dev *hdev, struct sk_buff *skb);
int mtk_btuart_recv(struct hci_dev *hdev, const u8 *data, size_t count);

#else

static void *mtk_btuart_init(struct device *dev)
{
	return 0;
}

static inline int mtk_btuart_setup(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

static inline int mtk_btuart_shutdown(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

static inline int mtk_btuart_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}

static int mtk_btuart_hci_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}

static inline int mtk_btuart_recv(struct hci_dev *hdev, const u8 *data,
				  size_t count)
{
	return -EOPNOTSUPP;
}

#endif
