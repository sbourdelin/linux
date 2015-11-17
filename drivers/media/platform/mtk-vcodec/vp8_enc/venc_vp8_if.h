/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Daniel Hsiao <daniel.hsiao@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _VENC_VP8_IF_H_
#define _VENC_VP8_IF_H_

#include "venc_drv_base.h"
#include "venc_vp8_vpu.h"

/*
 * struct venc_vp8_handle - vp8 encoder AP driver handle
 * @hw_base: vp8 encoder hardware register base
 * @work_bufs: working buffer
 * @work_buf_allocated: working buffer allocated flag
 * @frm_cnt: encoded frame count, it's used for I-frame judgement and
 *	     reset when force intra cmd received.
 * @ts_mode: temporal scalability mode (0: disable, 1: enable)
 *	     support three temporal layers - 0: 7.5fps 1: 7.5fps 2: 15fps.
 * @vpu_inst: VPU instance to exchange information between AP and VPU
 * @ctx: context for v4l2 layer integration
 * @dev: device for v4l2 layer integration
 */
struct venc_vp8_handle {
	void __iomem *hw_base;
	struct mtk_vcodec_mem work_bufs[VENC_VP8_VPU_WORK_BUF_MAX];
	bool work_buf_allocated;
	unsigned int frm_cnt;
	unsigned int ts_mode;
	struct venc_vp8_vpu_inst vpu_inst;
	void *ctx;
	struct platform_device *dev;
};

struct venc_common_if *get_vp8_enc_comm_if(void);

#endif
