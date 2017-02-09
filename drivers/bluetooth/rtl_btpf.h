/*
 *
 *  Realtek Bluetooth Profile profiling driver
 *
 *  Copyright (C) 2015 Realtek Semiconductor Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <net/bluetooth/hci_core.h>
#include <linux/list.h>

#define rtlbt_dbg(fmt, ...) \
		pr_debug("rtl_btpf: " fmt "\n", ##__VA_ARGS__)
#define rtlbt_info(fmt, ...) \
		pr_info("rtl_btpf: " fmt "\n", ##__VA_ARGS__)
#define rtlbt_warn(fmt, ...) \
		pr_warn("rtl_btpf: " fmt "\n", ##__VA_ARGS__)
#define rtlbt_err(fmt, ...) \
		pr_err("rtl_btpf: " fmt "\n", ##__VA_ARGS__)

#define HCI_VENDOR_SET_PF_REPORT_CMD	0xfc19
#define HCI_VENDOR_SET_BITPOOL_CMD	0xfc51

#define PAN_PACKET_COUNT                5

#define ACL_CONN	0x0
#define SYNC_CONN	0x1
#define LE_CONN		0x2

#define PSM_SDP     0x0001
#define PSM_RFCOMM  0x0003
#define PSM_PAN     0x000F
#define PSM_HID     0x0011
#define PSM_HID_INT 0x0013
#define PSM_AVCTP   0x0017
#define PSM_AVDTP   0x0019
#define PSM_FTP     0x1001
#define PSM_BIP     0x1003
#define PSM_OPP     0x1015

#define MAX_PROFILE_NUM		7
enum __profile_type {
	PROFILE_SCO = 0,
	PROFILE_HID = 1,
	PROFILE_A2DP = 2,
	PROFILE_PAN = 3,
	PROFILE_HID2 = 4, /* hid interval */
	PROFILE_HOGP = 5,
	PROFILE_VOICE = 6,
	PROFILE_MAX = 7
};

struct pf_pkt_icount {
	u32 a2dp;
	u32 pan;
	u32 hogp;
	u32 voice;
};

#define RTL_FROM_REMOTE		0
#define RTL_TO_REMOTE		1

#define RTL_PROFILE_MATCH_HANDLE	(1 << 0)
#define RTL_PROFILE_MATCH_SCID		(1 << 1)
#define RTL_PROFILE_MATCH_DCID		(1 << 2)
struct rtl_profile_id {
	u16	match_flags;
	u16	handle;
	u16	dcid;
	u16	scid;
};

struct rtl_profile {
	struct list_head list;
	u16 handle;
	u16 psm;
	u16 dcid;
	u16 scid;
	u8  idx;
};

struct rtl_hci_conn {
	struct list_head list;
	u16 handle;
	u8 type;
	u8 pf_bits;
	int pf_refs[MAX_PROFILE_NUM];
};

struct rtl_btpf {
	u16   hci_rev;
	u16   lmp_subver;

	struct hci_dev		*hdev;
	struct list_head	pf_list;
	struct list_head	conn_list;

	u8	pf_bits;
	u8	pf_state;
	int	pf_refs[MAX_PROFILE_NUM];

	struct pf_pkt_icount	icount;

	/* Monitor timers */
	struct timer_list	a2dp_timer;
	struct timer_list	pan_timer;

	struct workqueue_struct *workq;
	struct work_struct hci_work;

	struct socket *hci_sock;
#define BTPF_HCI_SOCK		1
#define BTPF_CID_RTL		2
	unsigned long flags;
};

#ifdef __LITTLE_ENDIAN
struct sbc_frame_hdr {
	u8 syncword:8;		/* Sync word */
	u8 subbands:1;		/* Subbands */
	u8 allocation_method:1;	/* Allocation method */
	u8 channel_mode:2;		/* Channel mode */
	u8 blocks:2;		/* Blocks */
	u8 sampling_frequency:2;	/* Sampling frequency */
	u8 bitpool:8;		/* Bitpool */
	u8 crc_check:8;		/* CRC check */
} __packed;

struct rtp_header {
	unsigned cc:4;
	unsigned x:1;
	unsigned p:1;
	unsigned v:2;

	unsigned pt:7;
	unsigned m:1;

	u16 sequence_number;
	u32 timestamp;
	u32 ssrc;
	u32 csrc[0];
} __packed;

#else /* !__LITTLE_ENDIAN */
struct sbc_frame_hdr {
	u8 syncword:8;		/* Sync word */
	u8 sampling_frequency:2;	/* Sampling frequency */
	u8 blocks:2;		/* Blocks */
	u8 channel_mode:2;		/* Channel mode */
	u8 allocation_method:1;	/* Allocation method */
	u8 subbands:1;		/* Subbands */
	u8 bitpool:8;		/* Bitpool */
	u8 crc_check:8;		/* CRC check */
} __packed;

struct rtp_header {
	unsigned v:2;
	unsigned p:1;
	unsigned x:1;
	unsigned cc:4;

	unsigned m:1;
	unsigned pt:7;

	u16 sequence_number;
	u32 timestamp;
	u32 ssrc;
	u32 csrc[0];
} __packed;
#endif /* __LITTLE_ENDIAN */

void rtl_btpf_deinit(void);
int rtl_btpf_init(void);
