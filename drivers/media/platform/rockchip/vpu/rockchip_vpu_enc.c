// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright (C) 2018 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-sg.h>

#include "rockchip_vpu.h"
#include "rockchip_vpu_enc.h"
#include "rockchip_vpu_hw.h"

#define JPEG_MAX_BYTES_PER_PIXEL 2

static const struct rockchip_vpu_fmt *
rockchip_vpu_find_format(struct rockchip_vpu_dev *dev, u32 fourcc)
{
	const struct rockchip_vpu_fmt *formats = dev->variant->enc_fmts;
	unsigned int i;

	for (i = 0; i < dev->variant->num_enc_fmts; i++) {
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	}

	return NULL;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_default_fmt(struct rockchip_vpu_dev *dev, bool bitstream)
{
	const struct rockchip_vpu_fmt *formats = dev->variant->enc_fmts;
	unsigned int i;

	for (i = 0; i < dev->variant->num_enc_fmts; i++) {
		if (bitstream == (formats[i].codec_mode != RK_VPU_CODEC_NONE))
			return &formats[i];
	}

	/* There must be at least one raw and one coded format in the array. */
	BUG_ON(i >= dev->variant->num_enc_fmts);
	return NULL;
}

static const struct v4l2_ctrl_config controls[] = {
	[ROCKCHIP_VPU_ENC_CTRL_Y_QUANT_TBL] = {
		.id = V4L2_CID_JPEG_LUMA_QUANTIZATION,
		.type = V4L2_CTRL_TYPE_U8,
		.step = 1,
		.def = 0x00,
		.min = 0x00,
		.max = 0xff,
		.dims = { 8, 8 }
	},
	[ROCKCHIP_VPU_ENC_CTRL_C_QUANT_TBL] = {
		.id = V4L2_CID_JPEG_CHROMA_QUANTIZATION,
		.type = V4L2_CTRL_TYPE_U8,
		.step = 1,
		.def = 0x00,
		.min = 0x00,
		.max = 0xff,
		.dims = { 8, 8 }
	},
};

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(file);

	strlcpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strlcpy(cap->card, vpu->vfd->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);

	/*
	 * This is only a mem-to-mem video device.
	 */
	cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *prov,
				  struct v4l2_frmsizeenum *fsize)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	const struct rockchip_vpu_fmt *fmt;

	if (fsize->index != 0) {
		vpu_debug(0, "invalid frame size index (expected 0, got %d)\n",
				fsize->index);
		return -EINVAL;
	}

	fmt = rockchip_vpu_find_format(dev, fsize->pixel_format);
	if (!fmt) {
		vpu_debug(0, "unsupported bitstream format (%08x)\n",
				fsize->pixel_format);
		return -EINVAL;
	}

	/* This only makes sense for codec formats */
	if (fmt->codec_mode == RK_VPU_CODEC_NONE)
		return -ENOTTY;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int vidioc_enum_fmt_vid_cap_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	const struct rockchip_vpu_fmt *fmt;
	const struct rockchip_vpu_fmt *formats = dev->variant->enc_fmts;
	int i, j = 0;

	for (i = 0; i < dev->variant->num_enc_fmts; i++) {
		/* Skip uncompressed formats */
		if (formats[i].codec_mode == RK_VPU_CODEC_NONE)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			strlcpy(f->description,
				fmt->name, sizeof(f->description));
			f->pixelformat = fmt->fourcc;
			f->flags = 0;
			f->flags |= V4L2_FMT_FLAG_COMPRESSED;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_out_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	const struct rockchip_vpu_fmt *fmt;
	const struct rockchip_vpu_fmt *formats = dev->variant->enc_fmts;
	int i, j = 0;

	for (i = 0; i < dev->variant->num_enc_fmts; i++) {
		if (formats[i].codec_mode != RK_VPU_CODEC_NONE)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			strlcpy(f->description,
				fmt->name, sizeof(f->description));
			f->pixelformat = fmt->fourcc;
			f->flags = 0;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static int vidioc_g_fmt_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->src_fmt;
	pix_mp->colorspace = ctx->colorspace;
	pix_mp->ycbcr_enc = ctx->ycbcr_enc;
	pix_mp->xfer_func = ctx->xfer_func;
	pix_mp->quantization = ctx->quantization;

	return 0;
}

static int vidioc_g_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->dst_fmt;
	pix_mp->colorspace = ctx->colorspace;
	pix_mp->ycbcr_enc = ctx->ycbcr_enc;
	pix_mp->xfer_func = ctx->xfer_func;
	pix_mp->quantization = ctx->quantization;

	return 0;
}

