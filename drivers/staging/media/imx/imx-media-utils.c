/*
 * V4L2 Media Controller Driver for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/module.h>
#include "imx-media.h"

/*
 * List of supported pixel formats for the subdevs.
 *
 * In all of these tables, the non-mbus formats (with no
 * mbus codes) must all fall at the end of the table.
 */

static const struct imx_media_pixfmt yuv_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_UYVY,
		.codes  = {
			MEDIA_BUS_FMT_UYVY8_2X8,
			MEDIA_BUS_FMT_UYVY8_1X16
		},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
	}, {
		.fourcc	= V4L2_PIX_FMT_YUYV,
		.codes  = {
			MEDIA_BUS_FMT_YUYV8_2X8,
			MEDIA_BUS_FMT_YUYV8_1X16
		},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
	},
	/***
	 * non-mbus YUV formats start here. NOTE! when adding non-mbus
	 * formats, NUM_NON_MBUS_YUV_FORMATS must be updated below.
	 ***/
	{
		.fourcc	= V4L2_PIX_FMT_YUV420,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU420,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
		.planar = true,
	},
};

#define NUM_NON_MBUS_YUV_FORMATS 5
#define NUM_YUV_FORMATS ARRAY_SIZE(yuv_formats)
#define NUM_MBUS_YUV_FORMATS (NUM_YUV_FORMATS - NUM_NON_MBUS_YUV_FORMATS)

