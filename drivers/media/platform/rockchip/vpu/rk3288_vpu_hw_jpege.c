// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 */

#include <asm/unaligned.h>
#include <media/v4l2-mem2mem.h>
#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"
#include "rk3288_vpu_regs.h"

#define VEPU_JPEG_QUANT_TABLE_COUNT 16

static void rk3288_vpu_set_src_img_ctrl(struct rockchip_vpu_dev *vpu,
					struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	struct v4l2_rect *crop = &ctx->src_crop;
	u32 overfill_r, overfill_b;
	u32 reg;

	overfill_r = pix_fmt->width - crop->width;
	overfill_b = pix_fmt->height - crop->height;

	reg = VEPU_REG_IN_IMG_CTRL_ROW_LEN(pix_fmt->width)
		| VEPU_REG_IN_IMG_CTRL_OVRFLR_D4(overfill_r)
		| VEPU_REG_IN_IMG_CTRL_OVRFLB_D4(overfill_b)
		| VEPU_REG_IN_IMG_CTRL_FMT(ctx->vpu_src_fmt->enc_fmt);
	vepu_write_relaxed(vpu, reg, VEPU_REG_IN_IMG_CTRL);
}

static void rk3288_vpu_jpege_set_buffers(struct rockchip_vpu_dev *vpu,
					 struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	struct vb2_buffer *buf;
	dma_addr_t dst, src[3];
	u32 dst_size;

	WARN_ON(pix_fmt->num_planes > 3);

	buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	dst = vb2_dma_contig_plane_dma_addr(buf, 0);
	dst_size = vb2_plane_size(buf, 0);

	vepu_write_relaxed(vpu, dst, VEPU_REG_ADDR_OUTPUT_STREAM);
	vepu_write_relaxed(vpu, dst_size, VEPU_REG_STR_BUF_LIMIT);

	buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (pix_fmt->num_planes == 1) {
		src[0] = vb2_dma_contig_plane_dma_addr(buf, 0);
		/* single plane formats we supported are all interlaced */
		src[1] = src[2] = src[0];
	} else if (pix_fmt->num_planes == 2) {
		src[PLANE_Y] = vb2_dma_contig_plane_dma_addr(buf, PLANE_Y);
		src[PLANE_CB] = vb2_dma_contig_plane_dma_addr(buf, PLANE_CB);
		src[PLANE_CR] = src[PLANE_CB];
	} else {
		src[PLANE_Y] = vb2_dma_contig_plane_dma_addr(buf, PLANE_Y);
		src[PLANE_CB] = vb2_dma_contig_plane_dma_addr(buf, PLANE_CB);
		src[PLANE_CR] = vb2_dma_contig_plane_dma_addr(buf, PLANE_CR);
	}

	vepu_write_relaxed(vpu, src[PLANE_Y], VEPU_REG_ADDR_IN_LUMA);
	vepu_write_relaxed(vpu, src[PLANE_CB], VEPU_REG_ADDR_IN_CB);
	vepu_write_relaxed(vpu, src[PLANE_CR], VEPU_REG_ADDR_IN_CR);
}

static void rk3288_vpu_jpege_set_qtables(struct rockchip_vpu_dev *vpu,
		__be32 *luma_qtable, __be32 *chroma_qtable)
{
	u32 reg, i;

	for (i = 0; i < VEPU_JPEG_QUANT_TABLE_COUNT; i++) {
		reg = get_unaligned_be32(&luma_qtable[i]);
		vepu_write_relaxed(vpu, reg, VEPU_REG_JPEG_LUMA_QUAT(i));

		reg = get_unaligned_be32(&chroma_qtable[i]);
		vepu_write_relaxed(vpu, reg, VEPU_REG_JPEG_CHROMA_QUAT(i));
	}
}

void rk3288_vpu_jpege_run(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;
	__be32 *chroma_qtable = NULL;
	__be32 *luma_qtable = NULL;
	u32 reg;

	if (ctx->vpu_dst_fmt->fourcc == V4L2_PIX_FMT_JPEG_RAW) {
		struct v4l2_ctrl *ctrl;

		ctrl = ctx->ctrls[ROCKCHIP_VPU_ENC_CTRL_Y_QUANT_TBL];
		luma_qtable = (__be32 *)ctrl->p_cur.p;

		ctrl = ctx->ctrls[ROCKCHIP_VPU_ENC_CTRL_C_QUANT_TBL];
		chroma_qtable = (__be32 *)ctrl->p_cur.p;
	}

	/* Switch to JPEG encoder mode before writing registers */
	vepu_write_relaxed(vpu, VEPU_REG_ENC_CTRL_ENC_MODE_JPEG,
			   VEPU_REG_ENC_CTRL);

	rk3288_vpu_set_src_img_ctrl(vpu, ctx);
	rk3288_vpu_jpege_set_buffers(vpu, ctx);
	if (luma_qtable && chroma_qtable)
		rk3288_vpu_jpege_set_qtables(vpu, luma_qtable, chroma_qtable);

	/* Make sure that all registers are written at this point. */
	wmb();

	/* Start the hardware. */
	reg = VEPU_REG_AXI_CTRL_OUTPUT_SWAP16
		| VEPU_REG_AXI_CTRL_INPUT_SWAP16
		| VEPU_REG_AXI_CTRL_BURST_LEN(16)
		| VEPU_REG_AXI_CTRL_OUTPUT_SWAP32
		| VEPU_REG_AXI_CTRL_INPUT_SWAP32
		| VEPU_REG_AXI_CTRL_OUTPUT_SWAP8
		| VEPU_REG_AXI_CTRL_INPUT_SWAP8;
	vepu_write(vpu, reg, VEPU_REG_AXI_CTRL);

	reg = VEPU_REG_ENC_CTRL_WIDTH(MB_WIDTH(ctx->src_fmt.width))
		| VEPU_REG_ENC_CTRL_HEIGHT(MB_HEIGHT(ctx->src_fmt.height))
		| VEPU_REG_ENC_CTRL_ENC_MODE_JPEG
		| VEPU_REG_ENC_PIC_INTRA
		| VEPU_REG_ENC_CTRL_EN_BIT;
	/* Kick the watchdog and start encoding */
	schedule_delayed_work(&vpu->watchdog_work, msecs_to_jiffies(2000));
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL);
}

void rk3288_vpu_jpege_done(struct rockchip_vpu_ctx *ctx,
			   enum vb2_buffer_state result)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	WARN_ON(!src);
	WARN_ON(!dst);

	dst->vb2_buf.planes[0].bytesused =
		vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT) / 8;
	dst->timecode = src->timecode;
	dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
	dst->flags &= ~V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
	dst->flags |= src->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;

	v4l2_m2m_buf_done(src, result);
	v4l2_m2m_buf_done(dst, result);
	v4l2_m2m_job_finish(vpu->m2m_dev, ctx->fh.m2m_ctx);
}
