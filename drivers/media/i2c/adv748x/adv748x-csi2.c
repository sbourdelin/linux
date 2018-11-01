// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Analog Devices ADV748X CSI-2 Transmitter
 *
 * Copyright (C) 2017 Renesas Electronics Corp.
 */

#include <linux/module.h>
#include <linux/mutex.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "adv748x.h"

#define ADV748X_CSI2_ROUTES_MAX 4

struct adv748x_csi2_format {
	unsigned int code;
	unsigned int datatype;
};

static const struct adv748x_csi2_format adv748x_csi2_formats[] = {
	{ .code = MEDIA_BUS_FMT_RGB888_1X24,    .datatype = 0x24, },
	{ .code = MEDIA_BUS_FMT_UYVY8_1X16,     .datatype = 0x1e, },
	{ .code = MEDIA_BUS_FMT_UYVY8_2X8,      .datatype = 0x1e, },
	{ .code = MEDIA_BUS_FMT_YUYV10_2X10,    .datatype = 0x1e, },
};

static unsigned int adv748x_csi2_code_to_datatype(unsigned int code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(adv748x_csi2_formats); i++)
		if (adv748x_csi2_formats[i].code == code)
			return adv748x_csi2_formats[i].datatype;
	return 0;
}

static int adv748x_csi2_set_virtual_channel(struct adv748x_csi2 *tx,
					    unsigned int vc)
{
	return tx_write(tx, ADV748X_CSI_VC_REF, vc << ADV748X_CSI_VC_REF_SHIFT);
}

/**
 * adv748x_csi2_register_link : Register and link internal entities
 *
 * @tx: CSI2 private entity
 * @v4l2_dev: Video registration device
 * @src: Source subdevice to establish link
 * @src_pad: Pad number of source to link to this @tx
 *
 * Ensure that the subdevice is registered against the v4l2_device, and link the
 * source pad to the sink pad of the CSI2 bus entity.
 */
static int adv748x_csi2_register_link(struct adv748x_csi2 *tx,
				      struct v4l2_device *v4l2_dev,
				      struct v4l2_subdev *src,
				      unsigned int src_pad)
{
	int enabled = MEDIA_LNK_FL_ENABLED;
	int ret;

	/*
	 * Dynamic linking of the AFE is not supported.
	 * Register the links as immutable.
	 */
	enabled |= MEDIA_LNK_FL_IMMUTABLE;

	if (!src->v4l2_dev) {
		ret = v4l2_device_register_subdev(v4l2_dev, src);
		if (ret)
			return ret;
	}

	return media_create_pad_link(&src->entity, src_pad,
				     &tx->sd.entity, ADV748X_CSI2_SINK,
				     enabled);
}

/* -----------------------------------------------------------------------------
 * v4l2_subdev_internal_ops
 *
 * We use the internal registered operation to be able to ensure that our
 * incremental subdevices (not connected in the forward path) can be registered
 * against the resulting video path and media device.
 */

static int adv748x_csi2_registered(struct v4l2_subdev *sd)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct adv748x_state *state = tx->state;

	adv_dbg(state, "Registered %s (%s)", is_txa(tx) ? "TXA":"TXB",
			sd->name);

	/*
	 * The adv748x hardware allows the AFE to route through the TXA, however
	 * this is not currently supported in this driver.
	 *
	 * Link HDMI->TXA, and AFE->TXB directly.
	 */
	if (is_txa(tx) && is_hdmi_enabled(state))
		return adv748x_csi2_register_link(tx, sd->v4l2_dev,
						  &state->hdmi.sd,
						  ADV748X_HDMI_SOURCE);
	if (!is_txa(tx) && is_afe_enabled(state))
		return adv748x_csi2_register_link(tx, sd->v4l2_dev,
						  &state->afe.sd,
						  ADV748X_AFE_SOURCE);
	return 0;
}

static const struct v4l2_subdev_internal_ops adv748x_csi2_internal_ops = {
	.registered = adv748x_csi2_registered,
};

/* -----------------------------------------------------------------------------
 * v4l2_subdev_video_ops
 */

static int adv748x_csi2_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct v4l2_subdev *src;

	src = adv748x_get_remote_sd(&tx->pads[ADV748X_CSI2_SINK]);
	if (!src)
		return -EPIPE;

	return v4l2_subdev_call(src, video, s_stream, enable);
}

