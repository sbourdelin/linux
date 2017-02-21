/*
 * Synopsys DesignWare AXI DMA Controller driver.
 *
 * Copyright (C) 2017 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _AXI_DMA_PLATFORM_REG_H
#define _AXI_DMA_PLATFORM_REG_H

#include <linux/bitops.h>

#define COMMON_REG_LEN		0x100
#define CHAN_REG_LEN		0x100

/* Common registers offset */
#define DMAC_ID			0x000 /* R DMAC ID */
#define DMAC_COMPVER		0x008 /* R DMAC Component Version */
#define DMAC_CFG		0x010 /* R/W DMAC Configuration */
#define DMAC_CHEN		0x018 /* R/W DMAC Channel Enable */
#define DMAC_CHEN_L		0x018 /* R/W DMAC Channel Enable 00-31 */
#define DMAC_CHEN_H		0x01C /* R/W DMAC Channel Enable 32-63 */
#define DMAC_INTSTATUS		0x030 /* R DMAC Interrupt Status */
#define DMAC_COMMON_INTCLEAR	0x038 /* W DMAC Interrupt Clear */
#define DMAC_COMMON_INTSTATUS_ENA 0x040 /* R DMAC Interrupt Status Enable */
#define DMAC_COMMON_INTSIGNAL_ENA 0x048 /* R/W DMAC Interrupt Signal Enable */
#define DMAC_COMMON_INTSTATUS	0x050 /* R DMAC Interrupt Status */
#define DMAC_RESET		0x058 /* R DMAC Reset Register1 */

/* DMA channel registers offset */
#define CH_SAR			0x000 /* R/W Chan Source Address */
#define CH_DAR			0x008 /* R/W Chan Destination Address */
#define CH_BLOCK_TS		0x010 /* R/W Chan Block Transfer Size */
#define CH_CTL			0x018 /* R/W Chan Control */
#define CH_CTL_L		0x018 /* R/W Chan Control 00-31 */
#define CH_CTL_H		0x01C /* R/W Chan Control 32-63 */
#define CH_CFG			0x020 /* R/W Chan Configuration */
#define CH_CFG_L		0x020 /* R/W Chan Configuration 00-31 */
#define CH_CFG_H		0x024 /* R/W Chan Configuration 32-63 */
#define CH_LLP			0x028 /* R/W Chan Linked List Pointer */
#define CH_STATUS		0x030 /* R Chan Status */
#define CH_SWHSSRC		0x038 /* R/W Chan SW Handshake Source */
#define CH_SWHSDST		0x040 /* R/W Chan SW Handshake Destination */
#define CH_BLK_TFR_RESUMEREQ	0x048 /* W Chan Block Transfer Resume Req */
#define CH_AXI_ID		0x050 /* R/W Chan AXI ID */
#define CH_AXI_QOS		0x058 /* R/W Chan AXI QOS */
#define CH_SSTAT		0x060 /* R Chan Source Status */
#define CH_DSTAT		0x068 /* R Chan Destination Status */
#define CH_SSTATAR		0x070 /* R/W Chan Source Status Fetch Addr */
#define CH_DSTATAR		0x078 /* R/W Chan Destination Status Fetch Addr */
#define CH_INTSTATUS_ENA	0x080 /* R/W Chan Interrupt Status Enable */
#define CH_INTSTATUS		0x088 /* R/W Chan Interrupt Status */
#define CH_INTSIGNAL_ENA	0x090 /* R/W Chan Interrupt Signal Enable */
#define CH_INTCLEAR		0x098 /* W Chan Interrupt Clear */


/* DMAC_CFG */
#define DMAC_EN_MASK		0x00000001U
#define DMAC_EN_POS		0

#define INT_EN_MASK		0x00000002U
#define INT_EN_POS		1

#define DMAC_CHAN_EN_SHIFT	0
#define DMAC_CHAN_EN_WE_SHIFT	8

#define DMAC_CHAN_SUSP_SHIFT	16
#define DMAC_CHAN_SUSP_WE_SHIFT	24

/* CH_CTL_H */
#define CH_CTL_H_LLI_LAST	BIT(30)
#define CH_CTL_H_LLI_VALID	BIT(31)

/* CH_CTL_L */
#define CH_CTL_L_LAST_WRITE_EN	BIT(30)

#define CH_CTL_L_DST_MSIZE_POS	18
#define CH_CTL_L_SRC_MSIZE_POS	14
enum {
	DWAXIDMAC_BURST_TRANS_LEN_1	= 0x0,
	DWAXIDMAC_BURST_TRANS_LEN_4,
	DWAXIDMAC_BURST_TRANS_LEN_8,
	DWAXIDMAC_BURST_TRANS_LEN_16,
	DWAXIDMAC_BURST_TRANS_LEN_32,
	DWAXIDMAC_BURST_TRANS_LEN_64,
	DWAXIDMAC_BURST_TRANS_LEN_128,
	DWAXIDMAC_BURST_TRANS_LEN_256,
	DWAXIDMAC_BURST_TRANS_LEN_512,
	DWAXIDMAC_BURST_TRANS_LEN_1024
};

#define CH_CTL_L_DST_WIDTH_POS	11
#define CH_CTL_L_SRC_WIDTH_POS	8

#define CH_CTL_L_DST_INC_POS	6
#define CH_CTL_L_SRC_INC_POS	4
enum {
	DWAXIDMAC_CH_CTL_L_INC	= 0x0,
	DWAXIDMAC_CH_CTL_L_NOINC
};