static void calculate_plane_sizes(const struct rockchip_vpu_fmt *fmt,
				  struct v4l2_pix_format_mplane *pix_mp)
{
	unsigned int w = pix_mp->width;
	unsigned int h = pix_mp->height;
	int i;

	for (i = 0; i < fmt->num_planes; ++i) {
		memset(pix_mp->plane_fmt[i].reserved, 0,
		       sizeof(pix_mp->plane_fmt[i].reserved));
		pix_mp->plane_fmt[i].bytesperline = w * fmt->depth[i] / 8;
		pix_mp->plane_fmt[i].sizeimage = h *
					pix_mp->plane_fmt[i].bytesperline;
		/*
		 * All of multiplanar formats we support have chroma
		 * planes subsampled by 2 vertically.
		 */
		if (i != 0)
			pix_mp->plane_fmt[i].sizeimage /= 2;
	}
}

static int vidioc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	char str[5];

	vpu_debug(4, "%s\n", fmt2str(pix_mp->pixelformat, str));

	fmt = rockchip_vpu_find_format(dev, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(dev, true);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	/* Limit to hardware min/max. */
	pix_mp->width = clamp(pix_mp->width,
			ctx->vpu_dst_fmt->frmsize.min_width,
			ctx->vpu_dst_fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			ctx->vpu_dst_fmt->frmsize.min_height,
			ctx->vpu_dst_fmt->frmsize.max_height);
	pix_mp->num_planes = fmt->num_planes;

	pix_mp->plane_fmt[0].sizeimage =
		pix_mp->width * pix_mp->height * JPEG_MAX_BYTES_PER_PIXEL;
	memset(pix_mp->plane_fmt[0].reserved, 0,
	       sizeof(pix_mp->plane_fmt[0].reserved));
	pix_mp->field = V4L2_FIELD_NONE;

	return 0;
}

static int vidioc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	char str[5];
	unsigned long dma_align;
	bool need_alignment;
	int i;

	vpu_debug(4, "%s\n", fmt2str(pix_mp->pixelformat, str));

	fmt = rockchip_vpu_find_format(dev, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(dev, false);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	/* Limit to hardware min/max. */
	pix_mp->width = clamp(pix_mp->width,
			ctx->vpu_dst_fmt->frmsize.min_width,
			ctx->vpu_dst_fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			ctx->vpu_dst_fmt->frmsize.min_height,
			ctx->vpu_dst_fmt->frmsize.max_height);
	/* Round up to macroblocks. */
	pix_mp->width = round_up(pix_mp->width, MB_DIM);
	pix_mp->height = round_up(pix_mp->height, MB_DIM);
	pix_mp->num_planes = fmt->num_planes;
	pix_mp->field = V4L2_FIELD_NONE;

	vpu_debug(0, "OUTPUT codec mode: %d\n", fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d, mb - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height,
		  MB_WIDTH(pix_mp->width),
		  MB_HEIGHT(pix_mp->height));

	/* Fill remaining fields */
	calculate_plane_sizes(fmt, pix_mp);

	dma_align = dma_get_cache_alignment();
	need_alignment = false;
	for (i = 0; i < fmt->num_planes; i++) {
		if (!IS_ALIGNED(pix_mp->plane_fmt[i].sizeimage,
				dma_align)) {
			need_alignment = true;
			break;
		}
	}
	if (!need_alignment)
		return 0;

	pix_mp->height = round_up(pix_mp->height, dma_align * 4 / MB_DIM);
	if (pix_mp->height > ctx->vpu_dst_fmt->frmsize.max_height) {
		vpu_err("Aligned height higher than maximum.\n");
		return -EINVAL;
	}
	/* Fill in remaining fields, again */
	calculate_plane_sizes(fmt, pix_mp);
	return 0;
}

static void rockchip_vpu_reset_dst_fmt(struct rockchip_vpu_dev *vpu,
					struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *fmt = &ctx->dst_fmt;

	ctx->vpu_dst_fmt = rockchip_vpu_get_default_fmt(vpu, true);

	memset(fmt, 0, sizeof(*fmt));

	fmt->width = ctx->vpu_dst_fmt->frmsize.min_width;
	fmt->height = ctx->vpu_dst_fmt->frmsize.min_height;
	fmt->pixelformat = ctx->vpu_dst_fmt->fourcc;
	fmt->num_planes = ctx->vpu_dst_fmt->num_planes;
	fmt->plane_fmt[0].sizeimage =
		fmt->width * fmt->height * JPEG_MAX_BYTES_PER_PIXEL;

	fmt->field = V4L2_FIELD_NONE;

	fmt->colorspace = ctx->colorspace;
	fmt->ycbcr_enc = ctx->ycbcr_enc;
	fmt->xfer_func = ctx->xfer_func;
	fmt->quantization = ctx->quantization;
}

