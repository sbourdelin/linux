/*
 * video stream multiplexer controlled via gpio or syscon
 *
 * Copyright (C) 2013 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 * Copyright (C) 2016 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>

struct vidsw {
	struct v4l2_subdev subdev;
	unsigned int num_pads;
	struct media_pad *pads;
	struct v4l2_mbus_framefmt *format_mbus;
	struct v4l2_fract timeperframe;
	struct v4l2_of_endpoint *endpoint;
	struct regmap_field *field;
	struct gpio_desc *gpio;
	int active;
};

static inline struct vidsw *v4l2_subdev_to_vidsw(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vidsw, subdev);
}

static void vidsw_set_active(struct vidsw *vidsw, int active)
{
	vidsw->active = active;
	if (active < 0)
		return;

	dev_dbg(vidsw->subdev.dev, "setting %d active\n", active);

	if (vidsw->field)
		regmap_field_write(vidsw->field, active);
	else if (vidsw->gpio)
		gpiod_set_value(vidsw->gpio, active);
}

static int vidsw_link_setup(struct media_entity *entity,
			    const struct media_pad *local,
			    const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);

	/* We have no limitations on enabling or disabling our output link */
	if (local->index == vidsw->num_pads - 1)
		return 0;

	dev_dbg(sd->dev, "link setup %s -> %s", remote->entity->name,
		local->entity->name);

	if (!(flags & MEDIA_LNK_FL_ENABLED)) {
		if (local->index == vidsw->active) {
			dev_dbg(sd->dev, "going inactive\n");
			vidsw->active = -1;
		}
		return 0;
	}

	if (vidsw->active >= 0) {
		struct media_pad *pad;

		if (vidsw->active == local->index)
			return 0;

		pad = media_entity_remote_pad(&vidsw->pads[vidsw->active]);
		if (pad) {
			struct media_link *link;
			int ret;

			link = media_entity_find_link(pad,
						&vidsw->pads[vidsw->active]);
			if (link) {
				ret = __media_entity_setup_link(link, 0);
				if (ret)
					return ret;
			}
		}
	}

	vidsw_set_active(vidsw, local->index);

	return 0;
}

static struct media_entity_operations vidsw_ops = {
	.link_setup = vidsw_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static bool vidsw_endpoint_disabled(struct device_node *ep)
{
	struct device_node *rpp;

	if (!of_device_is_available(ep))
		return true;

	rpp = of_graph_get_remote_port_parent(ep);
	if (!rpp)
		return true;

	return !of_device_is_available(rpp);
}

static int vidsw_async_init(struct vidsw *vidsw, struct device_node *node)
{
	struct device_node *ep;
	u32 portno;
	int numports;
	int ret;
	int i;
	bool active_link = false;

	numports = vidsw->num_pads;

	for (i = 0; i < numports - 1; i++)
		vidsw->pads[i].flags = MEDIA_PAD_FL_SINK;
	vidsw->pads[numports - 1].flags = MEDIA_PAD_FL_SOURCE;

	vidsw->subdev.entity.function = MEDIA_ENT_F_VID_MUX;
	ret = media_entity_pads_init(&vidsw->subdev.entity, numports,
				     vidsw->pads);
	if (ret < 0)
		return ret;

	vidsw->subdev.entity.ops = &vidsw_ops;

	for_each_endpoint_of_node(node, ep) {
		struct v4l2_of_endpoint endpoint;

		v4l2_of_parse_endpoint(ep, &endpoint);

		portno = endpoint.base.port;
		if (portno >= numports - 1)
			continue;

		if (vidsw_endpoint_disabled(ep)) {
			dev_dbg(vidsw->subdev.dev,
				"port %d disabled\n", portno);
			continue;
		}

		vidsw->endpoint[portno] = endpoint;

		if (portno == vidsw->active)
			active_link = true;
	}

	for (portno = 0; portno < numports - 1; portno++) {
		if (!vidsw->endpoint[portno].base.local_node)
			continue;

		/* If the active input is not connected, use another */
		if (!active_link) {
			vidsw_set_active(vidsw, portno);
			active_link = true;
		}
	}

	return v4l2_async_register_subdev(&vidsw->subdev);
}

int vidsw_g_mbus_config(struct v4l2_subdev *sd, struct v4l2_mbus_config *cfg)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);
	struct media_pad *pad;
	int ret;

	if (vidsw->active == -1) {
		dev_err(sd->dev, "no configuration for inactive mux\n");
		return -EINVAL;
	}

	/*
	 * Retrieve media bus configuration from the entity connected to the
	 * active input
	 */
	pad = media_entity_remote_pad(&vidsw->pads[vidsw->active]);
	if (pad) {
		sd = media_entity_to_v4l2_subdev(pad->entity);
		ret = v4l2_subdev_call(sd, video, g_mbus_config, cfg);
		if (ret == -ENOIOCTLCMD)
			pad = NULL;
		else if (ret < 0) {
			dev_err(sd->dev, "failed to get source configuration\n");
			return ret;
		}
	}
	if (!pad) {
		/* Mirror the input side on the output side */
		cfg->type = vidsw->endpoint[vidsw->active].bus_type;
		if (cfg->type == V4L2_MBUS_PARALLEL ||
		    cfg->type == V4L2_MBUS_BT656)
			cfg->flags = vidsw->endpoint[vidsw->active].bus.parallel.flags;
	}

	return 0;
}

