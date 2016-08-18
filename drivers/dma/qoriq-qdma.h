/*
 * drivers/dma/qoriq-qdma.h
 *
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 *
 * Driver for the Freescale qDMA engine with software command queue mode.
 * Channel virtualization is supported through enqueuing of DMA jobs to,
 * or dequeuing DMA jobs from, different work queues.
 * This module can be found on Freescale LS SoCs.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __DMA_QORIQ_QDMA_H__
#define __DMA_QORIQ_QDMA_H__

#include "virt-dma.h"

#define FSL_QDMA_DMR			0x0
#define FSL_QDMA_DSR			0x4
#define FSL_QDMA_DEIER			0x1e00
#define FSL_QDMA_DEDR			0x1e04
#define FSL_QDMA_DECFDW0R		0x1e10
#define FSL_QDMA_DECFDW1R		0x1e14
#define FSL_QDMA_DECFDW2R		0x1e18
#define FSL_QDMA_DECFDW3R		0x1e1c
#define FSL_QDMA_DECFQIDR		0x1e30
#define FSL_QDMA_DECBR			0x1e34

#define FSL_QDMA_BCQMR(x)		(0xc0 + 0x100 * (x))
#define FSL_QDMA_BCQSR(x)		(0xc4 + 0x100 * (x))
#define FSL_QDMA_BCQEDPA_SADDR(x)	(0xc8 + 0x100 * (x))
#define FSL_QDMA_BCQDPA_SADDR(x)	(0xcc + 0x100 * (x))
#define FSL_QDMA_BCQEEPA_SADDR(x)	(0xd0 + 0x100 * (x))
#define FSL_QDMA_BCQEPA_SADDR(x)	(0xd4 + 0x100 * (x))
#define FSL_QDMA_BCQIER(x)		(0xe0 + 0x100 * (x))
#define FSL_QDMA_BCQIDR(x)		(0xe4 + 0x100 * (x))

#define FSL_QDMA_SQDPAR			0x80c
#define FSL_QDMA_SQEPAR			0x814
#define FSL_QDMA_BSQMR			0x800
#define FSL_QDMA_BSQSR			0x804
#define FSL_QDMA_BSQICR			0x828
#define FSL_QDMA_CQMR			0xa00
#define FSL_QDMA_CQDSCR1		0xa08
#define FSL_QDMA_CQDSCR2                0xa0c
#define FSL_QDMA_CQIER			0xa10
#define FSL_QDMA_CQEDR			0xa14

#define FSL_QDMA_SQICR_ICEN

#define FSL_QDMA_CQIDR_CQT		0xff000000
#define FSL_QDMA_CQIDR_SQPE		0x800000
#define FSL_QDMA_CQIDR_SQT		0x8000

#define FSL_QDMA_BCQIER_CQTIE		0x8000
#define FSL_QDMA_BCQIER_CQPEIE		0x800000
#define FSL_QDMA_BSQICR_ICEN		0x80000000
#define FSL_QDMA_BSQICR_ICST(x)		((x) << 16)
#define FSL_QDMA_CQIER_MEIE		0x80000000
#define FSL_QDMA_CQIER_TEIE		0x1

#define FSL_QDMA_BCQMR_EN		0x80000000
#define FSL_QDMA_BCQMR_EI		0x40000000
#define FSL_QDMA_BCQMR_CD_THLD(x)	((x) << 20)
#define FSL_QDMA_BCQMR_CQ_SIZE(x)	((x) << 16)

#define FSL_QDMA_BCQSR_QF		0x10000

#define FSL_QDMA_BSQMR_EN		0x80000000
#define FSL_QDMA_BSQMR_DI		0x40000000
#define FSL_QDMA_BSQMR_CQ_SIZE(x)	((x) << 16)

#define FSL_QDMA_BSQSR_QE		0x20000

#define FSL_QDMA_DMR_DQD		0x40000000
#define FSL_QDMA_DSR_DB			0x80000000

#define FSL_QDMA_CMD_RWTTYPE		0x4

#define FSL_QDMA_CMD_RWTTYPE_OFFSET	28
#define FSL_QDMA_CMD_NS_OFFSET		27
#define FSL_QDMA_CMD_DQOS_OFFSET	24
#define FSL_QDMA_CMD_WTHROTL_OFFSET	20
#define FSL_QDMA_CMD_DSEN_OFFSET	19
#define FSL_QDMA_CMD_LWC_OFFSET		16

#define FSL_QDMA_E_SG_TABLE		1
#define FSL_QDMA_E_DATA_BUFFER		0

#define FSL_QDMA_MAX_BLOCK		4
#define FSL_QDMA_MAX_QUEUE		8
#define FSL_QDMA_BASE_BUFFER_SIZE	96
#define FSL_QDMA_EXPECT_SG_ENTRY_NUM	16
#define FSL_QDMA_CIRCULAR_SIZE_MIN	64
#define FSL_QDMA_CIRCULAR_SIZE_MAX	16384

/*
 * Descriptor bit shifts and masks.
 */

#define QDMA_CSGF_OFFSET_SHIFT		0
#define QDMA_CSGF_OFFSET_MASK		0x1fff
#define QDMA_CSGF_LENGTH_SHIFT		0
#define QDMA_CSGF_LENGTH_MASK		0x3
#define QDMA_CSGF_F			(1UL << 30)
#define QDMA_CSGF_E			(1UL << 31)
#define QDMA_CSGF_ADDR_LOW_MASK		0xffffffff
#define QDMA_CSGF_ADDR_GIHG_SHIFT	0
#define QDMA_CSGF_ADDR_HIGH_MASK	0xff

