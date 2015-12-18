/*
 * Copyright (C) STMicroelectronics SA 2015
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Hugues Fruchet <hugues.fruchet@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/of.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include "hva.h"
#include "hva-hw.h"

#define HVA_NAME "hva"

/*
 * 1 frame at least for user
 * limit number of frames to 16
 */
#define MAX_FRAMES	16
#define MIN_FRAMES	1

#define HVA_MIN_WIDTH	32
#define HVA_MAX_WIDTH	1920
#define HVA_MIN_HEIGHT	32
#define HVA_MAX_HEIGHT	1080

#define DFT_CFG_WIDTH		HVA_MIN_WIDTH
#define	DFT_CFG_HEIGHT		HVA_MIN_HEIGHT
#define DFT_CFG_BITRATE_MODE	V4L2_MPEG_VIDEO_BITRATE_MODE_CBR
#define DFT_CFG_GOP_SIZE	16
#define DFT_CFG_INTRA_REFRESH	true
#define DFT_CFG_FRAME_NUM	1
#define DFT_CFG_FRAME_DEN	30
#define DFT_CFG_QPMIN		5
#define DFT_CFG_QPMAX		51
#define DFT_CFG_DCT8X8		false
#define DFT_CFG_COMP_QUALITY	85
#define DFT_CFG_SAR_ENABLE	1
#define DFT_CFG_BITRATE		(20000 * 1024)
#define DFT_CFG_CPB_SIZE	(25000 * 1024)

static const struct hva_frameinfo frame_dflt_fmt = {
	.fmt		= {
				.pixelformat	= V4L2_PIX_FMT_NV12,
				.nb_planes	= 2,
				.bpp		= 12,
				.bpp_plane0	= 8,
				.w_align	= 2,
				.h_align	= 2
			  },
	.width		= DFT_CFG_WIDTH,
	.height		= DFT_CFG_HEIGHT,
	.crop		= {0, 0, DFT_CFG_WIDTH, DFT_CFG_HEIGHT},
	.frame_width	= DFT_CFG_WIDTH,
	.frame_height	= DFT_CFG_HEIGHT
};

static const struct hva_streaminfo stream_dflt_fmt = {
	.width		= DFT_CFG_WIDTH,
	.height		= DFT_CFG_HEIGHT
};

/* list of stream formats supported by hva hardware */
const u32 stream_fmt[] = {
};

/* list of pixel formats supported by hva hardware */
static const struct hva_frame_fmt frame_fmts[] = {
	/* NV12. YUV420SP - 1 plane for Y + 1 plane for (CbCr) */
	{
		.pixelformat	= V4L2_PIX_FMT_NV12,
		.nb_planes	= 2,
		.bpp		= 12,
		.bpp_plane0	= 8,
		.w_align	= 2,
		.h_align	= 2
	},
	/* NV21. YUV420SP - 1 plane for Y + 1 plane for (CbCr) */
	{
		.pixelformat	= V4L2_PIX_FMT_NV21,
		.nb_planes	= 2,
		.bpp		= 12,
		.bpp_plane0	= 8,
		.w_align	= 2,
		.h_align	= 2
	},
};

/* offset to differentiate OUTPUT/CAPTURE @mmap */
#define MMAP_FRAME_OFFSET (UL(0x100000000) / 2)

/* registry of available encoders */
const struct hva_encoder *hva_encoders[] = {
};

static const struct hva_frame_fmt *hva_find_frame_fmt(u32 pixelformat)
{
	const struct hva_frame_fmt *fmt;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frame_fmts); i++) {
		fmt = &frame_fmts[i];
		if (fmt->pixelformat == pixelformat)
			return fmt;
	}

	return NULL;
}

static void register_encoder(struct hva_device *hva,
			     const struct hva_encoder *enc)
{
	if (hva->nb_of_encoders >= HVA_MAX_ENCODERS) {
		dev_warn(hva->dev,
			 "%s can' t register encoder (max nb (%d) is reached!)\n",
			 enc->name, HVA_MAX_ENCODERS);
		return;
	}

	/* those encoder ops are mandatory */
	WARN_ON(!enc->open);
	WARN_ON(!enc->close);
	WARN_ON(!enc->encode);

	hva->encoders[hva->nb_of_encoders] = enc;
	hva->nb_of_encoders++;
	dev_info(hva->dev, "%s encoder registered\n", enc->name);
}

static void register_all(struct hva_device *hva)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hva_encoders); i++)
		register_encoder(hva, hva_encoders[i]);
}

static int hva_open_encoder(struct hva_ctx *ctx, u32 streamformat,
			    u32 pixelformat, struct hva_encoder **penc)
{
	struct hva_device *hva = ctx_to_hdev(ctx);
	struct device *dev = ctx_to_dev(ctx);
	struct hva_encoder *enc;
	unsigned int i;
	int ret;

	/* find an encoder which can deal with these formats */
	for (i = 0; i < hva->nb_of_encoders; i++) {
		enc = (struct hva_encoder *)hva->encoders[i];
		if ((enc->streamformat == streamformat) &&
		    (enc->pixelformat == pixelformat))
			break;	/* found */
	}

	if (i == hva->nb_of_encoders) {
		dev_err(dev, "%s no encoder found matching %4.4s => %4.4s\n",
			ctx->name, (char *)pixelformat, (char *)streamformat);
		return -EINVAL;
	}

	dev_dbg(dev, "%s one encoder matching %4.4s => %4.4s\n",
		ctx->name, (char *)&pixelformat, (char *)&streamformat);

	/* update name instance */
	snprintf(ctx->name, sizeof(ctx->name), "[%3d:%4.4s]",
		 hva->instance_id, (char *)&streamformat);

	/* open encoder instance */
	ret = enc->open(ctx);
	if (ret) {
		dev_err(hva->dev, "%s enc->open failed (%d)\n",
			ctx->name, ret);
		return ret;
	}

	*penc = enc;

	return ret;
}