static int vidsw_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);
	struct v4l2_subdev *upstream_sd;
	struct media_pad *pad;

	if (vidsw->active == -1) {
		dev_err(sd->dev, "Can not start streaming on inactive mux\n");
		return -EINVAL;
	}

	pad = media_entity_remote_pad(&sd->entity.pads[vidsw->active]);
	if (!pad) {
		dev_err(sd->dev, "Failed to find remote source pad\n");
		return -ENOLINK;
	}

	if (!is_media_entity_v4l2_subdev(pad->entity)) {
		dev_err(sd->dev, "Upstream entity is not a v4l2 subdev\n");
		return -ENODEV;
	}

	upstream_sd = media_entity_to_v4l2_subdev(pad->entity);

	return v4l2_subdev_call(upstream_sd, video, s_stream, enable);
}

static int vidsw_g_frame_interval(struct v4l2_subdev *sd,
				  struct v4l2_subdev_frame_interval *fi)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);

	fi->interval = vidsw->timeperframe;

	return 0;
}

static int vidsw_s_frame_interval(struct v4l2_subdev *sd,
				  struct v4l2_subdev_frame_interval *fi)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);

	/* Output pad mirrors active input pad, no limits on input pads */
	if (fi->pad == (vidsw->num_pads - 1))
		fi->interval = vidsw->timeperframe;

	vidsw->timeperframe = fi->interval;

	return 0;
}

static const struct v4l2_subdev_video_ops vidsw_subdev_video_ops = {
	.g_mbus_config = vidsw_g_mbus_config,
	.s_stream = vidsw_s_stream,
	.g_frame_interval = vidsw_g_frame_interval,
	.s_frame_interval = vidsw_s_frame_interval,
};

static struct v4l2_mbus_framefmt *
__vidsw_get_pad_format(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       unsigned int pad, u32 which)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &vidsw->format_mbus[pad];
	default:
		return NULL;
	}
}

static int vidsw_get_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *sdformat)
{
	sdformat->format = *__vidsw_get_pad_format(sd, cfg, sdformat->pad,
						   sdformat->which);
	return 0;
}

static int vidsw_set_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *sdformat)
{
	struct vidsw *vidsw = v4l2_subdev_to_vidsw(sd);
	struct v4l2_mbus_framefmt *mbusformat;

	if (sdformat->pad >= vidsw->num_pads)
		return -EINVAL;

	mbusformat = __vidsw_get_pad_format(sd, cfg, sdformat->pad,
					    sdformat->which);
	if (!mbusformat)
		return -EINVAL;

	/* Output pad mirrors active input pad, no limitations on input pads */
	if (sdformat->pad == (vidsw->num_pads - 1) && vidsw->active >= 0)
		sdformat->format = vidsw->format_mbus[vidsw->active];

	*mbusformat = sdformat->format;

	return 0;
}

static int vidsw_link_validate(struct v4l2_subdev *sd,
			       struct media_link *link,
			       struct v4l2_subdev_format *source_fmt,
			       struct v4l2_subdev_format *sink_fmt)
{
	int ret;

	ret = v4l2_subdev_link_validate_default(sd, link,
						source_fmt, sink_fmt);
	if (ret)
		return ret;

	return v4l2_subdev_link_validate_frame_interval(link);
}

static struct v4l2_subdev_pad_ops vidsw_pad_ops = {
	.get_fmt = vidsw_get_format,
	.set_fmt = vidsw_set_format,
	.link_validate = vidsw_link_validate,
};

