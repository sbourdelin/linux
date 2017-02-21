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

#ifndef _AXI_DMA_PLATFORM_H
#define _AXI_DMA_PLATFORM_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dmaengine.h>

#include "virt-dma.h"

#define DMAC_MAX_CHANNELS	8
#define DMAC_MAX_MASTERS	2
#define DMAC_MAX_BLK_SIZE	0x200000

struct dw_axi_dma_hcfg {
	u32	nr_channels;
	u32	nr_masters;
	u32	m_data_width;
	u32	block_size[DMAC_MAX_CHANNELS];
	u32	priority[DMAC_MAX_CHANNELS];
};

struct axi_dma_chan {
	struct axi_dma_chip		*chip;
	void __iomem			*chan_regs;
	u8				id;
	unsigned int			descs_allocated;

	struct virt_dma_chan		vc;

	/* these other elements are all protected by vc.lock */
	bool				is_paused;
};

struct dw_axi_dma {
	struct dma_device	dma;
	struct dw_axi_dma_hcfg	*hdata;
	struct dma_pool		*desc_pool;

	/* channels */
	struct axi_dma_chan	*chan;
};

struct axi_dma_chip {
	struct device		*dev;
	int			irq;
	void __iomem		*regs;
	struct clk		*clk;
	struct dw_axi_dma	*dw;
};

/* LLI == Linked List Item */
struct __attribute__ ((__packed__)) axi_dma_lli {
	__le64		sar;
	__le64		dar;
	__le32		block_ts_lo;
	__le32		block_ts_hi;
	__le64		llp;
	__le32		ctl_lo;
	__le32		ctl_hi;
	__le32		sstat;
	__le32		dstat;
	__le32		status_lo;
	__le32		ststus_hi;
	__le32		reserved_lo;
	__le32		reserved_hi;
};

struct axi_dma_desc {
	struct axi_dma_lli		lli;

	struct virt_dma_desc		vd;
	struct axi_dma_chan		*chan;
	struct list_head		xfer_list;
};

static inline struct device *dchan2dev(struct dma_chan *dchan)
{
	return &dchan->dev->device;
}

static inline struct device *chan2dev(struct axi_dma_chan *chan)
{
	return &chan->vc.chan.dev->device;
}

static inline struct axi_dma_desc *vd_to_axi_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct axi_dma_desc, vd);
}

static inline struct axi_dma_chan *vc_to_axi_dma_chan(struct virt_dma_chan *vc)
{
	return container_of(vc, struct axi_dma_chan, vc);
}

static inline struct axi_dma_chan *dchan_to_axi_dma_chan(struct dma_chan *dchan)
{
	return vc_to_axi_dma_chan(to_virt_chan(dchan));
}

#endif /* _AXI_DMA_PLATFORM_H */
