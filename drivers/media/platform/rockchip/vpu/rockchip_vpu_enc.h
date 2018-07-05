// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright (C) 2018 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifndef ROCKCHIP_VPU_ENC_H_
#define ROCKCHIP_VPU_ENC_H_

extern const struct v4l2_ioctl_ops rockchip_vpu_enc_ioctl_ops;
extern const struct vb2_ops rockchip_vpu_enc_queue_ops;

int rockchip_vpu_enc_init(struct rockchip_vpu_ctx *ctx);
void rockchip_vpu_enc_exit(struct rockchip_vpu_ctx *ctx);

#endif /* ROCKCHIP_VPU_ENC_H_  */
