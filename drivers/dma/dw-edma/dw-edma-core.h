/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare eDMA core driver
 */

#ifndef _DW_EDMA_CORE_H
#define _DW_EDMA_CORE_H

#include <linux/dma/edma.h>

#include "../virt-dma.h"

#define DRV_NAME				"dw-edma"

enum dw_edma_dir {
	EDMA_DIR_WRITE = 0,
	EDMA_DIR_READ
};

enum dw_edma_mode {
	EDMA_MODE_LEGACY = 0,
	EDMA_MODE_UNROLL
};

enum dw_edma_request {
	EDMA_REQ_NONE = 0,
	EDMA_REQ_STOP,
	EDMA_REQ_PAUSE
};

enum dw_edma_status {
	EDMA_ST_IDLE = 0,
	EDMA_ST_PAUSE,
	EDMA_ST_BUSY
};

struct dw_edma_chan;
struct dw_edma_chunk;

struct dw_edma_core_ops {
	/* eDMA management callbacks */
	void (*off)(struct dw_edma *dw);
	u16 (*ch_count)(struct dw_edma *dw, enum dw_edma_dir dir);
	enum dma_status (*ch_status)(struct dw_edma_chan *chan);
	void (*clear_done_int)(struct dw_edma_chan *chan);
	void (*clear_abort_int)(struct dw_edma_chan *chan);
	bool (*status_done_int)(struct dw_edma_chan *chan);
	bool (*status_abort_int)(struct dw_edma_chan *chan);
	void (*start)(struct dw_edma_chunk *chunk, bool first);
	int (*device_config)(struct dma_chan *dchan);
	/* eDMA debug fs callbacks */
	int (*debugfs_on)(struct dw_edma_chip *chip);
	void (*debugfs_off)(void);
};

struct dw_edma_burst {
	struct list_head		list;
	u64				sar;
	u64				dar;
	u32				sz;
};

struct dw_edma_chunk {
	struct list_head		list;
	struct dw_edma_chan		*chan;
	struct dw_edma_burst		*burst;

	u32				bursts_alloc;

	u8				cb;
	u32				sz;

	dma_addr_t			p_addr;		/* Linked list */
	dma_addr_t			v_addr;		/* Linked list */
};

struct dw_edma_desc {
	struct virt_dma_desc		vd;
	struct dw_edma_chan		*chan;
	struct dw_edma_chunk		*chunk;

	u32				chunks_alloc;

	u32				alloc_sz;
	u32				xfer_sz;
};

struct dw_edma_chan {
	struct virt_dma_chan		vc;
	struct dw_edma_chip		*chip;
	int				id;
	enum dw_edma_dir		dir;

	u64				ll_off;
	u32				ll_max;

	u64				msi_done_addr;
	u64				msi_abort_addr;
	u32				msi_data;

	enum dw_edma_request		request;
	enum dw_edma_status		status;
	u8				configured;

	dma_addr_t			p_addr;		/* Data */
};

struct dw_edma {
	char				name[20];

	struct dma_device		wr_edma;
	u16				wr_ch_count;
	struct dma_device		rd_edma;
	u16				rd_ch_count;

	void __iomem			*regs;

	void __iomem			*va_ll;
	resource_size_t			pa_ll;
	size_t				ll_sz;

	u64				msi_addr;
	u32				msi_data;

	u32				version;
	enum dw_edma_mode		mode;

	struct dw_edma_chan		*chan;
	const struct dw_edma_core_ops	*ops;

	raw_spinlock_t			lock;		/* Only for legacy */
};

static inline
struct dw_edma_chan *vc2dw_edma_chan(struct virt_dma_chan *vc)
{
	return container_of(vc, struct dw_edma_chan, vc);
}

static inline
struct dw_edma_chan *dchan2dw_edma_chan(struct dma_chan *dchan)
{
	return vc2dw_edma_chan(to_virt_chan(dchan));
}

#endif /* _DW_EDMA_CORE_H */
