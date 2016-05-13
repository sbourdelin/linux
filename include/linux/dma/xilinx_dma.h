/*
 * Xilinx DMA Engine drivers support header file
 *
 * Copyright (C) 2010-2014 Xilinx, Inc. All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __DMA_XILINX_DMA_H
#define __DMA_XILINX_DMA_H

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

/**
 * struct xilinx_vdma_config - VDMA Configuration structure
 * @frm_dly: Frame delay
 * @gen_lock: Whether in gen-lock mode
 * @master: Master that it syncs to
 * @frm_cnt_en: Enable frame count enable
 * @park: Whether wants to park
 * @park_frm: Frame to park on
 * @coalesc: Interrupt coalescing threshold
 * @delay: Delay counter
 * @reset: Reset Channel
 * @ext_fsync: External Frame Sync source
 */
struct xilinx_vdma_config {
	int frm_dly;
	int gen_lock;
	int master;
	int frm_cnt_en;
	int park;
	int park_frm;
	int coalesc;
	int delay;
	int reset;
	int ext_fsync;
};

/**
 * struct zynqmp_dma_config - ZYNQMP DMA Configuration structure
 * @ovrfetch: Overfetch status
 * @has_sg: Support scatter gather transfers
 * @ratectrl: Rate control value
 * @src_issue: Out standing transactions on source
 * @src_burst_len: Source burst length
 * @dst_burst_len: Dest burst length
 */
struct zynqmp_dma_config {
	bool ovrfetch;
	bool has_sg;
	u32 ratectrl;
	u32 src_issue;
	u32 src_burst_len;
	u32 dst_burst_len;
};

/**
 * enum xdma_ip_type: DMA IP type.
 *
 * XDMA_TYPE_AXIDMA: Axi dma ip.
 * XDMA_TYPE_CDMA: Axi cdma ip.
 * XDMA_TYPE_VDMA: Axi vdma ip.
 *
 */
enum xdma_ip_type {
	XDMA_TYPE_AXIDMA = 0,
	XDMA_TYPE_CDMA,
	XDMA_TYPE_VDMA,
};

int xilinx_vdma_channel_set_config(struct dma_chan *dchan,
					struct xilinx_vdma_config *cfg);
int zynqmp_dma_channel_set_config(struct dma_chan *dchan,
					struct zynqmp_dma_config *cfg);
#endif
