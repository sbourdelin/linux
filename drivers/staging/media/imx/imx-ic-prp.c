/*
 * V4L2 Capture IC Preprocess Subdev for Freescale i.MX5/6 SOC
 *
 * This subdevice handles capture of video frames from the CSI or VDIC,
 * which are routed directly to the Image Converter preprocess tasks,
 * for resizing, colorspace conversion, and rotation.
 *
 * Copyright (c) 2012-2017 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/imx.h>
#include "imx-media.h"
#include "imx-ic.h"

/*
 * Min/Max supported width and heights.
 */
#define MIN_W       176
#define MIN_H       144
#define MAX_W      4096
#define MAX_H      4096
#define W_ALIGN    4 /* multiple of 16 pixels */
#define H_ALIGN    1 /* multiple of 2 lines */
#define S_ALIGN    1 /* multiple of 2 */

struct prp_priv {
	struct imx_media_dev *md;
	struct imx_ic_priv *ic_priv;

	/* IPU units we require */
	struct ipu_soc *ipu;

	struct media_pad pad[PRP_NUM_PADS];

	struct v4l2_subdev *src_sd;
	struct v4l2_subdev *sink_sd_prpenc;
	struct v4l2_subdev *sink_sd_prpvf;

	/* the CSI id at link validate */
	int csi_id;

	/* the attached CSI at stream on */
	struct v4l2_subdev *csi_sd;
	/* the attached sensor at stream on */
	struct imx_media_subdev *sensor;

	struct v4l2_mbus_framefmt format_mbus[PRP_NUM_PADS];
	const struct imx_media_pixfmt *cc[PRP_NUM_PADS];

	bool stream_on; /* streaming is on */
};

static inline struct prp_priv *sd_to_priv(struct v4l2_subdev *sd)
{
	struct imx_ic_priv *ic_priv = v4l2_get_subdevdata(sd);

	return ic_priv->prp_priv;
}

static int prp_start(struct prp_priv *priv)
{
	struct imx_ic_priv *ic_priv = priv->ic_priv;

	if (!priv->sensor) {
		v4l2_err(&ic_priv->sd, "no sensor attached\n");
		return -EINVAL;
	}

	priv->ipu = priv->md->ipu[ic_priv->ipu_id];

	/* set IC to receive from CSI or VDI depending on source */
	if (priv->src_sd->grp_id & IMX_MEDIA_GRP_ID_VDIC)
		ipu_set_ic_src_mux(priv->ipu, 0, true);
	else
		ipu_set_ic_src_mux(priv->ipu, priv->csi_id, false);

	return 0;
}

static void prp_stop(struct prp_priv *priv)
{
}

static int prp_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= PRP_NUM_PADS)
		return -EINVAL;

	return imx_media_enum_ipu_format(NULL, &code->code, code->index, true);
}

static struct v4l2_mbus_framefmt *
__prp_get_fmt(struct prp_priv *priv, struct v4l2_subdev_pad_config *cfg,
	      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	struct imx_ic_priv *ic_priv = priv->ic_priv;

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&ic_priv->sd, cfg, pad);
	else
		return &priv->format_mbus[pad];
}

static int prp_get_fmt(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_format *sdformat)
{
	struct prp_priv *priv = sd_to_priv(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (sdformat->pad >= PRP_NUM_PADS)
		return -EINVAL;

	fmt = __prp_get_fmt(priv, cfg, sdformat->pad, sdformat->which);
	if (!fmt)
		return -EINVAL;

	sdformat->format = *fmt;

	return 0;
}

static int prp_set_fmt(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_format *sdformat)
{
	struct prp_priv *priv = sd_to_priv(sd);
	const struct imx_media_pixfmt *cc;
	struct v4l2_mbus_framefmt *infmt;
	u32 code;

	if (sdformat->pad >= PRP_NUM_PADS)
		return -EINVAL;

	if (priv->stream_on)
		return -EBUSY;

	cc = imx_media_find_ipu_format(0, sdformat->format.code, true);
	if (!cc) {
		imx_media_enum_ipu_format(NULL, &code, 0, true);
		cc = imx_media_find_ipu_format(0, code, true);
		sdformat->format.code = cc->codes[0];
	}

	v4l_bound_align_image(&sdformat->format.width, MIN_W, MAX_W,
			      W_ALIGN, &sdformat->format.height,
			      MIN_H, MAX_H, H_ALIGN, S_ALIGN);

	/* Output pads mirror input pad */
	if (sdformat->pad == PRP_SRC_PAD_PRPENC ||
	    sdformat->pad == PRP_SRC_PAD_PRPVF) {
		infmt = __prp_get_fmt(priv, cfg, PRP_SINK_PAD,
				      sdformat->which);
		sdformat->format = *infmt;
	}

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = sdformat->format;
	} else {
		priv->format_mbus[sdformat->pad] = sdformat->format;
		priv->cc[sdformat->pad] = cc;
	}

	return 0;
}