/* v4l2 ioctl operations */

static int hva_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct hva_device *hva = ctx_to_hdev(ctx);

	strlcpy(cap->driver, hva->pdev->name, sizeof(cap->driver));
	strlcpy(cap->card, hva->pdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 HVA_NAME);

	cap->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int hva_enum_fmt_frame(struct file *file, void *priv,
			      struct v4l2_fmtdesc *f)
{
	/* index don't have to exceed number of  format supported */
	if (f->index >=  ARRAY_SIZE(frame_fmts))
		return -EINVAL;

	/* pixel format */
	f->pixelformat = frame_fmts[f->index].pixelformat;

	return 0;
}

static int hva_enum_fmt_stream(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	/* index don't have to exceed number of stream format supported */
	if (f->index >= ARRAY_SIZE(stream_fmt))
		return -EINVAL;

	/* pixel format */
	f->pixelformat = stream_fmt[f->index];

	/* compressed */
	f->flags = V4L2_FMT_FLAG_COMPRESSED;

	return 0;
}

static int hva_try_fmt_stream(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct hva_device *hva = ctx_to_hdev(ctx);
	struct device *dev = ctx_to_dev(ctx);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct hva_encoder *enc = NULL;
	unsigned int i;

	if ((f->fmt.pix.width == 0) || (f->fmt.pix.height == 0))
		goto out;

	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.priv = 0;

	for (i = 0; i < hva->nb_of_encoders; i++) {
		enc = hva->encoders[i];
		if (enc->streamformat == pix->pixelformat)
			if ((f->fmt.pix.height * f->fmt.pix.width) <=
			    (enc->max_width * enc->max_height))
				return 0;
	}
out:
	dev_dbg(dev, "%s stream format or resolution %dx%d not supported\n",
		ctx->name, f->fmt.pix.width, f->fmt.pix.height);
	return -EINVAL;
}

static int hva_try_fmt_frame(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct hva_frame_fmt *format;
	u32 in_w, in_h;

	format = hva_find_frame_fmt(pix->pixelformat);
	if (!format) {
		dev_dbg(dev, "%s Unknown format 0x%x\n", ctx->name,
			pix->pixelformat);
		return -EINVAL;
	}

	/* adjust width & height */
	in_w = pix->width;
	in_h = pix->height;
	v4l_bound_align_image(&pix->width,
			      HVA_MIN_WIDTH, HVA_MAX_WIDTH,
			      ffs(format->w_align) - 1,
			      &pix->height,
			      HVA_MIN_HEIGHT, HVA_MAX_HEIGHT,
			      ffs(format->h_align) - 1,
			      0);

	if ((pix->width != in_w) || (pix->height != in_h))
		dev_dbg(dev, "%s size updated: %dx%d -> %dx%d\n", ctx->name,
			in_w, in_h, pix->width, pix->height);

	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.priv = 0;

	return 0;
}