static void rockchip_vpu_reset_src_fmt(struct rockchip_vpu_dev *vpu,
					struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;

	ctx->vpu_src_fmt = rockchip_vpu_get_default_fmt(vpu, false);

	memset(fmt, 0, sizeof(*fmt));

	fmt->width = ctx->vpu_dst_fmt->frmsize.min_width;
	fmt->height = ctx->vpu_dst_fmt->frmsize.min_height;
	fmt->pixelformat = ctx->vpu_src_fmt->fourcc;
	fmt->num_planes = ctx->vpu_src_fmt->num_planes;

	fmt->field = V4L2_FIELD_NONE;

	fmt->colorspace = ctx->colorspace;
	fmt->ycbcr_enc = ctx->ycbcr_enc;
	fmt->xfer_func = ctx->xfer_func;
	fmt->quantization = ctx->quantization;

	calculate_plane_sizes(ctx->vpu_src_fmt, fmt);
}

static int vidioc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	ctx->colorspace = pix_mp->colorspace;
	ctx->ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->xfer_func = pix_mp->xfer_func;
	ctx->quantization = pix_mp->quantization;

	/*
	 * Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq) &&
	    pix_mp->pixelformat != ctx->src_fmt.pixelformat)
		return -EBUSY;

	ret = vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	ctx->vpu_src_fmt = rockchip_vpu_find_format(vpu,
		pix_mp->pixelformat);

	/* Reset crop rectangle. */
	ctx->src_crop.width = pix_mp->width;
	ctx->src_crop.height = pix_mp->height;
	ctx->src_fmt = *pix_mp;

	return 0;
}

static int vidioc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	ctx->colorspace = pix_mp->colorspace;
	ctx->ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->xfer_func = pix_mp->xfer_func;
	ctx->quantization = pix_mp->quantization;

	/*
	 * Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_busy(peer_vq) &&
	    pix_mp->pixelformat != ctx->dst_fmt.pixelformat)
		return -EBUSY;

	ret = vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->vpu_dst_fmt = rockchip_vpu_find_format(vpu, pix_mp->pixelformat);
	ctx->dst_fmt = *pix_mp;

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 */
	rockchip_vpu_reset_src_fmt(vpu, ctx);

	return 0;
}

static int vidioc_cropcap(struct file *file, void *priv,
			  struct v4l2_cropcap *cap)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;

	/* Crop only supported on source. */
	if (cap->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	cap->bounds.left = 0;
	cap->bounds.top = 0;
	cap->bounds.width = fmt->width;
	cap->bounds.height = fmt->height;
	cap->defrect = cap->bounds;
	cap->pixelaspect.numerator = 1;
	cap->pixelaspect.denominator = 1;

	return 0;
}

static int vidioc_g_crop(struct file *file, void *priv, struct v4l2_crop *crop)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	int ret = 0;

	/* Crop only supported on source. */
	if (crop->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	crop->c = ctx->src_crop;

	return ret;
}

static int vidioc_s_crop(struct file *file, void *priv,
			 const struct v4l2_crop *crop)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;
	const struct v4l2_rect *rect = &crop->c;
	struct vb2_queue *vq;

	/* Crop only supported on source. */
	if (crop->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	/* Change not allowed if the queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, crop->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	/* We do not support offsets. */
	if (rect->left != 0 || rect->top != 0)
		goto fallback;

	/* We can crop only inside right- or bottom-most macroblocks. */
	if (round_up(rect->width, MB_DIM) != fmt->width
	    || round_up(rect->height, MB_DIM) != fmt->height)
		goto fallback;

	/* We support widths aligned to 4 pixels and arbitrary heights. */
	ctx->src_crop.width = round_up(rect->width, 4);
	ctx->src_crop.height = rect->height;

	return 0;

fallback:
	/* Default to full frame for incorrect settings. */
	ctx->src_crop.width = fmt->width;
	ctx->src_crop.height = fmt->height;
	return 0;
}

const struct v4l2_ioctl_ops rockchip_vpu_enc_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_cap,
	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out_mplane,
	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap_mplane,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_cropcap = vidioc_cropcap,
	.vidioc_g_crop = vidioc_g_crop,
	.vidioc_s_crop = vidioc_s_crop,
};