static int prp_link_setup(struct media_entity *entity,
			  const struct media_pad *local,
			  const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct imx_ic_priv *ic_priv = v4l2_get_subdevdata(sd);
	struct prp_priv *priv = ic_priv->prp_priv;
	struct v4l2_subdev *remote_sd;

	dev_dbg(ic_priv->dev, "link setup %s -> %s", remote->entity->name,
		local->entity->name);

	remote_sd = media_entity_to_v4l2_subdev(remote->entity);

	if (local->flags & MEDIA_PAD_FL_SINK) {
		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (priv->src_sd)
				return -EBUSY;
			if (priv->sink_sd_prpenc && (remote_sd->grp_id &
						     IMX_MEDIA_GRP_ID_VDIC))
				return -EINVAL;
			priv->src_sd = remote_sd;
		} else {
			priv->src_sd = NULL;
		}

		return 0;
	}

	/* this is a source pad */
	if (flags & MEDIA_LNK_FL_ENABLED) {
		switch (local->index) {
		case PRP_SRC_PAD_PRPENC:
			if (priv->sink_sd_prpenc)
				return -EBUSY;
			if (priv->src_sd && (priv->src_sd->grp_id &
					     IMX_MEDIA_GRP_ID_VDIC))
				return -EINVAL;
			priv->sink_sd_prpenc = remote_sd;
			break;
		case PRP_SRC_PAD_PRPVF:
			if (priv->sink_sd_prpvf)
				return -EBUSY;
			priv->sink_sd_prpvf = remote_sd;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (local->index) {
		case PRP_SRC_PAD_PRPENC:
			priv->sink_sd_prpenc = NULL;
			break;
		case PRP_SRC_PAD_PRPVF:
			priv->sink_sd_prpvf = NULL;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int prp_link_validate(struct v4l2_subdev *sd,
			     struct media_link *link,
			     struct v4l2_subdev_format *source_fmt,
			     struct v4l2_subdev_format *sink_fmt)
{
	struct imx_ic_priv *ic_priv = v4l2_get_subdevdata(sd);
	struct prp_priv *priv = ic_priv->prp_priv;
	struct v4l2_of_endpoint *sensor_ep;
	int ret;

	ret = v4l2_subdev_link_validate_default(sd, link,
						source_fmt, sink_fmt);
	if (ret)
		return ret;

	/* the ->PRPENC link cannot be enabled if the source is the VDIC */
	if (priv->sink_sd_prpenc && (priv->src_sd->grp_id &
				     IMX_MEDIA_GRP_ID_VDIC))
		return -EINVAL;

	priv->sensor = __imx_media_find_sensor(priv->md, &ic_priv->sd.entity);
	if (IS_ERR(priv->sensor)) {
		v4l2_err(&ic_priv->sd, "no sensor attached\n");
		ret = PTR_ERR(priv->sensor);
		priv->sensor = NULL;
		return ret;
	}

	sensor_ep = &priv->sensor->sensor_ep;

	if (priv->src_sd->grp_id & IMX_MEDIA_GRP_ID_CSI) {
		priv->csi_sd = priv->src_sd;
	} else {
		struct imx_media_subdev *csi =
			imx_media_find_pipeline_subdev(
				priv->md, &ic_priv->sd.entity,
				IMX_MEDIA_GRP_ID_CSI);
		if (IS_ERR(csi)) {
			v4l2_err(&ic_priv->sd, "no CSI attached\n");
			ret = PTR_ERR(csi);
			return ret;
		}

		priv->csi_sd = csi->sd;
	}

	switch (priv->csi_sd->grp_id) {
	case IMX_MEDIA_GRP_ID_CSI0:
		priv->csi_id = 0;
		break;
	case IMX_MEDIA_GRP_ID_CSI1:
		priv->csi_id = 1;
		break;
	default:
		return -EINVAL;
	}

	if (sensor_ep->bus_type == V4L2_MBUS_CSI2) {
		int vc_num = 0;
		/* see NOTE in imx-csi.c */
#if 0
		vc_num = imx_media_find_mipi_csi2_channel(
			priv->md, &ic_priv->sd.entity);
		if (vc_num < 0)
			return vc_num;
#endif
		/* only virtual channel 0 can be sent to IC */
		if (vc_num != 0)
			return -EINVAL;
	} else {
		/*
		 * only 8-bit pixels can be sent to IC for parallel
		 * busses
		 */
		if (sensor_ep->bus.parallel.bus_width >= 16)
			return -EINVAL;
	}

	return 0;
}

static int prp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx_ic_priv *ic_priv = v4l2_get_subdevdata(sd);
	struct prp_priv *priv = ic_priv->prp_priv;
	int ret = 0;

	if (!priv->src_sd || (!priv->sink_sd_prpenc && !priv->sink_sd_prpvf))
		return -EPIPE;

	dev_dbg(ic_priv->dev, "stream %s\n", enable ? "ON" : "OFF");

	if (enable && !priv->stream_on)
		ret = prp_start(priv);
	else if (!enable && priv->stream_on)
		prp_stop(priv);

	if (!ret)
		priv->stream_on = enable;
	return ret;
}

/*
 * retrieve our pads parsed from the OF graph by the media device
 */
static int prp_registered(struct v4l2_subdev *sd)
{
	struct prp_priv *priv = sd_to_priv(sd);
	int i, ret;
	u32 code;

	/* get media device */
	priv->md = dev_get_drvdata(sd->v4l2_dev->dev);

	for (i = 0; i < PRP_NUM_PADS; i++) {
		priv->pad[i].flags = (i == PRP_SINK_PAD) ?
			MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;

		/* set a default mbus format  */
		imx_media_enum_ipu_format(NULL, &code, 0, true);
		ret = imx_media_init_mbus_fmt(&priv->format_mbus[i],
					      640, 480, code, V4L2_FIELD_NONE,
					      &priv->cc[i]);
		if (ret)
			return ret;
	}

	return media_entity_pads_init(&sd->entity, PRP_NUM_PADS, priv->pad);
}

static struct v4l2_subdev_pad_ops prp_pad_ops = {
	.enum_mbus_code = prp_enum_mbus_code,
	.get_fmt = prp_get_fmt,
	.set_fmt = prp_set_fmt,
	.link_validate = prp_link_validate,
};

static struct v4l2_subdev_video_ops prp_video_ops = {
	.s_stream = prp_s_stream,
};

static struct media_entity_operations prp_entity_ops = {
	.link_setup = prp_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_subdev_ops prp_subdev_ops = {
	.video = &prp_video_ops,
	.pad = &prp_pad_ops,
};

static struct v4l2_subdev_internal_ops prp_internal_ops = {
	.registered = prp_registered,
};

static int prp_init(struct imx_ic_priv *ic_priv)
{
	struct prp_priv *priv;

	priv = devm_kzalloc(ic_priv->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ic_priv->prp_priv = priv;
	priv->ic_priv = ic_priv;

	return 0;
}

static void prp_remove(struct imx_ic_priv *ic_priv)
{
}

struct imx_ic_ops imx_ic_prp_ops = {
	.subdev_ops = &prp_subdev_ops,
	.internal_ops = &prp_internal_ops,
	.entity_ops = &prp_entity_ops,
	.init = prp_init,
	.remove = prp_remove,
};