static int hva_s_fmt_stream(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	int ret;

	dev_dbg(dev, "%s %s %dx%d fmt:%.4s size:%d\n",
		ctx->name, __func__, f->fmt.pix.width, f->fmt.pix.height,
		(u8 *)&f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

	ret = hva_try_fmt_stream(file, fh, f);
	if (ret) {
		dev_err(dev,
			"%s %s %.4s format or %dx%d resolution not supported\n",
			ctx->name, __func__, (char *)&f->fmt.pix.pixelformat,
			f->fmt.pix.width, f->fmt.pix.height);
		return ret;
	}

	/* update context */
	ctx->streaminfo.width = f->fmt.pix.width;
	ctx->streaminfo.height = f->fmt.pix.height;
	ctx->streaminfo.streamformat = f->fmt.pix.pixelformat;
	ctx->streaminfo.dpb = 1;
	ctx->flags |= HVA_FLAG_STREAMINFO;

	if ((!ctx->encoder) && (ctx->flags & HVA_FLAG_FRAMEINFO)) {
		ret = hva_open_encoder(ctx,
				       ctx->streaminfo.streamformat,
				       ctx->frameinfo.fmt.pixelformat,
				       &ctx->encoder);
		if (ret)
			return ret;
	}

	return 0;
}

static int hva_s_fmt_frame(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct hva_frame_fmt *fmt;
	int ret = 0;

	dev_dbg(dev, "%s %s %dx%d fmt:%.4s size:%d\n",
		ctx->name, __func__, f->fmt.pix.width, f->fmt.pix.height,
		(u8 *)&f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

	ret = hva_try_fmt_frame(file, fh, f);
	if (ret) {
		dev_err(dev,
			"%s %s %.4s format or %dx%d resolution not supported\n",
			ctx->name, __func__, (char *)&f->fmt.pix.pixelformat,
			f->fmt.pix.width, f->fmt.pix.height);
		return ret;
	}

	fmt = hva_find_frame_fmt(pix->pixelformat);
	if (!fmt) {
		dev_dbg(dev, "%s %s unknown format 0x%x\n", ctx->name,
			ctx->name, pix->pixelformat);
		return -EINVAL;
	}

	memcpy(&ctx->frameinfo.fmt, fmt, sizeof(struct hva_frame_fmt));
	ctx->frameinfo.frame_width = ALIGN(pix->width, 16);
	ctx->frameinfo.frame_height = ALIGN(pix->height, 16);
	ctx->frameinfo.width = pix->width;
	ctx->frameinfo.height = pix->height;
	ctx->frameinfo.crop.width = pix->width;
	ctx->frameinfo.crop.height = pix->height;
	ctx->frameinfo.crop.left = 0;
	ctx->frameinfo.crop.top = 0;

	ctx->flags |= HVA_FLAG_FRAMEINFO;

	if ((!ctx->encoder) && (ctx->flags & HVA_FLAG_STREAMINFO))
		ret = hva_open_encoder(ctx,
				       ctx->streaminfo.streamformat,
				       ctx->frameinfo.fmt.pixelformat,
				       &ctx->encoder);

	return ret;
}

static int hva_s_ext_ctrls(struct file *file, void *fh,
			   struct v4l2_ext_controls *ctrls)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	unsigned int i;

	dev_dbg(dev, "%s %s count controls %d\n", ctx->name, __func__,
		ctrls->count);

	for (i = 0; i < ctrls->count; i++) {
		switch (ctrls->controls[i].id) {
		case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
			ctx->ctrls.gop_size = ctrls->controls[i].value;
			dev_dbg(dev, "%s V4L2_CID_MPEG_VIDEO_GOP_SIZE %d\n",
				ctx->name, ctrls->controls[i].value);
			break;
		case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
			ctx->ctrls.bitrate_mode = ctrls->controls[i].value;
			dev_dbg(dev, "%s V4L2_CID_MPEG_VIDEO_BITRATE_MODE %d\n",
				ctx->name, ctrls->controls[i].value);
			break;
		case V4L2_CID_MPEG_VIDEO_BITRATE:
			ctx->ctrls.bitrate = ctrls->controls[i].value;
			dev_dbg(dev, "%s V4L2_CID_MPEG_VIDEO_BITRATE %d\n",
				ctx->name, ctrls->controls[i].value);
			break;
		case V4L2_CID_MPEG_VIDEO_CYCLIC_INTRA_REFRESH_MB:
			ctx->ctrls.intra_refresh = ctrls->controls[i].value;
			dev_dbg(dev,
				"%s V4L2_CID_MPEG_VIDEO_CYCLIC_INTRA_REFRESH_MB %d\n",
				ctx->name, ctrls->controls[i].value);
			break;
		case V4L2_CID_MPEG_VIDEO_ASPECT:
			/* only one video aspect ratio supported (1/1) */
			switch (ctrls->controls[i].value) {
			case V4L2_MPEG_VIDEO_ASPECT_1x1:
				dev_dbg(dev,
					"%s V4L2_CID_MPEG_VIDEO_ASPECT 1x1\n",
					ctx->name);
				break;
			case V4L2_MPEG_VIDEO_ASPECT_4x3:
			case V4L2_MPEG_VIDEO_ASPECT_16x9:
			case V4L2_MPEG_VIDEO_ASPECT_221x100:
			default:
				dev_err(dev,
					"%s V4L2_CID_MPEG_VIDEO_ASPECT: Unsupported aspect ratio %d\n",
					ctx->name, ctrls->controls[i].value);
				return -EINVAL;
			}
			break;
		default:
			dev_err(dev,
				"%s VIDIOC_S_EXT_CTRLS(): Unsupported control id %d\n",
				ctx->name, ctrls->controls[i].id);
			return -EINVAL;
		}
	}

	return 0;
}

static int hva_s_parm(struct file *file, void *fh, struct v4l2_streamparm *sp)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);

	ctx->time_per_frame.numerator = sp->parm.capture.timeperframe.numerator;
	ctx->time_per_frame.denominator =
	    sp->parm.capture.timeperframe.denominator;

	dev_dbg(dev, "%s set parameters %d/%d\n",
		ctx->name, ctx->time_per_frame.numerator,
		ctx->time_per_frame.denominator);

	return 0;
}

static int hva_g_parm(struct file *file, void *fh, struct v4l2_streamparm *sp)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);

	sp->parm.capture.timeperframe.numerator = ctx->time_per_frame.numerator;
	sp->parm.capture.timeperframe.denominator =
	    ctx->time_per_frame.denominator;

	dev_dbg(dev, "%s get parameters %d/%d\n",
		ctx->name, ctx->time_per_frame.numerator,
		ctx->time_per_frame.denominator);

	return 0;
}

static int hva_g_fmt_stream(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);

	/* get stream format */
	f->fmt.pix.width = ctx->streaminfo.width;
	f->fmt.pix.height = ctx->streaminfo.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	/* 32 bytes alignment */
	f->fmt.pix.bytesperline = ALIGN(f->fmt.pix.width, 32);
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.pixelformat = ctx->streaminfo.streamformat;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	dev_dbg(dev, "%s %s %dx%d fmt:%.4s size:%d\n",
		ctx->name, __func__, f->fmt.pix.width, f->fmt.pix.height,
		(u8 *)&f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);
	return 0;
}

