/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _HNS_ROCE_CMD_H
#define _HNS_ROCE_CMD_H

#include <linux/dma-mapping.h>

enum {
	/* Initialization and general commands */
	HNS_ROCE_CMD_SYS_EN		= 0x1,
	HNS_ROCE_CMD_SYS_DIS		= 0x2,
	HNS_ROCE_CMD_MAP_FA		= 0xfff,
	HNS_ROCE_CMD_UNMAP_FA		= 0xffe,
	HNS_ROCE_CMD_RUN_FW		= 0xff6,
	HNS_ROCE_CMD_MOD_STAT_CFG	= 0x34,
	HNS_ROCE_CMD_QUERY_DEV_CAP	= 0x3,
	HNS_ROCE_CMD_QUERY_FW		= 0x4,
	HNS_ROCE_CMD_ENABLE_LAM		= 0xff8,
	HNS_ROCE_CMD_DISABLE_LAM	= 0xff7,
	HNS_ROCE_CMD_QUERY_DDR		= 0x5,
	HNS_ROCE_CMD_QUERY_ADAPTER	= 0x6,
	HNS_ROCE_CMD_INIT_HCA		= 0x7,
	HNS_ROCE_CMD_CLOSE_HCA		= 0x8,
	HNS_ROCE_CMD_INIT_PORT		= 0x9,
	HNS_ROCE_CMD_CLOSE_PORT		= 0xa,
	HNS_ROCE_CMD_QUERY_HCA		= 0xb,
	HNS_ROCE_CMD_QUERY_PORT		= 0x43,
	HNS_ROCE_CMD_SENSE_PORT		= 0x4d,
	HNS_ROCE_CMD_SET_PORT		= 0xc,
	HNS_ROCE_CMD_ACCESS_DDR		= 0x2e,
	HNS_ROCE_CMD_MAP_ICM		= 0xffa,
	HNS_ROCE_CMD_UNMAP_ICM		= 0xff9,
	HNS_ROCE_CMD_MAP_ICM_AUX	= 0xffc,
	HNS_ROCE_CMD_UNMAP_ICM_AUX	= 0xffb,
	HNS_ROCE_CMD_SET_ICM_SIZE	= 0xffd,

	/* TPT commands */
	HNS_ROCE_CMD_SW2HW_MPT		= 0xd,
	HNS_ROCE_CMD_QUERY_MPT		= 0xe,
	HNS_ROCE_CMD_HW2SW_MPT		= 0xf,
	HNS_ROCE_CMD_READ_MTT		= 0x10,
	HNS_ROCE_CMD_WRITE_MTT		= 0x11,
	HNS_ROCE_CMD_SYNC_TPT		= 0x2f,

	/* EQ commands */
	HNS_ROCE_CMD_MAP_EQ		= 0x12,
	HNS_ROCE_CMD_SW2HW_EQ		= 0x13,
	HNS_ROCE_CMD_HW2SW_EQ		= 0x14,
	HNS_ROCE_CMD_QUERY_EQ		= 0x15,

	/* CQ commands */
	HNS_ROCE_CMD_SW2HW_CQ		= 0x16,
	HNS_ROCE_CMD_HW2SW_CQ		= 0x17,
	HNS_ROCE_CMD_QUERY_CQ		= 0x18,
	HNS_ROCE_CMD_MODIFY_CQ		= 0x2c,

	/* SRQ commands */
	HNS_ROCE_CMD_SW2HW_SRQ		= 0x35,
	HNS_ROCE_CMD_HW2SW_SRQ		= 0x36,
	HNS_ROCE_CMD_QUERY_SRQ		= 0x37,
	HNS_ROCE_CMD_ARM_SRQ		= 0x40,

