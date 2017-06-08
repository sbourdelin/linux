/*
 * EDAC defs for Marvell SoCs
 *
 * Copyright (C) 2017 Allied Telesis Labs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */
#ifndef _MVEBU_EDAC_H_
#define _MVEBU_EDAC_H_

#define MVEBU_REVISION " Ver: 2.0.0"
#define EDAC_MOD_STR	"MVEBU_edac"

/*
 * L2 Err Registers
 */
#define MVEBU_L2_ERR_COUNT		0x00	/* 0x8600 */
#define MVEBU_L2_ERR_THRESH		0x04	/* 0x8604 */
#define MVEBU_L2_ERR_ATTR		0x08	/* 0x8608 */
#define MVEBU_L2_ERR_ADDR		0x0c	/* 0x860c */
#define MVEBU_L2_ERR_CAP		0x10	/* 0x8610 */
#define MVEBU_L2_ERR_INJ_CTRL		0x14	/* 0x8614 */
#define MVEBU_L2_ERR_INJ_MASK		0x18	/* 0x8618 */

#define L2_ERR_UE_THRESH(val)		((val & 0xff) << 16)
#define L2_ERR_CE_THRESH(val)		(val & 0xffff)
#define L2_ERR_TYPE(val)		((val >> 8) & 0x3)

/*
 * SDRAM Controller Registers
 */
#define MVEBU_SDRAM_CONFIG		0x00	/* 0x1400 */
#define MVEBU_SDRAM_ERR_DATA_HI		0x40	/* 0x1440 */
#define MVEBU_SDRAM_ERR_DATA_LO		0x44	/* 0x1444 */
#define MVEBU_SDRAM_ERR_ECC_RCVD	0x48	/* 0x1448 */
#define MVEBU_SDRAM_ERR_ECC_CALC	0x4c	/* 0x144c */
#define MVEBU_SDRAM_ERR_ADDR		0x50	/* 0x1450 */
#define MVEBU_SDRAM_ERR_ECC_CNTL	0x54	/* 0x1454 */
#define MVEBU_SDRAM_ERR_ECC_ERR_CNT	0x58	/* 0x1458 */

#define MVEBU_SDRAM_REGISTERED	0x20000
#define MVEBU_SDRAM_ECC		0x40000

struct mvebu_l2_pdata {
	void __iomem *l2_vbase;
	char *name;
	int irq;
	int edac_idx;
};

struct mvebu_mc_pdata {
	void __iomem *mc_vbase;
	int total_mem;
	char *name;
	int irq;
	int edac_idx;
};

#endif