static const struct v4l2_subdev_video_ops adv748x_csi2_video_ops = {
	.s_stream = adv748x_csi2_s_stream,
};

/* -----------------------------------------------------------------------------
 * v4l2_subdev_pad_ops
 *
 * The CSI2 bus pads are ignorant to the data sizes or formats.
 * But we must support setting the pad formats for format propagation.
 */

static struct v4l2_mbus_framefmt *
adv748x_csi2_get_pad_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    unsigned int pad, u32 which)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(sd, cfg, pad);

	return &tx->format;
}

static int adv748x_csi2_get_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *sdformat)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct adv748x_state *state = tx->state;
	struct v4l2_mbus_framefmt *mbusformat;

	if (sdformat->pad != ADV748X_CSI2_SINK)
		return -EINVAL;

	mbusformat = adv748x_csi2_get_pad_format(sd, cfg, sdformat->pad,
						 sdformat->which);
	if (!mbusformat)
		return -EINVAL;

	mutex_lock(&state->mutex);

	sdformat->format = *mbusformat;

	mutex_unlock(&state->mutex);

	return 0;
}

static int adv748x_csi2_set_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *sdformat)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct adv748x_state *state = tx->state;
	struct v4l2_mbus_framefmt *mbusformat;
	int ret = 0;

	if (sdformat->pad != ADV748X_CSI2_SINK)
		return -EINVAL;

	mbusformat = adv748x_csi2_get_pad_format(sd, cfg, sdformat->pad,
						 sdformat->which);
	if (!mbusformat)
		return -EINVAL;

	mutex_lock(&state->mutex);

	if (sdformat->pad == ADV748X_CSI2_SOURCE) {
		const struct v4l2_mbus_framefmt *sink_fmt;

		sink_fmt = adv748x_csi2_get_pad_format(sd, cfg,
						       ADV748X_CSI2_SINK,
						       sdformat->which);

		if (!sink_fmt) {
			ret = -EINVAL;
			goto unlock;
		}

		sdformat->format = *sink_fmt;
	}

	*mbusformat = sdformat->format;

unlock:
	mutex_unlock(&state->mutex);

	return ret;
}

static int adv748x_csi2_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				       struct v4l2_mbus_frame_desc *fd)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct v4l2_mbus_framefmt *mbusformat;

	memset(fd, 0, sizeof(*fd));

	if (pad != ADV748X_CSI2_SOURCE)
		return -EINVAL;

	mbusformat = adv748x_csi2_get_pad_format(sd, NULL, ADV748X_CSI2_SINK,
						 V4L2_SUBDEV_FORMAT_ACTIVE);
	if (!mbusformat)
		return -EINVAL;

	fd->entry->stream = tx->vc;
	fd->entry->bus.csi2.channel = tx->vc;
	fd->entry->bus.csi2.data_type =
		adv748x_csi2_code_to_datatype(mbusformat->code);

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;

	return 0;
}

static int adv748x_csi2_get_routing(struct v4l2_subdev *sd,
				    struct v4l2_subdev_routing *routing)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct v4l2_subdev_route *r = routing->routes;
	unsigned int vc;

	if (routing->num_routes < ADV748X_CSI2_ROUTES_MAX) {
		routing->num_routes = ADV748X_CSI2_ROUTES_MAX;
		return -ENOSPC;
	}

	routing->num_routes = ADV748X_CSI2_ROUTES_MAX;

	for (vc = 0; vc < ADV748X_CSI2_ROUTES_MAX; vc++) {
		r->sink_pad = ADV748X_CSI2_SINK;
		r->sink_stream = 0;
		r->source_pad = ADV748X_CSI2_SOURCE;
		r->source_stream = vc;
		r->flags = vc == tx->vc ? V4L2_SUBDEV_ROUTE_FL_ACTIVE : 0;
		r++;
	}

	return 0;
}

static int adv748x_csi2_set_routing(struct v4l2_subdev *sd,
				    struct v4l2_subdev_routing *routing)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);
	struct v4l2_subdev_route *r = routing->routes;
	unsigned int i;
	int vc = -1;

	if (routing->num_routes > ADV748X_CSI2_ROUTES_MAX)
		return -ENOSPC;

	for (i = 0; i < routing->num_routes; i++) {
		if (r->sink_pad != ADV748X_CSI2_SINK ||
		    r->sink_stream != 0 ||
		    r->source_pad != ADV748X_CSI2_SOURCE ||
		    r->source_stream >= ADV748X_CSI2_ROUTES_MAX)
			return -EINVAL;

		if (r->flags & V4L2_SUBDEV_ROUTE_FL_ACTIVE) {
			if (vc != -1)
				return -EMLINK;

			vc = r->source_stream;
		}
		r++;
	}

	if (vc != -1)
		tx->vc = vc;

	adv748x_csi2_set_virtual_channel(tx, tx->vc);

	return 0;
}