#define QDMA_CCDF_STATUS_SHIFT		0
#define QDMA_CCDF_STATUS_MASK		0xff
#define QDMA_CCDF_SER			(1UL << 30)
#define QDMA_CCDF_OFFSET_SHIFT		20
#define QDMA_CCDF_OFFSET_MASK		0x1ff
#define QDMA_CCDF_FORMAT_SHIFT		29
#define QDMA_CCDF_FORMAT_MASK		0x3
#define QDMA_CCDF_ADDR_LOW_MASK		0xffffffff
#define QDMA_CCDF_ADDR_GIHG_SHIFT	0
#define QDMA_CCDF_ADDR_HIGH_MASK	0xff
#define QDMA_CCDF_QUEUE_SHIFT		24
#define QDMA_CCDF_QUEUE_MASK		0x3
#define QDMA_CCDF_DD_SHIFT		30
#define QDMA_CCDF_DD_MASK		0x2

#define QDMA_SDF_SSD_SHIFT		0
#define QDMA_SDF_SSD_MASK		0xfff
#define QDMA_SDF_SSS_SHIFT		12
#define QDMA_SDF_SSS_MASK		0xfff
#define QDMA_SDF_CMD_MASK		0xffffffff

#define QDMA_DDF_DSD_SHIFT		0
#define QDMA_DDF_DSD_MASK		0xfff
#define QDMA_DDF_DSS_SHIFT		12
#define QDMA_DDF_DSS_MASK		0xfff
#define QDMA_DDF_CMD_MASK		0xffffffff

/*
 * enum qdma_queue_type - QDMA queue type
 * @QDMA_QUEUE: work command queue
 * @QDMA_STATUS: work status queue
 */
enum qdma_queue_type {
	QDMA_QUEUE,
	QDMA_STATUS,
};

struct fsl_qdma_ccdf {
	u32 ser_status;
	u32 format_offset;
	u32 addr_low;
	u32 dd_q_addr_high;
};

struct fsl_qdma_csgf {
	u32 offset;
	u32 e_f_length;
	u32 addr_low;
	u32 addr_high;
};

struct fsl_qdma_sdf {
	u32 rev1;
	u32 sss_ssd;
	u32 rev2;
	u32 cmd;
};

struct fsl_qdma_ddf {
	u32 rev1;
	u32 dss_dsd;
	u32 rev2;
	u32 cmd;
};

struct fsl_qdma_chan {
	struct virt_dma_chan		vchan;
	struct virt_dma_desc		vdesc;
	enum dma_status			status;
	u32				slave_id;
	struct fsl_qdma_engine		*qdma;
	struct fsl_qdma_queue		*queue;
	struct list_head		qcomp;
};

struct fsl_qdma_queue {
	struct fsl_qdma_ccdf	*virt_head;
	struct fsl_qdma_ccdf	*virt_tail;
	struct list_head	comp_used;
	struct list_head	comp_free;
	struct dma_pool		*comp_pool;
	struct dma_pool		*sg_pool;
	spinlock_t		queue_lock;
	dma_addr_t		bus_addr;
	u32                     n_cq;
	u32			id;
	struct fsl_qdma_ccdf	*cq;
};

struct fsl_qdma_sg {
	dma_addr_t		bus_addr;
	void			*virt_addr;
};

struct fsl_qdma_comp {
	dma_addr_t              bus_addr;
	void			*virt_addr;
	struct fsl_qdma_chan	*qchan;
	struct fsl_qdma_sg	*sg_block;
	struct virt_dma_desc    vdesc;
	struct list_head	list;
	u32			sg_block_src;
	u32			sg_block_dst;
};

struct fsl_qdma_engine {
	struct dma_device	dma_dev;
	void __iomem		*ctrl_base;
	void __iomem		*block_base;
	u32			n_chans;
	u32			n_queues;
	struct mutex            fsl_qdma_mutex;
	int			error_irq;
	int			queue_irq;
	bool			big_endian;
	struct fsl_qdma_queue	*queue;
	struct fsl_qdma_queue	*status;
	struct fsl_qdma_chan	chans[];

};

struct fsl_qdma_frame {
	struct fsl_qdma_ccdf ccdf;
	struct fsl_qdma_csgf csgf_desc;
	struct fsl_qdma_csgf csgf_src;
	struct fsl_qdma_csgf csgf_dest;
	struct fsl_qdma_sdf sdf;
	struct fsl_qdma_ddf ddf;
};

static u32 qdma_readl(struct fsl_qdma_engine *qdma, u32 __iomem *addr)
{
	if (qdma->big_endian)
		return ioread32be(addr);
	else
		return ioread32(addr);
}

static void qdma_writel(struct fsl_qdma_engine *qdma, u32 val,
						u32 __iomem *addr)
{
	if (qdma->big_endian)
		iowrite32be(val, addr);
	else
		iowrite32(val, addr);
}

static struct fsl_qdma_chan *to_fsl_qdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct fsl_qdma_chan, vchan.chan);
}

static struct fsl_qdma_comp *to_fsl_qdma_comp(struct virt_dma_desc *vd)
{
	return container_of(vd, struct fsl_qdma_comp, vdesc);
}

#endif /* __DMA_QORIQ_QDMA_H__ */