#define CH_CTL_L_DST_MAST_POS	2
#define CH_CTL_L_DST_MAST	BIT(CH_CTL_L_DST_MAST_POS)
#define CH_CTL_L_SRC_MAST_POS	0
#define CH_CTL_L_SRC_MAST	BIT(CH_CTL_L_SRC_MAST_POS)

/* CH_CFG_H */
#define CH_CFG_H_PRIORITY_POS	17
#define CH_CFG_H_HS_SEL_DST_POS	4
#define CH_CFG_H_HS_SEL_SRC_POS	3
enum {
	DWAXIDMAC_HS_SEL_HW	= 0x0,
	DWAXIDMAC_HS_SEL_SW
};

#define CH_CFG_H_TT_FC_POS	0
enum {
	DWAXIDMAC_TT_FC_MEM_TO_MEM_DMAC	= 0x0,
	DWAXIDMAC_TT_FC_MEM_TO_PER_DMAC,
	DWAXIDMAC_TT_FC_PER_TO_MEM_DMAC,
	DWAXIDMAC_TT_FC_PER_TO_PER_DMAC,
	DWAXIDMAC_TT_FC_PER_TO_MEM_SRC,
	DWAXIDMAC_TT_FC_PER_TO_PER_SRC,
	DWAXIDMAC_TT_FC_MEM_TO_PER_DST,
	DWAXIDMAC_TT_FC_PER_TO_PER_DST
};

/* CH_CFG_L */
#define CH_CFG_L_DST_MULTBLK_TYPE_POS	2
#define CH_CFG_L_SRC_MULTBLK_TYPE_POS	0
enum {
	DWAXIDMAC_MBLK_TYPE_CONTIGUOUS	= 0x0,
	DWAXIDMAC_MBLK_TYPE_RELOAD,
	DWAXIDMAC_MBLK_TYPE_SHADOW_REG,
	DWAXIDMAC_MBLK_TYPE_LL
};

enum {
	DWAXIDMAC_IRQ_NONE		= 0x0,
	DWAXIDMAC_IRQ_BLOCK_TRF		= BIT(0),  /* block transfer complete */
	DWAXIDMAC_IRQ_DMA_TRF		= BIT(1),  /* dma transfer complete */
	DWAXIDMAC_IRQ_SRC_TRAN		= BIT(3),  /* source transaction complete */
	DWAXIDMAC_IRQ_DST_TRAN		= BIT(4),  /* destination transaction complete */
	DWAXIDMAC_IRQ_SRC_DEC_ERR	= BIT(5),  /* source decode error */
	DWAXIDMAC_IRQ_DST_DEC_ERR	= BIT(6),  /* destination decode error */
	DWAXIDMAC_IRQ_SRC_SLV_ERR	= BIT(7),  /* source slave error */
	DWAXIDMAC_IRQ_DST_SLV_ERR	= BIT(8),  /* destination slave error */
	DWAXIDMAC_IRQ_LLI_RD_DEC_ERR	= BIT(9),  /* LLI read decode error */
	DWAXIDMAC_IRQ_LLI_WR_DEC_ERR	= BIT(10), /* LLI write decode error */
	DWAXIDMAC_IRQ_LLI_RD_SLV_ERR	= BIT(11), /* LLI read slave error */
	DWAXIDMAC_IRQ_LLI_WR_SLV_ERR	= BIT(12), /* LLI write slave error */
	DWAXIDMAC_IRQ_INVALID_ERR	= BIT(13), /* LLI invalide error or Shadow register error */
	DWAXIDMAC_IRQ_MULTIBLKTYPE_ERR	= BIT(14), /* Slave Interface Multiblock type error */
	DWAXIDMAC_IRQ_DEC_ERR		= BIT(16), /* Slave Interface decode error */
	DWAXIDMAC_IRQ_WR2RO_ERR		= BIT(17), /* Slave Interface write to read only error */
	DWAXIDMAC_IRQ_RD2RWO_ERR	= BIT(18), /* Slave Interface read to write only error */
	DWAXIDMAC_IRQ_WRONCHEN_ERR	= BIT(19), /* Slave Interface write to channel error */
	DWAXIDMAC_IRQ_SHADOWREG_ERR	= BIT(20), /* Slave Interface shadow reg error */
	DWAXIDMAC_IRQ_WRONHOLD_ERR	= BIT(21), /* Slave Interface hold error */
	DWAXIDMAC_IRQ_LOCK_CLEARED	= BIT(27), /* Lock Cleared Status */
	DWAXIDMAC_IRQ_SRC_SUSPENDED	= BIT(28), /* Source Suspended Status */
	DWAXIDMAC_IRQ_SUSPENDED		= BIT(29), /* Channel Suspended Status */
	DWAXIDMAC_IRQ_DISABLED		= BIT(30), /* Channel Disabled Status */
	DWAXIDMAC_IRQ_ABORTED		= BIT(31), /* Channel Aborted Status */
	DWAXIDMAC_IRQ_ALL_ERR		= 0x003F7FE0,
	DWAXIDMAC_IRQ_ALL		= 0xFFFFFFFF
};

enum {
	DWAXIDMAC_TRANS_WIDTH_8		= 0x0,
	DWAXIDMAC_TRANS_WIDTH_16,
	DWAXIDMAC_TRANS_WIDTH_32,
	DWAXIDMAC_TRANS_WIDTH_64,
	DWAXIDMAC_TRANS_WIDTH_128,
	DWAXIDMAC_TRANS_WIDTH_256,
	DWAXIDMAC_TRANS_WIDTH_512,
	DWAXIDMAC_TRANS_WIDTH_MAX	= DWAXIDMAC_TRANS_WIDTH_512
};

#endif /* _AXI_DMA_PLATFORM_H */