static int hva_g_fmt_frame(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct hva_frame_fmt *fmt = &ctx->frameinfo.fmt;
	int width = ctx->frameinfo.frame_width;
	int height = ctx->frameinfo.frame_height;

	/* get source format */
	f->fmt.pix.pixelformat = fmt->pixelformat;
	f->fmt.pix.width = ctx->frameinfo.width;
	f->fmt.pix.height = ctx->frameinfo.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->fmt.pix.bytesperline = (width * fmt->bpp_plane0) / 8;
	f->fmt.pix.sizeimage = (width * height * fmt->bpp) / 8;

	dev_dbg(dev, "%s %s %dx%d fmt:%.4s size:%d\n",
		ctx->name, __func__, f->fmt.pix.width, f->fmt.pix.height,
		(u8 *)&f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

	return 0;
}

static int hva_reqbufs(struct file *file, void *priv,
		       struct v4l2_requestbuffers *reqbufs)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	int ret = 0;

	dev_dbg(dev, "%s %s %s\n", ctx->name, __func__,
		to_type_str(reqbufs->type));

	ret = vb2_reqbufs(get_queue(ctx, reqbufs->type), reqbufs);
	if (ret) {
		dev_err(dev, "%s vb2_reqbufs failed (%d)\n", ctx->name, ret);
		return ret;
	}

	if (reqbufs->count == 0) {
		/*
		 * buffers have been freed in vb2 __reqbufs()
		 * now cleanup "allocation context" ...
		 */
		if (reqbufs->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
			vb2_dma_contig_cleanup_ctx(ctx->q_frame.alloc_ctx[0]);
			ctx->q_frame.alloc_ctx[0] = NULL;
		} else if (reqbufs->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			vb2_dma_contig_cleanup_ctx(ctx->q_stream.alloc_ctx[0]);
			ctx->q_stream.alloc_ctx[0] = NULL;
		}
	}

	return 0;
}

static int hva_create_bufs(struct file *file, void *priv,
			   struct v4l2_create_buffers *create)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct vb2_queue *q = get_queue(ctx, create->format.type);

	return vb2_create_bufs(q, create);
}

static int hva_querybuf(struct file *file, void *priv, struct v4l2_buffer *b)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	int ret = 0;

	dev_dbg(dev, "%s %s %s[%d]\n", ctx->name, __func__,
		to_type_str(b->type), b->index);

	/* vb2 call */
	ret = vb2_querybuf(get_queue(ctx, b->type), b);
	if (ret) {
		dev_err(dev, "%s vb2_querybuf failed (%d)\n", ctx->name, ret);
		return ret;
	}

	/* add an offset to differentiate OUTPUT/CAPTURE @mmap time */
	if ((b->memory == V4L2_MEMORY_MMAP) &&
	    (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
		b->m.offset += MMAP_FRAME_OFFSET;
	}

	return 0;
}

static int hva_expbuf(struct file *file, void *fh, struct v4l2_exportbuffer *b)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	int ret = 0;

	/* request validation */
	if ((b->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) &&
	    (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
		dev_err(dev,
			"%s V4L2 EXPBUF: only type output/cature are supported\n",
			ctx->name);
		return -EINVAL;
	}

	/* vb2 call */
	ret = vb2_expbuf(get_queue(ctx, b->type), b);
	if (ret) {
		dev_err(dev, "%s vb2_expbuf failed (%d)\n", ctx->name, ret);
		return ret;
	}

	return 0;
}

static int hva_qbuf(struct file *file, void *priv, struct v4l2_buffer *b)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct vb2_queue *q = get_queue(ctx, b->type);
	int ret = 0;

	/* copy bytesused field from v4l2 buffer to vb2 buffer */
	if ((b->index < MAX_FRAMES) &&
	    (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE)) {
		struct hva_stream *s = (struct hva_stream *)q->bufs[b->index];

		s->payload = b->bytesused;
	}

	ret = vb2_qbuf(q, b);
	if (ret) {
		dev_err(dev, "%s vb2_qbuf failed (%d)\n", ctx->name, ret);
		return ret;
	}
	return 0;
}

static int hva_dqbuf(struct file *file, void *priv, struct v4l2_buffer *b)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct vb2_queue *q = get_queue(ctx, b->type);
	int ret = 0;

	/* vb2 call */
	ret = vb2_dqbuf(q, b, file->f_flags & O_NONBLOCK);
	if (ret) {
		dev_err(dev, "%s vb2_dqbuf failed (%d)\n", ctx->name, ret);
		return ret;
	}

	dev_dbg(dev, "%s %s %s[%d]\n", ctx->name, __func__,
		to_type_str(b->type), b->index);

	return 0;
}

static int hva_streamon(struct file *file, void *priv, enum v4l2_buf_type type)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	int ret = 0;

	/* reset frame number */
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		ctx->frame_num = 0;

	/* vb2 call */
	ret = vb2_streamon(get_queue(ctx, type), type);
	if (ret) {
		dev_err(dev, "%s vb2_streamon failed (%d)\n", ctx->name, ret);
		return ret;
	}

	return 0;
}

static int hva_streamoff(struct file *file, void *priv, enum v4l2_buf_type type)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct hva_stream *sr, *node;
	int ret = 0;

	/* release all active buffers */
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		list_for_each_entry_safe(sr, node, &ctx->list_stream, list) {
			list_del_init(&sr->list);
			vb2_buffer_done(&sr->v4l2.vb2_buf, VB2_BUF_STATE_ERROR);
		}
	}

	/* vb2 call */
	ret = vb2_streamoff(get_queue(ctx, type), type);
	if (ret) {
		dev_err(dev, "%s vb2_streamoff failed (%d)\n", ctx->name, ret);
		return ret;
	}

	return 0;
}