	/* QP/EE commands */
	HNS_ROCE_CMD_RST2INIT_QP	= 0x19,
	HNS_ROCE_CMD_INIT2RTR_QP	= 0x1a,
	HNS_ROCE_CMD_RTR2RTS_QP		= 0x1b,
	HNS_ROCE_CMD_RTS2RTS_QP		= 0x1c,
	HNS_ROCE_CMD_SQERR2RTS_QP	= 0x1d,
	HNS_ROCE_CMD_2ERR_QP		= 0x1e,
	HNS_ROCE_CMD_RTS2SQD_QP		= 0x1f,
	HNS_ROCE_CMD_SQD2SQD_QP		= 0x38,
	HNS_ROCE_CMD_SQD2RTS_QP		= 0x20,
	HNS_ROCE_CMD_2RST_QP		= 0x21,
	HNS_ROCE_CMD_QUERY_QP		= 0x22,
	HNS_ROCE_CMD_INIT2INIT_QP	= 0x2d,
	HNS_ROCE_CMD_SUSPEND_QP		= 0x32,
	HNS_ROCE_CMD_UNSUSPEND_QP	= 0x33,

	/* Special QP and management commands */
	HNS_ROCE_CMD_CONF_SPECIAL_QP	= 0x23,
	HNS_ROCE_CMD_MAD_IFC		= 0x24,

	/* Multicast commands */
	HNS_ROCE_CMD_READ_MCG		= 0x25,
	HNS_ROCE_CMD_WRITE_MCG		= 0x26,
	HNS_ROCE_CMD_MGID_HASH		= 0x27,

	/* Miscellaneous commands */
	HNS_ROCE_CMD_DIAG_RPRT		= 0x30,
	HNS_ROCE_CMD_NOP		= 0x31,

	/* Debug commands */
	HNS_ROCE_CMD_QUERY_DEBUG_MSG	= 0x2a,
	HNS_ROCE_CMD_SET_DEBUG_MSG	= 0x2b,
};

enum {
	HNS_ROCE_CMD_TIME_CLASS_A	= 10000,
	HNS_ROCE_CMD_TIME_CLASS_B	= 10000,
	HNS_ROCE_CMD_TIME_CLASS_C	= 10000,
};

enum {
	HNS_ROCE_MAILBOX_SIZE		=  4096
};

struct hns_roce_cmd_mailbox {
	void		       *buf;
	dma_addr_t		dma;
};

int __hns_roce_cmd(struct hns_roce_dev *hr_dev, u64 in_param,
			  u64 *out_param, int out_is_imm,
			  u32 in_modifier, u8 op_modifier,
			  u16 op, unsigned long timeout);

/* Invoke a command with no output parameter */
static inline int hns_roce_cmd(struct hns_roce_dev *hr_dev,
				     u64 in_param, u32 in_modifier,
				     u8 op_modifier, u16 op,
				     unsigned long timeout)
{
	return __hns_roce_cmd(hr_dev, in_param, NULL, 0, in_modifier,
			      op_modifier, op, timeout);
}

/* Invoke a command with an output mailbox */
static inline int hns_roce_cmd_box(struct hns_roce_dev *hr_dev,
					   u64 in_param, u64 out_param,
					   u32 in_modifier, u8 op_modifier,
					   u16 op, unsigned long timeout)
{
	return __hns_roce_cmd(hr_dev, in_param, &out_param, 0, in_modifier,
			      op_modifier, op, timeout);
}

/*
 * Invoke a command with an immediate output parameter (and copy the
 * output into the caller's out_param pointer after the command
 * executes).
 */
static inline int hns_roce_cmd_imm(struct hns_roce_dev *hr_dev,
					    u64 in_param, u64 *out_param,
					    u32 in_modifier, u8 op_modifier,
					    u16 op, unsigned long timeout)
{
	return __hns_roce_cmd(hr_dev, in_param, out_param, 1, in_modifier,
			      op_modifier, op, timeout);
}

struct hns_roce_cmd_mailbox
	*hns_roce_alloc_cmd_mailbox(struct hns_roce_dev *hr_dev);
void hns_roce_free_cmd_mailbox(struct hns_roce_dev *hr_dev,
					 struct hns_roce_cmd_mailbox *mailbox);

#endif /* _HNS_ROCE_CMD_H */
