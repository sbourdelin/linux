/*
 * st_fdma.h
 *
 * Copyright (C) 2014 STMicroelectronics
 * Author: Ludovic Barre <Ludovic.barre@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */
#ifndef __DMA_ST_FDMA_H
#define __DMA_ST_FDMA_H

#include <linux/dmaengine.h>
#include <linux/interrupt.h>

#include "virt-dma.h"

#define ST_FDMA_NR_DREQS 32
#define EM_SLIM	102	/* No official SLIM ELF ID */
#define FW_NAME_SIZE 30

enum {
	CLK_SLIM,
	CLK_HI,
	CLK_LOW,
	CLK_IC,
	CLK_MAX_NUM,
};

#define NAME_SZ 10

struct st_fdma_ram {
	char name[NAME_SZ];
	u32 offset;
	u32 size;
};

/**
 * struct st_fdma_generic_node - Free running/paced generic node
 *
 * @length: Length in bytes of a line in a 2D mem to mem
 * @sstride: Stride, in bytes, between source lines in a 2D data move
 * @dstride: Stride, in bytes, between destination lines in a 2D data move
 */
struct st_fdma_generic_node {
	u32 length;
	u32 sstride;
	u32 dstride;
};

/**
 * struct st_fdma_hw_node - Node structure used by fdma hw
 *
 * @next: Pointer to next node
 * @control: Transfer Control Parameters
 * @nbytes: Number of Bytes to read
 * @saddr: Source address
 * @daddr: Destination address
 *
 * @generic: generic node for free running/paced transfert type
 * 2 others transfert type are possible, but not yet implemented
 *
 * The NODE structures must be aligned to a 32 byte boundary
 */
struct st_fdma_hw_node {
	u32 next;
	u32 control;
	u32 nbytes;
	u32 saddr;
	u32 daddr;
	union {
		struct st_fdma_generic_node generic;
	};
} __aligned(32);

/*
 * node control parameters
 */
#define NODE_CTRL_REQ_MAP_MASK		GENMASK(4, 0)
#define NODE_CTRL_REQ_MAP_FREE_RUN	0x0
#define NODE_CTRL_REQ_MAP_DREQ(n)	((n) & NODE_CTRL_REQ_MAP_MASK)
#define NODE_CTRL_REQ_MAP_EXT		NODE_CTRL_REQ_MAP_MASK
#define NODE_CTRL_SRC_MASK		GENMASK(6, 5)
#define NODE_CTRL_SRC_STATIC		BIT(5)
#define NODE_CTRL_SRC_INCR		BIT(6)
#define NODE_CTRL_DST_MASK		GENMASK(8, 7)
#define NODE_CTRL_DST_STATIC		BIT(7)
#define NODE_CTRL_DST_INCR		BIT(8)
#define NODE_CTRL_SECURE		BIT(15)
#define NODE_CTRL_PAUSE_EON		BIT(30)
#define NODE_CTRL_INT_EON		BIT(31)

/**
 * struct st_fdma_sw_node - descriptor structure for link list
 *
 * @pdesc: Physical address of desc
 * @node: link used for putting this into a channel queue
 */
struct st_fdma_sw_node {
	dma_addr_t pdesc;
	struct st_fdma_hw_node *desc;
};

struct st_fdma_driverdata {
	const struct st_fdma_ram *fdma_mem;
	u32 num_mem;
	u32 id;
	char name[NAME_SZ];
};

struct st_fdma_desc {
	struct virt_dma_desc vdesc;
	struct st_fdma_chan *fchan;
	bool iscyclic;
	unsigned int n_nodes;
	struct st_fdma_sw_node node[];
};

enum st_fdma_type {
	ST_FDMA_TYPE_FREE_RUN,
	ST_FDMA_TYPE_PACED,
};

struct st_fdma_cfg {
	struct device_node *of_node;
	enum st_fdma_type type;
	dma_addr_t dev_addr;
	enum dma_transfer_direction dir;
	int req_line; /* request line */
	long req_ctrl; /* Request control */
};

struct st_fdma_chan {
	struct st_fdma_dev *fdev;
	struct dma_pool *node_pool;
	struct dma_slave_config scfg;
	struct st_fdma_cfg cfg;

	int dreq_line;

	struct virt_dma_chan vchan;
	struct st_fdma_desc *fdesc;
	enum dma_status	status;
};

struct st_fdma_dev {
	struct device *dev;
	const struct st_fdma_driverdata *drvdata;
	struct dma_device dma_device;

	void __iomem *io_base;
	struct resource *io_res;
	struct clk *clks[CLK_MAX_NUM];

	struct st_fdma_chan *chans;

	spinlock_t dreq_lock;
	unsigned long dreq_mask;

	u32 nr_channels;
	char fw_name[FW_NAME_SIZE];

	atomic_t fw_loaded;
};

/* Registers*/
/* FDMA interface */
#define FDMA_ID_OFST		0x00000
#define FDMA_VER_OFST		0x00004

#define FDMA_EN_OFST		0x00008
#define FDMA_EN_RUN			BIT(0)

#define FDMA_CLK_GATE_OFST	0x0000C
#define FDMA_CLK_GATE_DIS		BIT(0)
#define FDMA_CLK_GATE_RESET		BIT(2)

#define FDMA_SLIM_PC_OFST	0x00020

#define FDMA_REV_ID_OFST	0x10000
#define FDMA_REV_ID_MIN_MASK		GENMASK(15, 8)
#define FDMA_REV_ID_MIN(id)		((id & FDMA_REV_ID_MIN_MASK) >> 8)
#define FDMA_REV_ID_MAJ_MASK		GENMASK(23, 16)
#define FDMA_REV_ID_MAJ(id)		((id & FDMA_REV_ID_MAJ_MASK) >> 16)