static int is_rect_enclosed(struct v4l2_rect *a, struct v4l2_rect *b)
{
	/* return 1 if a is enclosed in b, or 0 otherwise. */
	if (a->left < b->left || a->top < b->top)
		return 0;

	if (a->left + a->width > b->left + b->width)
		return 0;

	if (a->top + a->height > b->top + b->height)
		return 0;

	return 1;
}

static int hva_g_selection(struct file *file, void *fh,
			   struct v4l2_selection *s)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		dev_err(dev, "%s %s: G_SELECTION failed, invalid type (%d)\n",
			ctx->name, __func__, s->type);
		return -EINVAL;
	}

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		/* cropped frame */
		s->r = ctx->frameinfo.crop;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* complete frame */
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = ctx->frameinfo.width;
		s->r.height = ctx->frameinfo.height;
		break;
	default:
		dev_err(dev, "%s %s: G_SELECTION failed, invalid target (%d)\n",
			ctx->name, __func__, s->target);
		return -EINVAL;
	}

	return 0;
}

static int hva_s_selection(struct file *file, void *fh,
			   struct v4l2_selection *s)
{
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	struct v4l2_rect *in, out;

	if ((s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) ||
	    (s->target != V4L2_SEL_TGT_CROP)) {
		dev_err(dev, "%s %s: S_SELECTION failed, invalid type (%d)\n",
			ctx->name, __func__, s->type);
		return -EINVAL;
	}

	in = &s->r;
	out = *in;

	/* align and check origin */
	out.left = ALIGN(in->left, ctx->frameinfo.fmt.w_align);
	out.top = ALIGN(in->top, ctx->frameinfo.fmt.h_align);

	if (((out.left + out.width) >  ctx->frameinfo.width) ||
	    ((out.top + out.height) >  ctx->frameinfo.height)) {
		dev_err(dev,
			"%s %s: S_SELECTION failed, invalid crop %dx%d@(%d,%d)\n",
			ctx->name, __func__, out.width, out.height,
			out.left, out.top);
		return -EINVAL;
	}

	/* checks adjust constraints flags */
	if (s->flags & V4L2_SEL_FLAG_LE && !is_rect_enclosed(&out, in))
		return -ERANGE;

	if (s->flags & V4L2_SEL_FLAG_GE && !is_rect_enclosed(in, &out))
		return -ERANGE;

	if ((out.left != in->left) || (out.top != in->top) ||
	    (out.width != in->width) || (out.height != in->height))
		*in = out;

	ctx->frameinfo.crop = s->r;

	return 0;
}

/* vb2 ioctls operations */

static int hva_vb2_frame_queue_setup(struct vb2_queue *q,
				     const void *parg,
				     unsigned int *num_buffers,
				     unsigned int *num_planes,
				     unsigned int sizes[], void *alloc_ctxs[])
{
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct device *dev = ctx_to_dev(ctx);
	int width = ctx->frameinfo.frame_width;
	int height = ctx->frameinfo.frame_height;

	dev_dbg(dev, "%s %s *num_buffers=%d\n", ctx->name, __func__,
		*num_buffers);

	/* only one plane supported */
	*num_planes = 1;

	/* setup nb of input buffers needed =
	 * user need (*num_buffer given, usually for grab pipeline) +
	 * encoder internal need
	 */
	if (*num_buffers < MIN_FRAMES) {
		dev_warn(dev,
			 "%s num_buffers too low (%d), increasing to %d\n",
			 ctx->name, *num_buffers, MIN_FRAMES);
		*num_buffers = MIN_FRAMES;
	}

	if (*num_buffers > MAX_FRAMES) {
		dev_warn(dev,
			 "%s input frame count too high (%d), cut to %d\n",
			 ctx->name, *num_buffers, MAX_FRAMES);
		*num_buffers = MAX_FRAMES;
	}

	if (sizes[0])
		dev_warn(dev, "%s psize[0] already set to %d\n", ctx->name,
			 sizes[0]);

	if (alloc_ctxs[0])
		dev_warn(dev, "%s allocators[0] already set\n", ctx->name);

	if (!(ctx->flags & HVA_FLAG_FRAMEINFO)) {
		dev_err(dev, "%s %s frame format not set, using default format\n",
			ctx->name, __func__);
	}

	sizes[0] = (width * height * ctx->frameinfo.fmt.bpp) / 8;
	alloc_ctxs[0] = vb2_dma_contig_init_ctx(dev);
	/* alloc_ctxs[0] will be freed @ reqbufs(0) or @ release */

	return 0;
}

static int hva_vb2_frame_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct device *dev = ctx_to_dev(ctx);
	struct hva_frame *fm = (struct hva_frame *)vb;

	if (!fm->prepared) {
		/* get memory addresses */
		fm->vaddr = vb2_plane_vaddr(&fm->v4l2.vb2_buf, 0);
		fm->paddr =
			vb2_dma_contig_plane_dma_addr(&fm->v4l2.vb2_buf, 0);
		fm->prepared = true;

		ctx->num_frames++;

		dev_dbg(dev, "%s frame[%d] prepared; virt=%p, phy=0x%x\n",
			ctx->name, vb->index, fm->vaddr,
			fm->paddr);
	}

	return 0;
}