static const struct v4l2_subdev_pad_ops adv748x_csi2_pad_ops = {
	.get_fmt = adv748x_csi2_get_format,
	.set_fmt = adv748x_csi2_set_format,
	.get_frame_desc = adv748x_csi2_get_frame_desc,
	.get_routing = adv748x_csi2_get_routing,
	.set_routing = adv748x_csi2_set_routing,
};

/* -----------------------------------------------------------------------------
 * v4l2_subdev_ops
 */

static const struct v4l2_subdev_ops adv748x_csi2_ops = {
	.video = &adv748x_csi2_video_ops,
	.pad = &adv748x_csi2_pad_ops,
};

/* -----------------------------------------------------------------------------
 * Subdev module and controls
 */

int adv748x_csi2_set_pixelrate(struct v4l2_subdev *sd, s64 rate)
{
	struct adv748x_csi2 *tx = adv748x_sd_to_csi2(sd);

	if (!tx->pixel_rate)
		return -EINVAL;

	return v4l2_ctrl_s_ctrl_int64(tx->pixel_rate, rate);
}

static int adv748x_csi2_s_ctrl(struct v4l2_ctrl *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_PIXEL_RATE:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops adv748x_csi2_ctrl_ops = {
	.s_ctrl = adv748x_csi2_s_ctrl,
};

static int adv748x_csi2_init_controls(struct adv748x_csi2 *tx)
{

	v4l2_ctrl_handler_init(&tx->ctrl_hdl, 1);

	tx->pixel_rate = v4l2_ctrl_new_std(&tx->ctrl_hdl,
					   &adv748x_csi2_ctrl_ops,
					   V4L2_CID_PIXEL_RATE, 1, INT_MAX,
					   1, 1);

	tx->sd.ctrl_handler = &tx->ctrl_hdl;
	if (tx->ctrl_hdl.error) {
		v4l2_ctrl_handler_free(&tx->ctrl_hdl);
		return tx->ctrl_hdl.error;
	}

	return v4l2_ctrl_handler_setup(&tx->ctrl_hdl);
}

int adv748x_csi2_init(struct adv748x_state *state, struct adv748x_csi2 *tx)
{
	int ret;

	if (!is_tx_enabled(tx))
		return 0;

	/* Initialise the virtual channel */
	tx->vc = 0;
	adv748x_csi2_set_virtual_channel(tx, tx->vc);

	adv748x_subdev_init(&tx->sd, state, &adv748x_csi2_ops,
			    MEDIA_ENT_F_VID_IF_BRIDGE,
			    is_txa(tx) ? "txa" : "txb");

	/* Ensure that matching is based upon the endpoint fwnodes */
	tx->sd.fwnode = of_fwnode_handle(state->endpoints[tx->port]);

	/* Register internal ops for incremental subdev registration */
	tx->sd.internal_ops = &adv748x_csi2_internal_ops;

	tx->pads[ADV748X_CSI2_SINK].flags = MEDIA_PAD_FL_SINK;
	tx->pads[ADV748X_CSI2_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&tx->sd.entity, ADV748X_CSI2_NR_PADS,
				     tx->pads);
	if (ret)
		return ret;

	ret = adv748x_csi2_init_controls(tx);
	if (ret)
		goto err_free_media;

	ret = v4l2_async_register_subdev(&tx->sd);
	if (ret)
		goto err_free_ctrl;

	return 0;

err_free_ctrl:
	v4l2_ctrl_handler_free(&tx->ctrl_hdl);
err_free_media:
	media_entity_cleanup(&tx->sd.entity);

	return ret;
}

void adv748x_csi2_cleanup(struct adv748x_csi2 *tx)
{
	if (!is_tx_enabled(tx))
		return;

	v4l2_async_unregister_subdev(&tx->sd);
	media_entity_cleanup(&tx->sd.entity);
	v4l2_ctrl_handler_free(&tx->ctrl_hdl);
}