static int rockchip_vpu_queue_setup(struct vb2_queue *vq,
				  unsigned int *num_buffers,
				  unsigned int *num_planes,
				  unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	int ret = 0;
	int i;

	*num_buffers = clamp_t(unsigned int,
			*num_buffers, 1, VIDEO_MAX_FRAME);

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		*num_planes = ctx->vpu_dst_fmt->num_planes;

		sizes[0] = ctx->dst_fmt.plane_fmt[0].sizeimage;
		vpu_debug(0, "capture sizes[%d]: %d\n", 0, sizes[0]);
		break;

	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		*num_planes = ctx->vpu_src_fmt->num_planes;

		for (i = 0; i < ctx->vpu_src_fmt->num_planes; ++i) {
			sizes[i] = ctx->src_fmt.plane_fmt[i].sizeimage;
			vpu_debug(0, "output sizes[%d]: %d\n", i, sizes[i]);
		}
		break;

	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		ret = -EINVAL;
	}

	return ret;
}

static int rockchip_vpu_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	unsigned int sz;
	int ret = 0;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		sz = ctx->dst_fmt.plane_fmt[0].sizeimage;

		vpu_debug(4, "plane size: %ld, dst size: %d\n",
			vb2_plane_size(vb, 0), sz);
		if (vb2_plane_size(vb, 0) < sz) {
			vpu_err("plane size is too small for capture\n");
			ret = -EINVAL;
		}
		break;

	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		for (i = 0; i < ctx->vpu_src_fmt->num_planes; ++i) {
			sz = ctx->src_fmt.plane_fmt[i].sizeimage;

			vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n", i,
				vb2_plane_size(vb, i), sz);
			if (vb2_plane_size(vb, i) < sz) {
				vpu_err("size of plane %d is too small for output\n",
					i);
				break;
			}
		}

		if (i != ctx->vpu_src_fmt->num_planes)
			ret = -EINVAL;
		break;

	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		ret = -EINVAL;
	}

	return ret;
}

static void rockchip_vpu_buf_queue(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int rockchip_vpu_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);
	enum rockchip_vpu_codec_mode codec_mode;

	/* Set codec_ops for the chosen destination format */
	codec_mode = ctx->vpu_dst_fmt->codec_mode;
	ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];

	return 0;
}

static void rockchip_vpu_stop_streaming(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	/* The mem2mem framework calls v4l2_m2m_cancel_job before
	 * .stop_streaming, so there isn't any job running and
	 * it is safe to return all the buffers.
	 */
	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

const struct vb2_ops rockchip_vpu_enc_queue_ops = {
	.queue_setup = rockchip_vpu_queue_setup,
	.buf_prepare = rockchip_vpu_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.start_streaming = rockchip_vpu_start_streaming,
	.stop_streaming = rockchip_vpu_stop_streaming,
};

int rockchip_vpu_enc_ctrls_setup(struct rockchip_vpu_ctx *ctx)
{
	int i, num_ctrls = ARRAY_SIZE(controls);

	if (num_ctrls > ARRAY_SIZE(ctx->ctrls)) {
		vpu_err("context control array not large enough\n");
		return -EINVAL;
	}

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, num_ctrls);
	if (ctx->ctrl_handler.error) {
		vpu_err("v4l2_ctrl_handler_init failed\n");
		return ctx->ctrl_handler.error;
	}

	for (i = 0; i < num_ctrls; i++) {
		ctx->ctrls[i] = v4l2_ctrl_new_custom(&ctx->ctrl_handler,
						     &controls[i], NULL);
		if (ctx->ctrl_handler.error) {
			vpu_err("Adding control (%d) failed %d\n", i,
				ctx->ctrl_handler.error);
			v4l2_ctrl_handler_free(&ctx->ctrl_handler);
			return ctx->ctrl_handler.error;
		}
	}

	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
	ctx->num_ctrls = num_ctrls;
	return 0;
}

int rockchip_vpu_enc_init(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;
	int ret;

	rockchip_vpu_reset_dst_fmt(vpu, ctx);
	rockchip_vpu_reset_src_fmt(vpu, ctx);

	ret = rockchip_vpu_enc_ctrls_setup(ctx);
	if (ret) {
		vpu_err("Failed to set up controls.\n");
		return ret;
	}
	return 0;
}

void rockchip_vpu_enc_exit(struct rockchip_vpu_ctx *ctx)
{
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
}
