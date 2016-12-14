/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - device attributes
 *
 * Copyright (c) 2014,2016 Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef _UAPI_LINUX_FLEXCARD_H
#define _UAPI_LINUX_FLEXCARD_H

#include <linux/types.h>

enum flexcard_clk_type {
	FLEXCARD_CLK_1MHZ = 0x0,
	FLEXCARD_CLK_10MHZ = 0x1,
	FLEXCARD_CLK_100MHZ = 0x2,
	FLEXCARD_CLK_EXT1 = 0x11,
	FLEXCARD_CLK_EXT2 = 0x12,
};

struct flexcard_clk_desc {
	enum flexcard_clk_type type;
	__u32 freq;
};

#define FCGCLKSRC       _IOR(0xeb, 0, struct flexcard_clk_desc)
#define FCSCLKSRC       _IOW(0xeb, 1, struct flexcard_clk_desc)

struct fc_version {
	__u8	dev;
	__u8	min;
	__u8	maj;
	__u8	reserved;
} __packed;

/* PCI BAR 0: Flexcard configuration */
struct fc_bar0_conf {
	__u32 r1;			/* 000 */
	struct fc_version fc_fw_ver;	/* 004 */
	struct fc_version fc_hw_ver;	/* 008 */
	__u32 r2[3];			/* 00c */
	__u64 fc_sn;			/* 018 */
	__u32 fc_uid;			/* 020 */
	__u32 r3[7];			/* 024 */
	__u32 fc_lic[6];		/* 040 */
	__u32 fc_slic[6];		/* 058 */
	__u32 trig_ctrl1;		/* 070 */
	__u32 r4;			/* 074 */
	__u32 trig_ctrl2;		/* 078 */
	__u32 r5[22];			/* 07c */
	__u32 amreg;			/* 0d4 */
	__u32 tiny_stat;		/* 0d8 */
	__u32 r6[5];			/* 0dc */
	__u32 can_dat_cnt;		/* 0f0 */
	__u32 can_err_cnt;		/* 0f4 */
	__u32 fc_data_cnt;		/* 0f8 */
	__u32 r7;			/* 0fc */
	__u32 fc_rocr;			/* 100 */
	__u32 r8;			/* 104 */
	__u32 pg_ctrl;			/* 108 */
	__u32 pg_term;			/* 10c */
	__u32 r9;			/* 110 */
	__u32 irs;			/* 114 */
	__u32 fr_tx_cnt;		/* 118 */
	__u32 irc;			/* 11c */
	__u64 pcnt;			/* 120 */
	__u32 r10;			/* 128 */
	__u32 nmv_cnt;			/* 12c */
	__u32 info_cnt;			/* 130 */
	__u32 stat_trg_cnt;		/* 134 */
	__u32 r11;			/* 138 */
	__u32 fr_rx_cnt;		/* 13c */
} __packed;

#endif /* _UAPI_LINUX_FLEXCARD_H */
