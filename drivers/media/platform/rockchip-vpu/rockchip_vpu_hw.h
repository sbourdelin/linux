/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
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

#ifndef ROCKCHIP_VPU_HW_H_
#define ROCKCHIP_VPU_HW_H_

#include <media/videobuf2-core.h>

#define ROCKCHIP_HEADER_SIZE		1280
#define ROCKCHIP_HW_PARAMS_SIZE		5487
#define ROCKCHIP_RET_PARAMS_SIZE	488

struct rockchip_vpu_dev;
struct rockchip_vpu_ctx;
struct rockchip_vpu_buf;

/**
 * enum rockchip_vpu_type - vpu type.
 * @RK_VPU_NONE:	No vpu type. Used for RAW video formats.
 * @RK3288_VPU:		Vpu on rk3288 soc.
 * @RK3229_VPU:		Vpu on rk3229 soc.
 * @RKVDEC:		Rkvdec.
 */
enum rockchip_vpu_type {
	RK_VPU_NONE	= -1,
	RK3288_VPU,
	RK3229_VPU,
};

#define ROCKCHIP_VPU_MATCHES(type1, type2) \
	(type1 == RK_VPU_NONE || type2 == RK_VPU_NONE || type1 == type2)

/**
 * struct rockchip_vpu_aux_buf - auxiliary DMA buffer for hardware data
 * @cpu:	CPU pointer to the buffer.
 * @dma:	DMA address of the buffer.
 * @size:	Size of the buffer.
 */
struct rockchip_vpu_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/**
 * struct rockchip_vpu_hw_ctx - Context private data of hardware code.
 * @codec_ops:		Set of operations associated with current codec mode.
 */
struct rockchip_vpu_hw_ctx {
	const struct rockchip_vpu_codec_ops *codec_ops;

	/* Specific for particular codec modes. */
};

int rockchip_vpu_hw_probe(struct rockchip_vpu_dev *vpu);
void rockchip_vpu_hw_remove(struct rockchip_vpu_dev *vpu);

int rockchip_vpu_init(struct rockchip_vpu_ctx *ctx);
void rockchip_vpu_deinit(struct rockchip_vpu_ctx *ctx);

void rockchip_vpu_run(struct rockchip_vpu_ctx *ctx);

void rockchip_vpu_power_on(struct rockchip_vpu_dev *vpu);

#endif /* RK3288_VPU_HW_H_ */