static const struct imx_media_pixfmt rgb_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_RGB565,
		.codes  = {MEDIA_BUS_FMT_RGB565_2X8_LE},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
	}, {
		.fourcc	= V4L2_PIX_FMT_RGB24,
		.codes  = {
			MEDIA_BUS_FMT_RGB888_1X24,
			MEDIA_BUS_FMT_RGB888_2X12_LE
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 24,
	}, {
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.codes  = {MEDIA_BUS_FMT_ARGB8888_1X32},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
		.ipufmt = true,
	},
	/*** raw bayer formats start here ***/
	{
		.fourcc = V4L2_PIX_FMT_SBGGR8,
		.codes  = {MEDIA_BUS_FMT_SBGGR8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGBRG8,
		.codes  = {MEDIA_BUS_FMT_SGBRG8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGRBG8,
		.codes  = {MEDIA_BUS_FMT_SGRBG8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SRGGB8,
		.codes  = {MEDIA_BUS_FMT_SRGGB8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SBGGR16,
		.codes  = {
			MEDIA_BUS_FMT_SBGGR10_1X10,
			MEDIA_BUS_FMT_SBGGR12_1X12,
			MEDIA_BUS_FMT_SBGGR14_1X14,
			MEDIA_BUS_FMT_SBGGR16_1X16
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGBRG16,
		.codes  = {
			MEDIA_BUS_FMT_SGBRG10_1X10,
			MEDIA_BUS_FMT_SGBRG12_1X12,
			MEDIA_BUS_FMT_SGBRG14_1X14,
			MEDIA_BUS_FMT_SGBRG16_1X16,
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGRBG16,
		.codes  = {
			MEDIA_BUS_FMT_SGRBG10_1X10,
			MEDIA_BUS_FMT_SGRBG12_1X12,
			MEDIA_BUS_FMT_SGRBG14_1X14,
			MEDIA_BUS_FMT_SGRBG16_1X16,
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SRGGB16,
		.codes  = {
			MEDIA_BUS_FMT_SRGGB10_1X10,
			MEDIA_BUS_FMT_SRGGB12_1X12,
			MEDIA_BUS_FMT_SRGGB14_1X14,
			MEDIA_BUS_FMT_SRGGB16_1X16,
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
		.bayer  = true,
	},
	/***
	 * non-mbus RGB formats start here. NOTE! when adding non-mbus
	 * formats, NUM_NON_MBUS_RGB_FORMATS must be updated below.
	 ***/
	{
		.fourcc	= V4L2_PIX_FMT_BGR24,
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 24,
	}, {
		.fourcc	= V4L2_PIX_FMT_BGR32,
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
	},
};

#define NUM_NON_MBUS_RGB_FORMATS 2
#define NUM_RGB_FORMATS ARRAY_SIZE(rgb_formats)
#define NUM_MBUS_RGB_FORMATS (NUM_RGB_FORMATS - NUM_NON_MBUS_RGB_FORMATS)

static const struct imx_media_pixfmt ipu_yuv_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV32,
		.codes  = {MEDIA_BUS_FMT_AYUV8_1X32},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 32,
		.ipufmt = true,
	},
};

#define NUM_IPU_YUV_FORMATS ARRAY_SIZE(ipu_yuv_formats)

static const struct imx_media_pixfmt ipu_rgb_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.codes  = {MEDIA_BUS_FMT_ARGB8888_1X32},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
		.ipufmt = true,
	},
};

#define NUM_IPU_RGB_FORMATS ARRAY_SIZE(ipu_rgb_formats)

static inline u32 pixfmt_to_colorspace(const struct imx_media_pixfmt *fmt)
{
	return (fmt->cs == IPUV3_COLORSPACE_RGB) ?
		V4L2_COLORSPACE_SRGB : V4L2_COLORSPACE_SMPTE170M;
}

static const struct imx_media_pixfmt *find_format(u32 fourcc,
						  u32 code,
						  enum codespace_sel cs_sel,
						  bool allow_non_mbus,
						  bool allow_bayer)
{
	const struct imx_media_pixfmt *array, *fmt, *ret = NULL;
	u32 array_size;
	int i, j;

	switch (cs_sel) {
	case CS_SEL_YUV:
		array_size = NUM_YUV_FORMATS;
		array = yuv_formats;
		break;
	case CS_SEL_RGB:
		array_size = NUM_RGB_FORMATS;
		array = rgb_formats;
		break;
	case CS_SEL_ANY:
		array_size = NUM_YUV_FORMATS + NUM_RGB_FORMATS;
		array = yuv_formats;
		break;
	default:
		return NULL;
	}

	for (i = 0; i < array_size; i++) {
		if (cs_sel == CS_SEL_ANY && i >= NUM_YUV_FORMATS)
			fmt = &rgb_formats[i - NUM_YUV_FORMATS];
		else
			fmt = &array[i];

		if ((!allow_non_mbus && fmt->codes[0] == 0) ||
		    (!allow_bayer && fmt->bayer))
			continue;

		if (fourcc && fmt->fourcc == fourcc) {
			ret = fmt;
			goto out;
		}

		for (j = 0; code && fmt->codes[j]; j++) {
			if (code == fmt->codes[j]) {
				ret = fmt;
				goto out;
			}
		}
	}

out:
	return ret;
}

static int enum_format(u32 *fourcc, u32 *code, u32 index,
		       enum codespace_sel cs_sel,
		       bool allow_non_mbus,
		       bool allow_bayer)
{
	const struct imx_media_pixfmt *fmt;
	u32 mbus_yuv_sz = NUM_MBUS_YUV_FORMATS;
	u32 mbus_rgb_sz = NUM_MBUS_RGB_FORMATS;
	u32 yuv_sz = NUM_YUV_FORMATS;
	u32 rgb_sz = NUM_RGB_FORMATS;

	switch (cs_sel) {
	case CS_SEL_YUV:
		if (index >= yuv_sz ||
		    (!allow_non_mbus && index >= mbus_yuv_sz))
			return -EINVAL;
		fmt = &yuv_formats[index];
		break;
	case CS_SEL_RGB:
		if (index >= rgb_sz ||
		    (!allow_non_mbus && index >= mbus_rgb_sz))
			return -EINVAL;
		fmt = &rgb_formats[index];
		if (!allow_bayer && fmt->bayer)
			return -EINVAL;
		break;
	case CS_SEL_ANY:
		if (!allow_non_mbus) {
			if (index >= mbus_yuv_sz) {
				index -= mbus_yuv_sz;
				if (index >= mbus_rgb_sz)
					return -EINVAL;
				fmt = &rgb_formats[index];
				if (!allow_bayer && fmt->bayer)
					return -EINVAL;
			} else {
				fmt = &yuv_formats[index];
			}
		} else {
			if (index >= yuv_sz + rgb_sz)
				return -EINVAL;
			if (index >= yuv_sz) {
				fmt = &rgb_formats[index - yuv_sz];
				if (!allow_bayer && fmt->bayer)
					return -EINVAL;
			} else {
				fmt = &yuv_formats[index];
			}
		}
		break;
	default:
		return -EINVAL;
	}

	if (fourcc)
		*fourcc = fmt->fourcc;
	if (code)
		*code = fmt->codes[0];

	return 0;
}

const struct imx_media_pixfmt *
imx_media_find_format(u32 fourcc, enum codespace_sel cs_sel)
{
	return find_format(fourcc, 0, cs_sel, true, false);
}
EXPORT_SYMBOL_GPL(imx_media_find_format);

int imx_media_enum_format(u32 *fourcc, u32 index, enum codespace_sel cs_sel)
{
	return enum_format(fourcc, NULL, index, cs_sel, true, false);
}
EXPORT_SYMBOL_GPL(imx_media_enum_format);

const struct imx_media_pixfmt *
imx_media_find_mbus_format(u32 code, enum codespace_sel cs_sel,
			   bool allow_bayer)
{
	return find_format(0, code, cs_sel, false, allow_bayer);
}
EXPORT_SYMBOL_GPL(imx_media_find_mbus_format);

int imx_media_enum_mbus_format(u32 *code, u32 index, enum codespace_sel cs_sel,
			       bool allow_bayer)
{
	return enum_format(NULL, code, index, cs_sel, false, allow_bayer);
}
EXPORT_SYMBOL_GPL(imx_media_enum_mbus_format);

const struct imx_media_pixfmt *
imx_media_find_ipu_format(u32 code, enum codespace_sel cs_sel)
{
	const struct imx_media_pixfmt *array, *fmt, *ret = NULL;
	u32 array_size;
	int i, j;

	switch (cs_sel) {
	case CS_SEL_YUV:
		array_size = NUM_IPU_YUV_FORMATS;
		array = ipu_yuv_formats;
		break;
	case CS_SEL_RGB:
		array_size = NUM_IPU_RGB_FORMATS;
		array = ipu_rgb_formats;
		break;
	case CS_SEL_ANY:
		array_size = NUM_IPU_YUV_FORMATS + NUM_IPU_RGB_FORMATS;
		array = ipu_yuv_formats;
		break;
	default:
		return NULL;
	}

	for (i = 0; i < array_size; i++) {
		if (cs_sel == CS_SEL_ANY && i >= NUM_IPU_YUV_FORMATS)
			fmt = &ipu_rgb_formats[i - NUM_IPU_YUV_FORMATS];
		else
			fmt = &array[i];

		for (j = 0; code && fmt->codes[j]; j++) {
			if (code == fmt->codes[j]) {
				ret = fmt;
				goto out;
			}
		}
	}

out:
	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_find_ipu_format);

int imx_media_enum_ipu_format(u32 *code, u32 index, enum codespace_sel cs_sel)
{
	switch (cs_sel) {
	case CS_SEL_YUV:
		if (index >= NUM_IPU_YUV_FORMATS)
			return -EINVAL;
		*code = ipu_yuv_formats[index].codes[0];
		break;
	case CS_SEL_RGB:
		if (index >= NUM_IPU_RGB_FORMATS)
			return -EINVAL;
		*code = ipu_rgb_formats[index].codes[0];
		break;
	case CS_SEL_ANY:
		if (index >= NUM_IPU_YUV_FORMATS + NUM_IPU_RGB_FORMATS)
			return -EINVAL;
		if (index >= NUM_IPU_YUV_FORMATS) {
			index -= NUM_IPU_YUV_FORMATS;
			*code = ipu_rgb_formats[index].codes[0];
		} else {
			*code = ipu_yuv_formats[index].codes[0];
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_enum_ipu_format);

int imx_media_init_mbus_fmt(struct v4l2_mbus_framefmt *mbus,
			    u32 width, u32 height, u32 code, u32 field,
			    const struct imx_media_pixfmt **cc)
{
	const struct imx_media_pixfmt *lcc;

	mbus->width = width;
	mbus->height = height;
	mbus->field = field;
	if (code == 0)
		imx_media_enum_mbus_format(&code, 0, CS_SEL_YUV, false);
	lcc = imx_media_find_mbus_format(code, CS_SEL_ANY, false);
	if (!lcc) {
		lcc = imx_media_find_ipu_format(code, CS_SEL_ANY);
		if (!lcc)
			return -EINVAL;
	}

	mbus->code = code;
	mbus->colorspace = pixfmt_to_colorspace(lcc);

	if (cc)
		*cc = lcc;

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_init_mbus_fmt);

int imx_media_mbus_fmt_to_pix_fmt(struct v4l2_pix_format *pix,
				  struct v4l2_mbus_framefmt *mbus,
				  const struct imx_media_pixfmt *cc)
{
	u32 stride;

	if (!cc) {
		cc = imx_media_find_ipu_format(mbus->code, CS_SEL_ANY);
		if (!cc)
			cc = imx_media_find_mbus_format(mbus->code, CS_SEL_ANY,
							true);
		if (!cc)
			return -EINVAL;
	}

	/*
	 * TODO: the IPU currently does not support the AYUV32 format,
	 * so until it does convert to a supported YUV format.
	 */
	if (cc->ipufmt && cc->cs == IPUV3_COLORSPACE_YUV) {
		u32 code;

		imx_media_enum_mbus_format(&code, 0, CS_SEL_YUV, false);
		cc = imx_media_find_mbus_format(code, CS_SEL_YUV, false);
	}

	stride = cc->planar ? mbus->width : (mbus->width * cc->bpp) >> 3;

	pix->width = mbus->width;
	pix->height = mbus->height;
	pix->pixelformat = cc->fourcc;
	pix->colorspace = mbus->colorspace;
	pix->xfer_func = mbus->xfer_func;
	pix->ycbcr_enc = mbus->ycbcr_enc;
	pix->quantization = mbus->quantization;
	pix->field = mbus->field;
	pix->bytesperline = stride;
	pix->sizeimage = (pix->width * pix->height * cc->bpp) >> 3;

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_mbus_fmt_to_pix_fmt);

int imx_media_mbus_fmt_to_ipu_image(struct ipu_image *image,
				    struct v4l2_mbus_framefmt *mbus)
{
	int ret;

	memset(image, 0, sizeof(*image));

	ret = imx_media_mbus_fmt_to_pix_fmt(&image->pix, mbus, NULL);
	if (ret)
		return ret;

	image->rect.width = mbus->width;
	image->rect.height = mbus->height;

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_mbus_fmt_to_ipu_image);

int imx_media_ipu_image_to_mbus_fmt(struct v4l2_mbus_framefmt *mbus,
				    struct ipu_image *image)
{
	const struct imx_media_pixfmt *fmt;

	fmt = imx_media_find_format(image->pix.pixelformat, CS_SEL_ANY);
	if (!fmt)
		return -EINVAL;

	memset(mbus, 0, sizeof(*mbus));
	mbus->width = image->pix.width;
	mbus->height = image->pix.height;
	mbus->code = fmt->codes[0];
	mbus->colorspace = pixfmt_to_colorspace(fmt);
	mbus->field = image->pix.field;

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_ipu_image_to_mbus_fmt);

void imx_media_free_dma_buf(struct imx_media_dev *imxmd,
			    struct imx_media_dma_buf *buf)
{
	if (buf->virt)
		dma_free_coherent(imxmd->md.dev, buf->len,
				  buf->virt, buf->phys);

	buf->virt = NULL;
	buf->phys = 0;
}
EXPORT_SYMBOL_GPL(imx_media_free_dma_buf);

int imx_media_alloc_dma_buf(struct imx_media_dev *imxmd,
			    struct imx_media_dma_buf *buf,
			    int size)
{
	imx_media_free_dma_buf(imxmd, buf);

	buf->len = PAGE_ALIGN(size);
	buf->virt = dma_alloc_coherent(imxmd->md.dev, buf->len, &buf->phys,
				       GFP_DMA | GFP_KERNEL);
	if (!buf->virt) {
		dev_err(imxmd->md.dev, "failed to alloc dma buffer\n");
		return -ENOMEM;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(imx_media_alloc_dma_buf);

/* form a subdev name given a group id and ipu id */
void imx_media_grp_id_to_sd_name(char *sd_name, int sz, u32 grp_id, int ipu_id)
{
	int id;

	switch (grp_id) {
	case IMX_MEDIA_GRP_ID_CSI0...IMX_MEDIA_GRP_ID_CSI1:
		id = (grp_id >> IMX_MEDIA_GRP_ID_CSI_BIT) - 1;
		snprintf(sd_name, sz, "ipu%d_csi%d", ipu_id + 1, id);
		break;
	case IMX_MEDIA_GRP_ID_VDIC:
		snprintf(sd_name, sz, "ipu%d_vdic", ipu_id + 1);
		break;
	case IMX_MEDIA_GRP_ID_IC_PRP:
		snprintf(sd_name, sz, "ipu%d_ic_prp", ipu_id + 1);
		break;
	case IMX_MEDIA_GRP_ID_IC_PRPENC:
		snprintf(sd_name, sz, "ipu%d_ic_prpenc", ipu_id + 1);
		break;
	case IMX_MEDIA_GRP_ID_IC_PRPVF:
		snprintf(sd_name, sz, "ipu%d_ic_prpvf", ipu_id + 1);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(imx_media_grp_id_to_sd_name);

struct imx_media_subdev *
imx_media_find_subdev_by_sd(struct imx_media_dev *imxmd,
			    struct v4l2_subdev *sd)
{
	struct imx_media_subdev *imxsd;
	int i;

	for (i = 0; i < imxmd->num_subdevs; i++) {
		imxsd = &imxmd->subdev[i];
		if (sd == imxsd->sd)
			return imxsd;
	}

	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(imx_media_find_subdev_by_sd);

struct imx_media_subdev *
imx_media_find_subdev_by_id(struct imx_media_dev *imxmd, u32 grp_id)
{
	struct imx_media_subdev *imxsd;
	int i;

	for (i = 0; i < imxmd->num_subdevs; i++) {
		imxsd = &imxmd->subdev[i];
		if (imxsd->sd && imxsd->sd->grp_id == grp_id)
			return imxsd;
	}

	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(imx_media_find_subdev_by_id);

/*
 * Search for an entity in the current pipeline, either a subdev
 * with given grp_id, or a video device if vdev is true.
 * Called with mdev->graph_mutex held.
 */
static struct media_entity *
find_pipeline_entity(struct imx_media_dev *imxmd,
		     struct media_graph *graph,
		     struct media_entity *start_entity,
		     u32 grp_id, bool vdev)
{
	struct media_entity *entity;
	struct v4l2_subdev *sd;

	media_graph_walk_start(graph, start_entity);

	while ((entity = media_graph_walk_next(graph))) {
		if ((vdev && !is_media_entity_v4l2_video_device(entity)) ||
		    (!vdev && !is_media_entity_v4l2_subdev(entity)))
			continue;

		if (vdev)
			return entity;

		sd = media_entity_to_v4l2_subdev(entity);
		if (sd->grp_id & grp_id)
			return entity;
	}

	return NULL;
}

/*
 * Search for a subdev in the current pipeline with given grp_id.
 * Called with mdev->graph_mutex held.
 */
static struct v4l2_subdev *
find_pipeline_subdev(struct imx_media_dev *imxmd,
		     struct media_graph *graph,
		     struct media_entity *start_entity,
		     u32 grp_id)
{
	struct media_entity *entity = find_pipeline_entity(imxmd, graph,
							   start_entity,
							   grp_id, false);
	return entity ? media_entity_to_v4l2_subdev(entity) : NULL;
}

/*
 * Search for a video device in the current pipeline.
 * Called with mdev->graph_mutex held.
 */
static struct video_device *
find_pipeline_vdev(struct imx_media_dev *imxmd,
		   struct media_graph *graph,
		   struct media_entity *start_entity)
{
	struct media_entity *entity = find_pipeline_entity(imxmd, graph,
							   start_entity,
							   0, true);
	return entity ? media_entity_to_video_device(entity) : NULL;
}

/*
 * Search for an entity in the current pipeline with given grp_id,
 * then locate the remote enabled source pad from that entity.
 * Called with mdev->graph_mutex held.
 */
static struct media_pad *
find_pipeline_remote_source_pad(struct imx_media_dev *imxmd,
				struct media_graph *graph,
				struct media_entity *start_entity,
				u32 grp_id)
{
	struct media_pad *pad = NULL;
	struct media_entity *me;
	struct v4l2_subdev *sd;
	int i;

	sd = find_pipeline_subdev(imxmd, graph, start_entity, grp_id);
	if (!sd)
		return NULL;
	me = &sd->entity;

	/* Find remote source pad */
	for (i = 0; i < me->num_pads; i++) {
		struct media_pad *spad = &me->pads[i];

		if (!(spad->flags & MEDIA_PAD_FL_SINK))
			continue;
		pad = media_entity_remote_pad(spad);
		if (pad)
			return pad;
	}

	return NULL;
}

/*
 * Find the mipi-csi2 virtual channel reached from the given
 * start entity in the current pipeline.
 * Must be called with mdev->graph_mutex held.
 */
int imx_media_find_mipi_csi2_channel(struct imx_media_dev *imxmd,
				     struct media_entity *start_entity)
{
	struct media_graph graph;
	struct v4l2_subdev *sd;
	struct media_pad *pad;
	int ret;

	ret = media_graph_walk_init(&graph, &imxmd->md);
	if (ret)
		return ret;

	/* first try to locate the mipi-csi2 from the video mux */
	pad = find_pipeline_remote_source_pad(imxmd, &graph, start_entity,
					      IMX_MEDIA_GRP_ID_VIDMUX);
	/* if couldn't reach it from there, try from a CSI */
	if (!pad)
		pad = find_pipeline_remote_source_pad(imxmd, &graph,
						      start_entity,
						      IMX_MEDIA_GRP_ID_CSI);
	if (pad) {
		sd = media_entity_to_v4l2_subdev(pad->entity);
		if (sd->grp_id & IMX_MEDIA_GRP_ID_CSI2) {
			ret = pad->index - 1; /* found it! */
			dev_dbg(imxmd->md.dev, "found vc%d from %s\n",
				ret, start_entity->name);
			goto cleanup;
		}
	}

	ret = -EPIPE;

cleanup:
	media_graph_walk_cleanup(&graph);
	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_find_mipi_csi2_channel);

/*
 * Find a subdev reached from the given start entity in the
 * current pipeline.
 * Must be called with mdev->graph_mutex held.
 */
struct imx_media_subdev *
imx_media_find_pipeline_subdev(struct imx_media_dev *imxmd,
			       struct media_entity *start_entity,
			       u32 grp_id)
{
	struct imx_media_subdev *imxsd;
	struct media_graph graph;
	struct v4l2_subdev *sd;
	int ret;

	ret = media_graph_walk_init(&graph, &imxmd->md);
	if (ret)
		return ERR_PTR(ret);

	sd = find_pipeline_subdev(imxmd, &graph, start_entity, grp_id);
	if (!sd) {
		imxsd = ERR_PTR(-ENODEV);
		goto cleanup;
	}

	imxsd = imx_media_find_subdev_by_sd(imxmd, sd);
cleanup:
	media_graph_walk_cleanup(&graph);
	return imxsd;
}
EXPORT_SYMBOL_GPL(imx_media_find_pipeline_subdev);

/*
 * Find a video device reached from the given start entity in the
 * current pipeline.
 * Must be called with mdev->graph_mutex held.
 */
struct video_device *
imx_media_find_pipeline_video_device(struct imx_media_dev *imxmd,
				     struct media_entity *start_entity)
{
	struct video_device *vdev;
	struct media_graph graph;
	int ret;

	ret = media_graph_walk_init(&graph, &imxmd->md);
	if (ret)
		return ERR_PTR(ret);

	vdev = find_pipeline_vdev(imxmd, &graph, start_entity);
	if (!vdev)
		vdev = ERR_PTR(-ENODEV);

	media_graph_walk_cleanup(&graph);
	return vdev;
}
EXPORT_SYMBOL_GPL(imx_media_find_pipeline_video_device);

struct imx_media_subdev *
__imx_media_find_sensor(struct imx_media_dev *imxmd,
			struct media_entity *start_entity)
{
	return imx_media_find_pipeline_subdev(imxmd, start_entity,
					      IMX_MEDIA_GRP_ID_SENSOR);
}
EXPORT_SYMBOL_GPL(__imx_media_find_sensor);

struct imx_media_subdev *
imx_media_find_sensor(struct imx_media_dev *imxmd,
		      struct media_entity *start_entity)
{
	struct imx_media_subdev *sensor;

	mutex_lock(&imxmd->md.graph_mutex);
	sensor = __imx_media_find_sensor(imxmd, start_entity);
	mutex_unlock(&imxmd->md.graph_mutex);

	return sensor;
}
EXPORT_SYMBOL_GPL(imx_media_find_sensor);

/*
 * The subdevs have to be powered on/off, and streaming
 * enabled/disabled, in a specific sequence.
 */
static const u32 stream_on_seq[] = {
	IMX_MEDIA_GRP_ID_IC_PRPVF,
	IMX_MEDIA_GRP_ID_IC_PRPENC,
	IMX_MEDIA_GRP_ID_IC_PRP,
	IMX_MEDIA_GRP_ID_VDIC,
	IMX_MEDIA_GRP_ID_CSI2,
	IMX_MEDIA_GRP_ID_SENSOR,
	IMX_MEDIA_GRP_ID_VIDMUX,
	IMX_MEDIA_GRP_ID_CSI,
};

static const u32 stream_off_seq[] = {
	IMX_MEDIA_GRP_ID_IC_PRPVF,
	IMX_MEDIA_GRP_ID_IC_PRPENC,
	IMX_MEDIA_GRP_ID_IC_PRP,
	IMX_MEDIA_GRP_ID_VDIC,
	IMX_MEDIA_GRP_ID_CSI,
	IMX_MEDIA_GRP_ID_VIDMUX,
	IMX_MEDIA_GRP_ID_SENSOR,
	IMX_MEDIA_GRP_ID_CSI2,
};

#define NUM_STREAM_ENTITIES ARRAY_SIZE(stream_on_seq)

static const u32 power_on_seq[] = {
	IMX_MEDIA_GRP_ID_CSI2,
	IMX_MEDIA_GRP_ID_SENSOR,
	IMX_MEDIA_GRP_ID_VIDMUX,
	IMX_MEDIA_GRP_ID_CSI,
	IMX_MEDIA_GRP_ID_VDIC,
	IMX_MEDIA_GRP_ID_IC_PRPENC,
	IMX_MEDIA_GRP_ID_IC_PRPVF,
};

static const u32 power_off_seq[] = {
	IMX_MEDIA_GRP_ID_IC_PRPVF,
	IMX_MEDIA_GRP_ID_IC_PRPENC,
	IMX_MEDIA_GRP_ID_VDIC,
	IMX_MEDIA_GRP_ID_CSI,
	IMX_MEDIA_GRP_ID_VIDMUX,
	IMX_MEDIA_GRP_ID_SENSOR,
	IMX_MEDIA_GRP_ID_CSI2,
};

#define NUM_POWER_ENTITIES ARRAY_SIZE(power_on_seq)

static int imx_media_set_stream(struct imx_media_dev *imxmd,
				struct media_entity *start_entity,
				bool on)
{
	struct media_graph graph;
	struct v4l2_subdev *sd;
	int i, ret;
	u32 id;

	mutex_lock(&imxmd->md.graph_mutex);

	ret = media_graph_walk_init(&graph, &imxmd->md);
	if (ret)
		goto unlock;

	for (i = 0; i < NUM_STREAM_ENTITIES; i++) {
		id = on ? stream_on_seq[i] : stream_off_seq[i];
		sd = find_pipeline_subdev(imxmd, &graph,
					  start_entity, id);
		if (!sd)
			continue;

		ret = v4l2_subdev_call(sd, video, s_stream, on);
		if (on && ret && ret != -ENOIOCTLCMD)
			break;
	}

	media_graph_walk_cleanup(&graph);
unlock:
	mutex_unlock(&imxmd->md.graph_mutex);

	return (on && ret && ret != -ENOIOCTLCMD) ? ret : 0;
}

/*
 * Turn current pipeline streaming on/off starting from entity.
 */
int imx_media_pipeline_set_stream(struct imx_media_dev *imxmd,
				  struct media_entity *entity,
				  struct media_pipeline *pipe,
				  bool on)
{
	int ret = 0;

	if (on) {
		ret = media_pipeline_start(entity, pipe);
		if (ret)
			return ret;
		ret = imx_media_set_stream(imxmd, entity, true);
		if (!ret)
			return 0;
		/* fall through */
	}

	imx_media_set_stream(imxmd, entity, false);
	if (entity->pipe)
		media_pipeline_stop(entity);

	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_pipeline_set_stream);

static int imx_media_set_power(struct imx_media_dev *imxmd,
			       struct media_graph *graph,
			       struct media_entity *start_entity, bool on)
{
	struct v4l2_subdev *sd;
	int i, ret = 0;
	u32 id;

	for (i = 0; i < NUM_POWER_ENTITIES; i++) {
		id = on ? power_on_seq[i] : power_off_seq[i];
		sd = find_pipeline_subdev(imxmd, graph, start_entity, id);
		if (!sd)
			continue;

		ret = v4l2_subdev_call(sd, core, s_power, on);
		if (on && ret && ret != -ENOIOCTLCMD)
			break;
	}

	return (on && ret && ret != -ENOIOCTLCMD) ? ret : 0;
}

/*
 * Turn current pipeline power on/off starting from start_entity.
 * Must be called with mdev->graph_mutex held.
 */
int imx_media_pipeline_set_power(struct imx_media_dev *imxmd,
				 struct media_graph *graph,
				 struct media_entity *start_entity, bool on)
{
	int ret;

	ret = imx_media_set_power(imxmd, graph, start_entity, on);
	if (ret)
		imx_media_set_power(imxmd, graph, start_entity, false);

	return ret;
}
EXPORT_SYMBOL_GPL(imx_media_pipeline_set_power);

MODULE_DESCRIPTION("i.MX5/6 v4l2 media controller driver");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
