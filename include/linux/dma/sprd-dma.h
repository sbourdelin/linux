/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SPRD_DMA_H
#define __SPRD_DMA_H
#include <linux/dmaengine.h>

/* DMA request ID definition */
#define DMA_SOFTWARE_UID	0
#define DMA_SIM_RX		1
#define DMA_SIM_TX		2
#define DMA_IIS0_RX		3
#define DMA_IIS0_TX		4
#define DMA_IIS1_RX		5
#define DMA_IIS1_TX		6
#define DMA_IIS2_RX		7
#define DMA_IIS2_TX		8
#define DMA_IIS3_RX		9
#define DMA_IIS3_TX		10
#define DMA_SPI0_RX		11
#define DMA_SPI0_TX		12
#define DMA_SPI1_RX		13
#define DMA_SPI1_TX		14
#define DMA_SPI2_RX		15
#define DMA_SPI2_TX		16
#define DMA_UART0_RX		17
#define DMA_UART0_TX		18
#define DMA_UART1_RX		19
#define DMA_UART1_TX		20
#define DMA_UART2_RX		21
#define DMA_UART2_TX		22
#define DMA_UART3_RX		23
#define DMA_UART3_TX		24
#define DMA_UART4_RX		25
#define DMA_UART4_TX		26
#define DMA_DRM_CPT		27
#define DMA_DRM_RAW		28
#define DMA_VB_DA0		29
#define DMA_VB_DA1		30
#define DMA_VB_AD0		31
#define DMA_VB_AD1		32
#define DMA_VB_AD2		33
#define DMA_VB_AD3		34
#define DMA_GPS			35
#define DMA_SDIO0_RD		36
#define DMA_SDIO0_WR		37
#define DMA_SDIO1_RD		38
#define DMA_SDIO1_WR		39
#define DMA_SDIO2_RD		40
#define DMA_SDIO2_WR		41
#define DMA_EMMC_RD		42
#define DMA_EMMC_WR		43

/*
 * enum dma_datawidth: define the DMA transfer data width
 * @BYTE_WIDTH: 1 byte width
 * @SHORT_WIDTH: 2 bytes width
 * @WORD_WIDTH: 4 bytes width
 * @DWORD_WIDTH: 8 bytes width
 */
enum dma_datawidth {
	BYTE_WIDTH,
	SHORT_WIDTH,
	WORD_WIDTH,
	DWORD_WIDTH,
};

/*
 * enum dma_request_mode: define the DMA request mode
 * @FRAG_REQ_MODE: fragment request mode
 * @BLOCK_REQ_MODE: block request mode
 * @TRANS_REQ_MODE: transaction request mode
 * @LIST_REQ_MODE: link-list request mode
 *
 * We have 4 types request mode: fragment mode, block mode, transaction mode
 * and linklist mode. One transaction can contain several blocks, one block can
 * contain several fragments. Link-list mode means we can save several DMA
 * configuration into one reserved memory, then DMA can fetch each DMA
 * configuration automatically to start transfer.
 */
enum dma_request_mode {
	FRAG_REQ_MODE,
	BLOCK_REQ_MODE,
	TRANS_REQ_MODE,
	LIST_REQ_MODE,
};

/*
 * enum dma_int_type: define the DMA interrupt type
 * @NO_INT: do not need generate DMA interrupt.
 * @FRAG_DONE: fragment done interrupt when one fragment request is done.
 * @BLK_DONE: block done interrupt when one block request is done.
 * @TRANS_DONE: tansaction done interrupt when one transaction request is done.
 * @LIST_DONE: link-list done interrupt when one link-list request is done.
 * @CONFIG_ERR: configure error interrupt when configuration is incorrect
 * @BLOCK_FRAG_DONE: block and fragment interrupt when one fragment or block
 * request is done.
 * @TRANS_FRAG_DONE: transaction and fragment interrupt when one transaction
 * request or fragment request is done.
 * @TRANS_BLOCK_DONE: transaction and block interrupt when one transaction
 * request or block request is done.
 */
enum dma_int_type {
	NO_INT,
	FRAG_DONE,
	BLK_DONE,
	TRANS_DONE,
	LIST_DONE,
	CONFIG_ERR,
	BLOCK_FRAG_DONE,
	TRANS_FRAG_DONE,
	TRANS_BLOCK_DONE,
};

/*
 * enum dma_pri_level: define the DMA channel priority level
 * @DMA_PRI_0: level 0
 * @DMA_PRI_1: level 1
 * @DMA_PRI_2: level 2
 * @DMA_PRI_3: level 3
 *
 * When there are several DMA channels need to start, the DMA controller's
 * arbitration will choose the high priority channel to start firstly.
 */
enum dma_pri_level {
	DMA_PRI_0,
	DMA_PRI_1,
	DMA_PRI_2,
	DMA_PRI_3,
};

/*
 * enum dma_switch_mode: define the DMA transfer format
 * @DATA_ABCD: ABCD to ABCD
 * @DATA_DCBA: ABCD to DCBA
 * @DATA_BADC: ABCD to BADC
 * @DATA_CDAB: ABCD to CDAB
 */