static void hva_vb2_frame_queue(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct device *dev = ctx_to_dev(ctx);
	const struct hva_encoder *enc = ctx_to_enc(ctx);
	struct hva_frame *fm = NULL;
	struct hva_stream *sr = NULL;
	int ret = 0;

	fm = (struct hva_frame *)vb;

	if (!vb2_is_streaming(q)) {
		vb2_buffer_done(&fm->v4l2.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	/* get a free destination buffer */
	if (list_empty(&ctx->list_stream)) {
		dev_err(dev, "%s no free buffer for destination stream!\n",
			ctx->name);
		ctx->sys_errors++;
		goto err;
	}
	sr = list_first_entry(&ctx->list_stream, struct hva_stream, list);

	if (!sr)
		goto err;

	list_del(&sr->list);

	/* encode the frame & get stream unit */
	ret = enc->encode(ctx, fm, sr);
	if (ret)
		goto err;

	/* propagate frame timestamp */
	sr->v4l2.timestamp = fm->v4l2.timestamp;

	ctx->encoded_frames++;

	vb2_buffer_done(&sr->v4l2.vb2_buf, VB2_BUF_STATE_DONE);
	vb2_buffer_done(&fm->v4l2.vb2_buf, VB2_BUF_STATE_DONE);

	return;
err:
	if (sr)
		vb2_buffer_done(&sr->v4l2.vb2_buf, VB2_BUF_STATE_ERROR);

	vb2_buffer_done(&fm->v4l2.vb2_buf, VB2_BUF_STATE_ERROR);
}

static int hva_vb2_stream_queue_setup(struct vb2_queue *q,
				      const void *parg,
				      unsigned int *num_buffers,
				      unsigned int *num_planes,
				      unsigned int sizes[], void *alloc_ctxs[])
{
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct device *dev = ctx_to_dev(ctx);
	int max_buf_size = 0;
	u32 pixelformat;
	int width;
	int height;

	dev_dbg(dev, "%s %s *num_buffers=%d\n", ctx->name, __func__,
		*num_buffers);

	/* only one plane supported */
	*num_planes = 1;

	/* number of buffers must be at least 1 */
	if (*num_buffers < 1)
		*num_buffers = 1;

	if (sizes[0])
		dev_warn(dev, "%s psize[0] already set to %d\n", ctx->name,
			 sizes[0]);

	if (alloc_ctxs[0])
		dev_warn(dev, "%s allocators[0] already set\n", ctx->name);

	if (!(ctx->flags & HVA_FLAG_STREAMINFO)) {
		dev_err(dev, "%s %s stream format not set, using dflt format\n",
			ctx->name, __func__);
	}

	pixelformat = ctx->streaminfo.streamformat;
	width = ctx->streaminfo.width;
	height = ctx->streaminfo.height;

	switch (pixelformat) {
	default:
		dev_err(dev, "%s %s Unknown stream format\n", ctx->name,
			__func__);
	}

	sizes[0] = max_buf_size;
	alloc_ctxs[0] = vb2_dma_contig_init_ctx(dev);	/* free @ release */

	return 0;
}

static int hva_vb2_stream_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct device *dev = ctx_to_dev(ctx);
	struct hva_stream *sr = (struct hva_stream *)vb;

	if (!sr->prepared) {
		/* get memory addresses */
		sr->vaddr = vb2_plane_vaddr(&sr->v4l2.vb2_buf, 0);
		sr->paddr = vb2_dma_contig_plane_dma_addr(&sr->v4l2.vb2_buf, 0);
		sr->prepared = true;

		dev_dbg(dev, "%s stream[%d] prepared; virt=%p, phy=0x%x\n",
			ctx->name, vb->index, sr->vaddr, sr->paddr);
	}

	return 0;
}

static void hva_vb2_stream_queue(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct hva_ctx *ctx = fh_to_ctx(q->drv_priv);
	struct hva_stream *sr = (struct hva_stream *)vb;

	/* check validity of video stream */
	if (vb) {
		/* enqueue to a list destination stream */
		list_add(&sr->list, &ctx->list_stream);
	}
}

static struct vb2_ops hva_vb2_frame_ops = {
	.queue_setup = hva_vb2_frame_queue_setup,
	.buf_prepare = hva_vb2_frame_prepare,
	.buf_queue = hva_vb2_frame_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static struct vb2_ops hva_vb2_stream_ops = {
	.queue_setup = hva_vb2_stream_queue_setup,
	.buf_prepare = hva_vb2_stream_prepare,
	.buf_queue = hva_vb2_stream_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* file basics operations */

static int hva_open(struct file *file)
{
	struct hva_device *hva = video_drvdata(file);
	struct vb2_queue *q;
	struct device *dev;
	struct hva_ctx *ctx;
	int ret = 0;
	unsigned int i;

	WARN_ON(!hva);
	dev = hva->dev;

	mutex_lock(&hva->lock);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&hva->lock);
		return -ENOMEM;
	}

	/* store the context address in the contexts list */
	for (i = 0; i < MAX_CONTEXT; i++) {
		if (!hva->contexts_list[i]) {
			hva->contexts_list[i] = ctx;
			/* save client id in context */
			ctx->client_id = i;
			break;
		}
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	/* recopy device handlers */
	ctx->dev = hva->dev;
	ctx->hdev = hva;

	/* setup vb2 queue for frame input */
	q = &ctx->q_frame;
	q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT; /* to say input, weird! */
	q->io_modes = VB2_MMAP | VB2_DMABUF;

	/* save file handle to private data field of the queue */
	q->drv_priv = &ctx->fh;

	/* overload vb2 buffer size with private struct */
	q->buf_struct_size = sizeof(struct hva_frame);

	q->ops = &hva_vb2_frame_ops;
	q->mem_ops = (struct vb2_mem_ops *)&vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &hva->lock;

	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(dev, "%s [x:x] vb2_queue_init(frame) failed (%d)\n",
			HVA_PREFIX,  ret);
		ctx->sys_errors++;
		goto err_fh_del;
	}

	/* setup vb2 queue of the destination */
	q = &ctx->q_stream;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;

	/* save file handle to private data field of the queue */
	q->drv_priv = &ctx->fh;

	/* overload vb2 buffer size with private struct */
	q->buf_struct_size = sizeof(struct hva_stream);

	q->ops = &hva_vb2_stream_ops;
	q->mem_ops = (struct vb2_mem_ops *)&vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &hva->lock;

	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(dev, "%s [x:x] vb2_queue_init(stream) failed (%d)\n",
			HVA_PREFIX, ret);
		ctx->sys_errors++;
		goto err_queue_del_frame;
	}

	/* initialize the list of stream buffers */
	INIT_LIST_HEAD(&ctx->list_stream);

	/* name this instance */
	hva->instance_id++;	/* rolling id to identify this instance */
	snprintf(ctx->name, sizeof(ctx->name), "[%3d:----]", hva->instance_id);

	/* initialize controls */
	ctx->ctrls.bitrate_mode = DFT_CFG_BITRATE_MODE;
	ctx->ctrls.bitrate = DFT_CFG_BITRATE;
	ctx->ctrls.cpb_size = DFT_CFG_CPB_SIZE;
	ctx->ctrls.gop_size = DFT_CFG_GOP_SIZE;
	ctx->ctrls.intra_refresh = DFT_CFG_INTRA_REFRESH;
	ctx->ctrls.dct8x8 = DFT_CFG_DCT8X8;
	ctx->ctrls.qpmin = DFT_CFG_QPMIN;
	ctx->ctrls.qpmax = DFT_CFG_QPMAX;
	ctx->ctrls.jpeg_comp_quality = DFT_CFG_COMP_QUALITY;
	ctx->ctrls.vui_sar = DFT_CFG_SAR_ENABLE;

	/* set by default time per frame */
	ctx->time_per_frame.numerator = DFT_CFG_FRAME_NUM;
	ctx->time_per_frame.denominator = DFT_CFG_FRAME_DEN;

	/* default format */
	ctx->streaminfo = stream_dflt_fmt;
	ctx->frameinfo = frame_dflt_fmt;

	hva->nb_of_instances++;

	mutex_unlock(&hva->lock);

	return 0;

err_queue_del_frame:
	vb2_queue_release(&ctx->q_frame);
err_fh_del:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	hva->contexts_list[ctx->client_id] = NULL;
	devm_kfree(dev, ctx);

	mutex_unlock(&hva->lock);

	return ret;
}

