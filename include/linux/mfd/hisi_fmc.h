/*
 * Header file for HiSilicon Flash Memory Controller Driver
 *
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __HISI_FMC_H
#define __HISI_FMC_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/mutex.h>

/* Hardware register offsets and field definitions */
#define FMC_CFG				0x00
#define FMC_CFG_OP_MODE_MASK		BIT_MASK(0)
#define FMC_CFG_OP_MODE_BOOT		0
#define FMC_CFG_OP_MODE_NORMAL		1
#define FMC_CFG_FLASH_SEL(type)		(((type) & 0x3) << 1)
#define FMC_CFG_FLASH_SEL_MASK		0x6
#define FMC_ECC_TYPE(type)		(((type) & 0x7) << 5)
#define FMC_ECC_TYPE_MASK		GENMASK(7, 5)
#define SPI_NOR_ADDR_MODE_MASK		BIT_MASK(10)
#define SPI_NOR_ADDR_MODE_3BYTES	(0x0 << 10)
#define SPI_NOR_ADDR_MODE_4BYTES	(0x1 << 10)
#define FMC_GLOBAL_CFG			0x04
#define FMC_GLOBAL_CFG_WP_ENABLE	BIT(6)
#define FMC_SPI_TIMING_CFG		0x08
#define TIMING_CFG_TCSH(nr)		(((nr) & 0xf) << 8)
#define TIMING_CFG_TCSS(nr)		(((nr) & 0xf) << 4)
#define TIMING_CFG_TSHSL(nr)		((nr) & 0xf)
#define CS_HOLD_TIME			0x6
#define CS_SETUP_TIME			0x6
#define CS_DESELECT_TIME		0xf
#define FMC_INT				0x18
#define FMC_INT_OP_DONE			BIT(0)
#define FMC_INT_CLR			0x20
#define FMC_CMD				0x24
#define FMC_CMD_CMD1(cmd)		((cmd) & 0xff)
#define FMC_ADDRL			0x2c
#define FMC_OP_CFG			0x30
#define OP_CFG_FM_CS(cs)		((cs) << 11)
#define OP_CFG_MEM_IF_TYPE(type)	(((type) & 0x7) << 7)
#define OP_CFG_ADDR_NUM(addr)		(((addr) & 0x7) << 4)
#define OP_CFG_DUMMY_NUM(dummy)		((dummy) & 0xf)
#define FMC_DATA_NUM			0x38
#define FMC_DATA_NUM_CNT(cnt)		((cnt) & GENMASK(13, 0))
#define FMC_OP				0x3c
#define FMC_OP_DUMMY_EN			BIT(8)
#define FMC_OP_CMD1_EN			BIT(7)
#define FMC_OP_ADDR_EN			BIT(6)
#define FMC_OP_WRITE_DATA_EN		BIT(5)
#define FMC_OP_READ_DATA_EN		BIT(2)
#define FMC_OP_READ_STATUS_EN		BIT(1)
#define FMC_OP_REG_OP_START		BIT(0)
#define FMC_DMA_LEN			0x40
#define FMC_DMA_LEN_SET(len)		((len) & GENMASK(27, 0))
#define FMC_DMA_SADDR_D0		0x4c
#define HIFMC_DMA_MAX_LEN		(4096)
#define HIFMC_DMA_MASK			(HIFMC_DMA_MAX_LEN - 1)
#define FMC_OP_DMA			0x68
#define OP_CTRL_RD_OPCODE(code)		(((code) & 0xff) << 16)
#define OP_CTRL_WR_OPCODE(code)		(((code) & 0xff) << 8)
#define OP_CTRL_RW_OP(op)		((op) << 1)
#define OP_CTRL_DMA_OP_READY		BIT(0)
#define FMC_OP_READ			0x0
#define FMC_OP_WRITE			0x1
#define FMC_WAIT_TIMEOUT		1000000

#define HIFMC_MAX_CHIP_NUM		2

enum hifmc_iftype {
	IF_TYPE_STD,
	IF_TYPE_DUAL,
	IF_TYPE_DIO,
	IF_TYPE_QUAD,
	IF_TYPE_QIO,
};

struct hisi_fmc {
	void __iomem *regbase;
	void __iomem *iobase;
	struct clk *clk;
	struct mutex lock;
};

#endif