enum dma_switch_mode {
	DATA_ABCD,
	DATA_DCBA,
	DATA_BADC,
	DATA_CDAB,
};

/*
 * enum dma_end_type: define the DMA configuration end type
 * @DMA_NOT_END: DMA configuration is not end
 * @DMA_END: DMA configuration is end but not one link-list cycle configuration
 * @DMA_LINK: DMA configuration is end but it is one link-list cycle
 * configuration
 *
 * Since DMA contrller can support link-list transfer mode, that means user can
 * supply several DMA configuration and each configuration can be pointed by
 * previous link pointer register, then DMA controller will start to transfer
 * for each DMA configuration automatically. DMA_END and DMA_LINK flag can
 * indicate these several configuration is end, but DMA_LINK can also indicate
 * these listed configuration is one cycle. For example if we have 4 group DMA
 * configuration and we set DMA_LINK, which means it will start to transfer from
 * cfg0--->cfg1--->cfg2--->cfg3, then back to cfg0 as one cycle.
 */
enum dma_end_type {
	DMA_NOT_END,
	DMA_END,
	DMA_LINK,
};

/*
 * enum dma_flags: define the DMA flags
 * @DMA_HARDWARE_REQ: hardware request channel to start transfer by hardware id.
 * @DMA_SOFTWARE_REQ: software request channel to start transfer.
 * @DMA_GROUP1_SRC: indicate this is source channel of group 1 which can be
 * start another channel.
 * @DMA_GROUP1_DST: indicate this is destination channel of group 1 which will
 * be started by source channel.
 * @DMA_GROUP2_SRC: indicate this is source channel of group 2 which can be
 * start another channel.
 * @DMA_GROUP2_DST: indicate this is destination channel of group 2 which will
 * be started by source channel.
 * @DMA_MUTL_FRAG_DONE: when fragment is done of source channel which will
 * start another destination channel.
 * @DMA_MUTL_BLK_DONE: when block is done of source channel which will start
 * another destination channel.
 * @DMA_MUTL_TRANS_DONE: when transaction is done of source channel which will
 * start another destination channel.
 * @DMA_MUTL_LIST_DONE: when link-list is done of source channel which will
 * start another destination channel.
 *
 * Since DMA controller can support 2 stage transfer which means when one
 * channel transfer is done, then it can start another channel's transfer
 * automatically by interrupt type.
 */
enum dma_flags {
	DMA_HARDWARE_REQ = BIT(0),
	DMA_SOFTWARE_REQ = BIT(1),
	DMA_GROUP1_SRC = BIT(2),
	DMA_GROUP1_DST = BIT(3),
	DMA_GROUP2_SRC = BIT(4),
	DMA_GROUP2_DST = BIT(5),
	DMA_MUTL_FRAG_DONE = BIT(6),
	DMA_MUTL_BLK_DONE = BIT(7),
	DMA_MUTL_TRANS_DONE = BIT(8),
	DMA_MUTL_LIST_DONE = BIT(9),
};

/*
 * struct sprd_dma_cfg: DMA configuration for users
 * @config: salve config structure
 * @chn_pri: the channel priority
 * @datawidth: the data width
 * @req_mode: the request mode
 * @irq_mode: the interrupt mode
 * @swt_mode: the switch mode
 * @link_cfg_v: point to the virtual memory address to save link-list DMA
 * configuration
 * @link_cfg_p: point to the physical memory address to save link-list DMA
 * configuration
 * @src_addr: the source address
 * @des_addr: the destination address
 * @fragmens_len: one fragment request length
 * @block_len; one block request length
 * @transcation_len: one transaction request length
 * @src_step: source side transfer step
 * @des_step: destination side transfer step
 * @src_frag_step: source fragment transfer step
 * @dst_frag_step: destination fragment transfer step
 * @src_blk_step: source block transfer step
 * @dst_blk_step: destination block transfer step
 * @wrap_ptr: wrap jump pointer address
 * @wrap_to: wrap jump to address
 * @dev_id: hardware device id to start DMA transfer
 * @is_end: DMA configuration end type
 */
struct sprd_dma_cfg {
	struct dma_slave_config config;
	enum dma_pri_level chn_pri;
	enum dma_datawidth datawidth;
	enum dma_request_mode req_mode;
	enum dma_int_type irq_mode;
	enum dma_switch_mode swt_mode;
	unsigned long link_cfg_v;
	unsigned long link_cfg_p;
	unsigned long src_addr;
	unsigned long des_addr;
	u32 fragmens_len;
	u32 block_len;
	u32 transcation_len;
	u32 src_step;
	u32 des_step;
	u32 src_frag_step;
	u32 dst_frag_step;
	u32 src_blk_step;
	u32 dst_blk_step;
	u32 wrap_ptr;
	u32 wrap_to;
	u32 dev_id;
	enum dma_end_type is_end;
};

#endif