static int hva_release(struct file *file)
{
	struct hva_device *hva = video_drvdata(file);
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx_to_dev(ctx);
	const struct hva_encoder *enc = ctx_to_enc(ctx);

	mutex_lock(&hva->lock);

	/* free queues: source & destination */
	vb2_queue_release(&ctx->q_frame);
	vb2_queue_release(&ctx->q_stream);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	/* will free dma memory of each frame in queue */
	vb2_queue_release(&ctx->q_frame);
	if (ctx->q_frame.alloc_ctx[0])
		vb2_dma_contig_cleanup_ctx(ctx->q_frame.alloc_ctx[0]);

	/* will free dma memory of each aus in queue */
	vb2_queue_release(&ctx->q_stream);
	if (ctx->q_stream.alloc_ctx[0])
		vb2_dma_contig_cleanup_ctx(ctx->q_stream.alloc_ctx[0]);

	/* clear context in contexts list */
	if ((ctx->client_id >= MAX_CONTEXT) ||
	    (hva->contexts_list[ctx->client_id] != ctx)) {
		dev_err(dev, "%s can't clear context in contexts list!\n",
			ctx->name);
		ctx->sys_errors++;
	}
	hva->contexts_list[ctx->client_id] = NULL;

	/* close encoder */
	if (enc)
		enc->close(ctx);

	devm_kfree(dev, ctx);

	hva->nb_of_instances--;

	mutex_unlock(&hva->lock);

	return 0;
}

static int hva_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct hva_device *hva = video_drvdata(file);
	struct hva_ctx *ctx = fh_to_ctx(file->private_data);
	struct device *dev = ctx->dev;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	enum v4l2_buf_type type;
	int ret;

	mutex_lock(&hva->lock);

	/* offset used to differentiate OUTPUT/CAPTURE */
	if (offset < MMAP_FRAME_OFFSET) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	} else {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vma->vm_pgoff -= (MMAP_FRAME_OFFSET >> PAGE_SHIFT);
	}

	/* vb2 call */
	ret = vb2_mmap(get_queue(ctx, type), vma);
	if (ret) {
		dev_err(dev, "%s vb2_mmap failed (%d)\n", ctx->name, ret);
		ctx->sys_errors++;
		mutex_unlock(&hva->lock);
		return ret;
	}

	mutex_unlock(&hva->lock);

	return 0;
}

