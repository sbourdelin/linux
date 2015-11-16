/*
 * vsp1_clu.c  --  R-Car VSP1 Cubic Look-Up Table
 *
 * Copyright (C) 2015 Renesas Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/vsp1.h>

#include <media/v4l2-subdev.h>

#include "vsp1.h"
#include "vsp1_clu.h"

#define CLU_MIN_SIZE				4U
#define CLU_MAX_SIZE				8190U

/* -----------------------------------------------------------------------------
 * Device Access
 */

static inline void vsp1_clu_write(struct vsp1_clu *clu, u32 reg, u32 data)
{
	vsp1_write(clu->entity.vsp1, reg, data);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Core Operations
 */

static int clu_configure(struct vsp1_clu *clu, struct vsp1_clu_config *config)
{
	struct vsp1_clu_entry *entries;
	unsigned int i;
	int ret;

	if (config->nentries > 17*17*17)
		return -EINVAL;

	entries = kcalloc(config->nentries, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	ret = copy_from_user(entries, config->entries,
			     config->nentries * sizeof(*entries));
	if (ret) {
		ret = -EFAULT;
		goto done;
	}

	for (i = 0; i < config->nentries; ++i) {
		u32 addr = entries[i].addr;
		u32 value = entries[i].value;

		if (((addr >> 0) & 0xff) >= 17 ||
		    ((addr >> 8) & 0xff) >= 17 ||
		    ((addr >> 16) & 0xff) >= 17 ||
		    ((addr >> 24) & 0xff) != 0 ||
		    (value & 0xff000000) != 0) {
			ret = -EINVAL;
			goto done;
		}

		vsp1_clu_write(clu, VI6_CLU_ADDR, addr);
		vsp1_clu_write(clu, VI6_CLU_DATA, value);
	}

done:
	kfree(entries);
	return ret;
}

static long clu_ioctl(struct v4l2_subdev *subdev, unsigned int cmd, void *arg)
{
	struct vsp1_clu *clu = to_clu(subdev);

	switch (cmd) {
	case VIDIOC_VSP1_CLU_CONFIG:
		return clu_configure(clu, arg);

	default:
		return -ENOIOCTLCMD;
	}
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Video Operations
 */

static int clu_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct vsp1_clu *clu = to_clu(subdev);

	if (!enable)
		return 0;

	vsp1_clu_write(clu, VI6_CLU_CTRL, VI6_CLU_CTRL_MVS | VI6_CLU_CTRL_EN);

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Pad Operations
 */

static int clu_enum_mbus_code(struct v4l2_subdev *subdev,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	static const unsigned int codes[] = {
		MEDIA_BUS_FMT_ARGB8888_1X32,
		MEDIA_BUS_FMT_AHSV8888_1X32,
		MEDIA_BUS_FMT_AYUV8_1X32,
	};
	struct vsp1_clu *clu = to_clu(subdev);
	struct v4l2_mbus_framefmt *format;

	if (code->pad == CLU_PAD_SINK) {
		if (code->index >= ARRAY_SIZE(codes))
			return -EINVAL;

		code->code = codes[code->index];
	} else {
		/* The CLU can't perform format conversion, the sink format is
		 * always identical to the source format.
		 */
		if (code->index)
			return -EINVAL;

		format = vsp1_entity_get_pad_format(&clu->entity, cfg,
						    CLU_PAD_SINK, code->which);
		code->code = format->code;
	}

	return 0;
}

static int clu_enum_frame_size(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct vsp1_clu *clu = to_clu(subdev);
	struct v4l2_mbus_framefmt *format;

	format = vsp1_entity_get_pad_format(&clu->entity, cfg,
					    fse->pad, fse->which);

	if (fse->index || fse->code != format->code)
		return -EINVAL;

	if (fse->pad == CLU_PAD_SINK) {
		fse->min_width = CLU_MIN_SIZE;
		fse->max_width = CLU_MAX_SIZE;
		fse->min_height = CLU_MIN_SIZE;
		fse->max_height = CLU_MAX_SIZE;
	} else {
		/* The size on the source pad are fixed and always identical to
		 * the size on the sink pad.
		 */
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

	return 0;
}

static int clu_get_format(struct v4l2_subdev *subdev,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct vsp1_clu *clu = to_clu(subdev);

	fmt->format = *vsp1_entity_get_pad_format(&clu->entity, cfg, fmt->pad,
						  fmt->which);

	return 0;
}

static int clu_set_format(struct v4l2_subdev *subdev,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct vsp1_clu *clu = to_clu(subdev);
	struct v4l2_mbus_framefmt *format;

	/* Default to YUV if the requested format is not supported. */
	if (fmt->format.code != MEDIA_BUS_FMT_ARGB8888_1X32 &&
	    fmt->format.code != MEDIA_BUS_FMT_AHSV8888_1X32 &&
	    fmt->format.code != MEDIA_BUS_FMT_AYUV8_1X32)
		fmt->format.code = MEDIA_BUS_FMT_AYUV8_1X32;

	format = vsp1_entity_get_pad_format(&clu->entity, cfg, fmt->pad,
					    fmt->which);

	if (fmt->pad == CLU_PAD_SOURCE) {
		/* The CLU output format can't be modified. */
		fmt->format = *format;
		return 0;
	}

	format->code = fmt->format.code;
	format->width = clamp_t(unsigned int, fmt->format.width,
				CLU_MIN_SIZE, CLU_MAX_SIZE);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 CLU_MIN_SIZE, CLU_MAX_SIZE);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	format = vsp1_entity_get_pad_format(&clu->entity, cfg, CLU_PAD_SOURCE,
					    fmt->which);
	*format = fmt->format;

	return 0;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

static struct v4l2_subdev_core_ops clu_core_ops = {
	.ioctl = clu_ioctl,
};

static struct v4l2_subdev_video_ops clu_video_ops = {
	.s_stream = clu_s_stream,
};

static struct v4l2_subdev_pad_ops clu_pad_ops = {
	.enum_mbus_code = clu_enum_mbus_code,
	.enum_frame_size = clu_enum_frame_size,
	.get_fmt = clu_get_format,
	.set_fmt = clu_set_format,
};

static struct v4l2_subdev_ops clu_ops = {
	.core	= &clu_core_ops,
	.video	= &clu_video_ops,
	.pad    = &clu_pad_ops,
};

/* -----------------------------------------------------------------------------
 * Initialization and Cleanup
 */

struct vsp1_clu *vsp1_clu_create(struct vsp1_device *vsp1)
{
	struct v4l2_subdev *subdev;
	struct vsp1_clu *clu;
	int ret;

	clu = devm_kzalloc(vsp1->dev, sizeof(*clu), GFP_KERNEL);
	if (clu == NULL)
		return ERR_PTR(-ENOMEM);

	clu->entity.type = VSP1_ENTITY_CLU;

	ret = vsp1_entity_init(vsp1, &clu->entity, 2);
	if (ret < 0)
		return ERR_PTR(ret);

	/* Initialize the V4L2 subdev. */
	subdev = &clu->entity.subdev;
	v4l2_subdev_init(subdev, &clu_ops);

	subdev->entity.ops = &vsp1_media_ops;
	subdev->internal_ops = &vsp1_subdev_internal_ops;
	snprintf(subdev->name, sizeof(subdev->name), "%s clu",
		 dev_name(vsp1->dev));
	v4l2_set_subdevdata(subdev, clu);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	vsp1_entity_init_formats(subdev, NULL);

	return clu;
}