static struct v4l2_subdev_ops vidsw_subdev_ops = {
	.pad = &vidsw_pad_ops,
	.video = &vidsw_subdev_video_ops,
};

static int of_get_reg_field(struct device_node *node, struct reg_field *field)
{
	u32 bit_mask;
	int ret;

	ret = of_property_read_u32(node, "reg", &field->reg);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(node, "bit-mask", &bit_mask);
	if (ret < 0)
		return ret;

	ret = of_property_read_u32(node, "bit-shift", &field->lsb);
	if (ret < 0)
		return ret;

	field->msb = field->lsb + fls(bit_mask) - 1;

	return 0;
}

static int vidsw_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct of_endpoint endpoint;
	struct device_node *ep;
	struct reg_field field;
	struct vidsw *vidsw;
	struct regmap *map;
	unsigned int num_pads;
	int ret;

	vidsw = devm_kzalloc(&pdev->dev, sizeof(*vidsw), GFP_KERNEL);
	if (!vidsw)
		return -ENOMEM;

	platform_set_drvdata(pdev, vidsw);

	v4l2_subdev_init(&vidsw->subdev, &vidsw_subdev_ops);
	snprintf(vidsw->subdev.name, sizeof(vidsw->subdev.name), "%s",
			np->name);
	vidsw->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vidsw->subdev.dev = &pdev->dev;

	/* init default frame interval */
	vidsw->timeperframe.numerator = 1;
	vidsw->timeperframe.denominator = 30;

	/*
	 * The largest numbered port is the output port. It determines
	 * total number of pads
	 */
	num_pads = 0;
	for_each_endpoint_of_node(np, ep) {
		of_graph_parse_endpoint(ep, &endpoint);
		num_pads = max(num_pads, endpoint.port + 1);
	}

	if (num_pads < 2) {
		dev_err(&pdev->dev, "Not enough ports %d\n", num_pads);
		return -EINVAL;
	}

	ret = of_get_reg_field(np, &field);
	if (ret == 0) {
		map = syscon_node_to_regmap(np->parent);
		if (!map) {
			dev_err(&pdev->dev, "Failed to get syscon register map\n");
			return PTR_ERR(map);
		}

		vidsw->field = devm_regmap_field_alloc(&pdev->dev, map, field);
		if (IS_ERR(vidsw->field)) {
			dev_err(&pdev->dev, "Failed to allocate regmap field\n");
			return PTR_ERR(vidsw->field);
		}

		regmap_field_read(vidsw->field, &vidsw->active);
	} else {
		if (num_pads > 3) {
			dev_err(&pdev->dev, "Too many ports %d\n", num_pads);
			return -EINVAL;
		}

		vidsw->gpio = devm_gpiod_get(&pdev->dev, NULL, GPIOD_OUT_LOW);
		if (IS_ERR(vidsw->gpio)) {
			dev_warn(&pdev->dev,
				 "could not request control gpio: %d\n", ret);
			vidsw->gpio = NULL;
		}

		vidsw->active = gpiod_get_value(vidsw->gpio) ? 1 : 0;
	}

	vidsw->num_pads = num_pads;
	vidsw->pads = devm_kzalloc(&pdev->dev, sizeof(*vidsw->pads) * num_pads,
			GFP_KERNEL);
	vidsw->format_mbus = devm_kzalloc(&pdev->dev,
			sizeof(*vidsw->format_mbus) * num_pads, GFP_KERNEL);
	vidsw->endpoint = devm_kzalloc(&pdev->dev,
			sizeof(*vidsw->endpoint) * (num_pads - 1), GFP_KERNEL);

	ret = vidsw_async_init(vidsw, np);
	if (ret)
		return ret;

	return 0;
}

static int vidsw_remove(struct platform_device *pdev)
{
	struct vidsw *vidsw = platform_get_drvdata(pdev);
	struct v4l2_subdev *sd = &vidsw->subdev;

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static const struct of_device_id vidsw_dt_ids[] = {
	{ .compatible = "video-multiplexer", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, vidsw_dt_ids);

static struct platform_driver vidsw_driver = {
	.probe		= vidsw_probe,
	.remove		= vidsw_remove,
	.driver		= {
		.of_match_table = vidsw_dt_ids,
		.name = "video-multiplexer",
	},
};

module_platform_driver(vidsw_driver);

MODULE_DESCRIPTION("video stream multiplexer");
MODULE_AUTHOR("Sascha Hauer, Pengutronix");
MODULE_AUTHOR("Philipp Zabel, Pengutronix");
MODULE_LICENSE("GPL");