/* v4l2 ops */
static const struct v4l2_file_operations hva_fops = {
	.owner = THIS_MODULE,
	.open = hva_open,
	.release = hva_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = hva_mmap,
};

/* v4l2 ioctl ops */
static const struct v4l2_ioctl_ops hva_ioctl_ops = {
	.vidioc_querycap = hva_querycap,
	/* formats ioctl */
	.vidioc_enum_fmt_vid_cap	= hva_enum_fmt_stream,
	.vidioc_enum_fmt_vid_out	= hva_enum_fmt_frame,
	.vidioc_g_fmt_vid_cap		= hva_g_fmt_stream,
	.vidioc_g_fmt_vid_out		= hva_g_fmt_frame,
	.vidioc_try_fmt_vid_cap		= hva_try_fmt_stream,
	.vidioc_try_fmt_vid_out		= hva_try_fmt_frame,
	.vidioc_s_fmt_vid_cap		= hva_s_fmt_stream,
	.vidioc_s_fmt_vid_out		= hva_s_fmt_frame,
	.vidioc_s_ext_ctrls		= hva_s_ext_ctrls,
	.vidioc_g_parm			= hva_g_parm,
	.vidioc_s_parm			= hva_s_parm,
	/* buffers ioctls */
	.vidioc_reqbufs			= hva_reqbufs,
	.vidioc_create_bufs             = hva_create_bufs,
	.vidioc_querybuf		= hva_querybuf,
	.vidioc_expbuf			= hva_expbuf,
	.vidioc_qbuf			= hva_qbuf,
	.vidioc_dqbuf			= hva_dqbuf,
	/* stream ioctls */
	.vidioc_streamon		= hva_streamon,
	.vidioc_streamoff		= hva_streamoff,
	.vidioc_g_selection		= hva_g_selection,
	.vidioc_s_selection		= hva_s_selection,
};

static int hva_probe(struct platform_device *pdev)
{
	struct hva_device *hva;
	struct device *dev = &pdev->dev;
	struct video_device *vdev;
	int ret;

	hva = devm_kzalloc(dev, sizeof(*hva), GFP_KERNEL);
	if (!hva) {
		ret = -ENOMEM;
		goto err;
	}

	hva->dev = dev;
	hva->pdev = pdev;
	platform_set_drvdata(pdev, hva);

	mutex_init(&hva->lock);

	/* probe hardware */
	ret = hva_hw_probe(pdev, hva);
	if (ret)
		goto err;

	/* register all available encoders */
	register_all(hva);

	/* register on V4L2 */
	ret = v4l2_device_register(dev, &hva->v4l2_dev);
	if (ret) {
		dev_err(dev, "%s %s could not register v4l2 device\n",
			HVA_PREFIX, HVA_NAME);
		goto err_hw_remove;
	}

	vdev = video_device_alloc();
	vdev->fops = &hva_fops;
	vdev->ioctl_ops = &hva_ioctl_ops;
	vdev->release = video_device_release;
	vdev->lock = &hva->lock;
	vdev->v4l2_dev = &hva->v4l2_dev;
	snprintf(vdev->name, sizeof(vdev->name), "%s", HVA_NAME);
	vdev->vfl_dir = VFL_DIR_M2M;
	ret = video_register_device(vdev, VFL_TYPE_GRABBER, 0);
	if (ret) {
		dev_err(dev, "%s %s failed to register video device\n",
			HVA_PREFIX, HVA_NAME);
		goto err_vdev_release;
	}

	hva->vdev = vdev;
	video_set_drvdata(vdev, hva);

	dev_info(dev, "%s %s registered as /dev/video%d\n", HVA_PREFIX,
		 HVA_NAME, vdev->num);

	dev_info(dev, "%s %s esram reserved for address: %p size:%d\n",
		 HVA_PREFIX, HVA_NAME, (void *)hva->esram_addr,
		 hva->esram_size);

	return 0;

err_vdev_release:
	video_device_release(vdev);

err_hw_remove:
	hva_hw_remove(hva);

err:
	return ret;
}

static int hva_remove(struct platform_device *pdev)
{
	struct hva_device *hva = platform_get_drvdata(pdev);
	struct device *dev = hva_to_dev(hva);

	dev_info(dev, "%s removing %s\n", HVA_PREFIX, pdev->name);

	hva_hw_remove(hva);

	video_unregister_device(hva->vdev);
	v4l2_device_unregister(&hva->v4l2_dev);

	return 0;
}

static const struct dev_pm_ops hva_pm_ops = {
	.runtime_suspend = hva_hw_runtime_suspend,
	.runtime_resume = hva_hw_runtime_resume,
};

static const struct of_device_id hva_match_types[] = {
	{
	 .compatible = "st,stih407-hva",
	},
	{ /* end node */ }
};

MODULE_DEVICE_TABLE(of, hva_match_types);

struct platform_driver hva_driver = {
	.probe  = hva_probe,
	.remove = hva_remove,
	.driver = {
		.name           = HVA_NAME,
		.owner          = THIS_MODULE,
		.of_match_table = hva_match_types,
		.pm             = &hva_pm_ops,
		},
};

module_platform_driver(hva_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yannick Fertre <yannick.fertre@st.com>");
MODULE_DESCRIPTION("HVA video encoder V4L2 driver");