#define FDMA_STBUS_SYNC_OFST	0x17F88
#define FDMA_STBUS_SYNC_DIS		BIT(0)

#define FDMA_CMD_STA_OFST	0x17FC0
#define FDMA_CMD_SET_OFST	0x17FC4
#define FDMA_CMD_CLR_OFST	0x17FC8
#define FDMA_CMD_MASK_OFST	0x17FCC
#define FDMA_CMD_START(ch)		(0x1 << (ch << 1))
#define FDMA_CMD_PAUSE(ch)		(0x2 << (ch << 1))
#define FDMA_CMD_FLUSH(ch)		(0x3 << (ch << 1))

#define FDMA_INT_STA_OFST	0x17FD0
#define FDMA_INT_STA_CH			0x1
#define FDMA_INT_STA_ERR		0x2

#define FDMA_INT_SET_OFST	0x17FD4
#define FDMA_INT_CLR_OFST	0x17FD8
#define FDMA_INT_MASK_OFST	0x17FDC

#define fdma_read(fdev, name) \
	readl_relaxed((fdev)->io_base + FDMA_##name##_OFST)

#define fdma_write(fdev, val, name) \
	writel_relaxed((val), (fdev)->io_base + FDMA_##name##_OFST)

/* fchan interface */
#define FDMA_CH_CMD_OFST	0x10200
#define FDMA_CH_CMD_STA_MASK		GENMASK(1, 0)
#define FDMA_CH_CMD_STA_IDLE		(0x0)
#define FDMA_CH_CMD_STA_START		(0x1)
#define FDMA_CH_CMD_STA_RUNNING		(0x2)
#define FDMA_CH_CMD_STA_PAUSED		(0x3)
#define FDMA_CH_CMD_ERR_MASK		GENMASK(4, 2)
#define FDMA_CH_CMD_ERR_INT		(0x0 << 2)
#define FDMA_CH_CMD_ERR_NAND		(0x1 << 2)
#define FDMA_CH_CMD_ERR_MCHI		(0x2 << 2)
#define FDMA_CH_CMD_DATA_MASK		GENMASK(31, 5)
#define fchan_read(fchan, name) \
	readl_relaxed((fchan)->fdev->io_base \
			+ (fchan)->vchan.chan.chan_id * 0x4 \
			+ FDMA_##name##_OFST)

#define fchan_write(fchan, val, name) \
	writel_relaxed((val), (fchan)->fdev->io_base \
			+ (fchan)->vchan.chan.chan_id * 0x4 \
			+ FDMA_##name##_OFST)

/* req interface */
#define FDMA_REQ_CTRL_OFST	0x10240
#define dreq_write(fchan, val, name) \
	writel_relaxed((val), (fchan)->fdev->io_base \
			+ fchan->dreq_line * 0x04 \
			+ FDMA_##name##_OFST)
/* node interface */
#define FDMA_NODE_SZ 128
#define FDMA_PTRN_OFST		0x10800
#define FDMA_CNTN_OFST		0x10808
#define FDMA_SADDRN_OFST	0x1080c
#define FDMA_DADDRN_OFST	0x10810
#define fnode_read(fchan, name) \
	readl_relaxed((fchan)->fdev->io_base \
			+ (fchan)->vchan.chan.chan_id * FDMA_NODE_SZ \
			+ FDMA_##name##_OFST)

#define fnode_write(fchan, val, name) \
	writel_relaxed((val), (fchan)->fdev->io_base \
			+ (fchan)->vchan.chan.chan_id * FDMA_NODE_SZ \
			+ FDMA_##name##_OFST)

/*
 * request control bits
 */
#define REQ_CTRL_NUM_OPS_MASK		GENMASK(31, 24)
#define REQ_CTRL_NUM_OPS(n)		(REQ_CTRL_NUM_OPS_MASK & ((n) << 24))
#define REQ_CTRL_INITIATOR_MASK		BIT(22)
#define	REQ_CTRL_INIT0			(0x0 << 22)
#define	REQ_CTRL_INIT1			(0x1 << 22)
#define REQ_CTRL_INC_ADDR_ON		BIT(21)
#define REQ_CTRL_DATA_SWAP_ON		BIT(17)
#define REQ_CTRL_WNR			BIT(14)
#define REQ_CTRL_OPCODE_MASK		GENMASK(7, 4)
#define REQ_CTRL_OPCODE_LD_ST1		(0x0 << 4)
#define REQ_CTRL_OPCODE_LD_ST2		(0x1 << 4)
#define REQ_CTRL_OPCODE_LD_ST4		(0x2 << 4)
#define REQ_CTRL_OPCODE_LD_ST8		(0x3 << 4)
#define REQ_CTRL_OPCODE_LD_ST16		(0x4 << 4)
#define REQ_CTRL_OPCODE_LD_ST32		(0x5 << 4)
#define REQ_CTRL_OPCODE_LD_ST64		(0x6 << 4)
#define REQ_CTRL_HOLDOFF_MASK		GENMASK(2, 0)
#define REQ_CTRL_HOLDOFF(n)		((n) & REQ_CTRL_HOLDOFF_MASK)

/* bits used by client to configure request control */
#define REQ_CTRL_CFG_MASK (REQ_CTRL_HOLDOFF_MASK | REQ_CTRL_DATA_SWAP_ON \
			| REQ_CTRL_INC_ADDR_ON | REQ_CTRL_INITIATOR_MASK)

bool st_fdma_filter_fn(struct dma_chan *chan, void *param);

#endif	/* __DMA_ST_FDMA_H */
