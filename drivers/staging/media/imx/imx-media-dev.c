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
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_platform.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mc.h>
#include <video/imx-ipu-v3.h>
#include <media/imx.h>
#include "imx-media.h"
#include "imx-media-of.h"

#define DEVICE_NAME "imx-media"

static inline struct imx_media_dev *notifier2dev(struct v4l2_async_notifier *n)
{
	return container_of(n, struct imx_media_dev, subdev_notifier);
}

/*
 * Find a subdev by device node or device name. This is called during
 * driver load to form the async subdev list and bind them.
 */
struct imx_media_subdev *
imx_media_find_async_subdev(struct imx_media_dev *imxmd,
			    struct device_node *np,
			    const char *devname)
{
	struct imx_media_subdev *imxsd;
	int i;

	for (i = 0; i < imxmd->subdev_notifier.num_subdevs; i++) {
		imxsd = &imxmd->subdev[i];
		switch (imxsd->asd.match_type) {
		case V4L2_ASYNC_MATCH_OF:
			if (np && imxsd->asd.match.of.node == np)
				return imxsd;
			break;
		case V4L2_ASYNC_MATCH_DEVNAME:
			if (devname &&
			    !strcmp(imxsd->asd.match.device_name.name, devname))
				return imxsd;
			break;
		default:
			break;
		}
	}

	return NULL;
}

/*
 * Adds a subdev to the async subdev list. If np is non-NULL, adds
 * the async as a V4L2_ASYNC_MATCH_OF match type, otherwise as a
 * V4L2_ASYNC_MATCH_DEVNAME match type using devname. This is called
 * during driver load when forming the async subdev list.
 */
struct imx_media_subdev *
imx_media_add_async_subdev(struct imx_media_dev *imxmd,
			   struct device_node *np,
			   const char *devname)
{
	struct imx_media_subdev *imxsd;
	struct v4l2_async_subdev *asd;
	int sd_idx;

	/* return NULL if this subdev already added */
	if (imx_media_find_async_subdev(imxmd, np, devname)) {
		dev_dbg(imxmd->dev, "%s: already added %s\n",
			__func__, np ? np->name : devname);
		return NULL;
	}

	sd_idx = imxmd->subdev_notifier.num_subdevs;
	if (sd_idx >= IMX_MEDIA_MAX_SUBDEVS) {
		dev_err(imxmd->dev, "%s: too many subdevs! can't add %s\n",
			__func__, np ? np->name : devname);
		return ERR_PTR(-ENOSPC);
	}

	imxsd = &imxmd->subdev[sd_idx];

	asd = &imxsd->asd;
	if (np) {
		asd->match_type = V4L2_ASYNC_MATCH_OF;
		asd->match.of.node = np;
	} else {
		asd->match_type = V4L2_ASYNC_MATCH_DEVNAME;
		strncpy(imxsd->devname, devname, sizeof(imxsd->devname));
		asd->match.device_name.name = imxsd->devname;
	}

	imxmd->async_ptrs[sd_idx] = asd;
	imxmd->subdev_notifier.num_subdevs++;

	dev_dbg(imxmd->dev, "%s: added %s, match type %s\n",
		__func__, np ? np->name : devname, np ? "OF" : "DEVNAME");

	return imxsd;
}

/*
 * Adds an imx-media link to a subdev pad's link list. This is called
 * during driver load when forming the links between subdevs.
 *
 * @pad: the local pad
 * @remote_node: the device node of the remote subdev
 * @remote_devname: the device name of the remote subdev
 * @local_pad: local pad index
 * @remote_pad: remote pad index
 */
int imx_media_add_pad_link(struct imx_media_dev *imxmd,
			   struct imx_media_pad *pad,
			   struct device_node *remote_node,
			   const char *remote_devname,
			   int local_pad, int remote_pad)
{
	struct imx_media_link *link;
	int link_idx;

	link_idx = pad->num_links;
	if (link_idx >= IMX_MEDIA_MAX_LINKS) {
		dev_err(imxmd->dev, "%s: too many links!\n", __func__);
		return -ENOSPC;
	}

	link = &pad->link[link_idx];

	link->remote_sd_node = remote_node;
	if (remote_devname)
		strncpy(link->remote_devname, remote_devname,
			sizeof(link->remote_devname));

	link->local_pad = local_pad;
	link->remote_pad = remote_pad;

	pad->num_links++;

	return 0;
}

/*
 * get IPU from this CSI and add it to the list of IPUs
 * the media driver will control.
 */
static int imx_media_get_ipu(struct imx_media_dev *imxmd,
			     struct v4l2_subdev *csi_sd)
{
	struct ipu_soc *ipu;
	int ipu_id;

	ipu = dev_get_drvdata(csi_sd->dev->parent);
	if (!ipu) {
		v4l2_err(&imxmd->v4l2_dev,
			 "CSI %s has no parent IPU!\n", csi_sd->name);
		return -ENODEV;
	}

	ipu_id = ipu_get_num(ipu);
	if (ipu_id > 1) {
		v4l2_err(&imxmd->v4l2_dev, "invalid IPU id %d!\n", ipu_id);
		return -ENODEV;
	}

	if (!imxmd->ipu[ipu_id])
		imxmd->ipu[ipu_id] = ipu;

	return 0;
}

/* async subdev bound notifier */
static int imx_media_subdev_bound(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *sd,
				  struct v4l2_async_subdev *asd)
{
	struct imx_media_dev *imxmd = notifier2dev(notifier);
	struct imx_media_subdev *imxsd;
	int i, ret = -EINVAL;

	imxsd = imx_media_find_async_subdev(imxmd, sd->dev->of_node,
					    dev_name(sd->dev));
	if (!imxsd)
		goto out;

	imxsd->sd = sd;

	if (sd->grp_id & IMX_MEDIA_GRP_ID_CSI) {
		ret = imx_media_get_ipu(imxmd, sd);
		if (ret)
			return ret;
	} else if (imxsd->num_sink_pads == 0 &&
		   ((sd->entity.flags & (MEDIA_ENT_F_CAM_SENSOR |
					 MEDIA_ENT_F_ATV_DECODER)) ||
		    (sd->entity.function & (MEDIA_ENT_F_CAM_SENSOR |
					    MEDIA_ENT_F_ATV_DECODER)))) {
		/* this is a sensor */
		sd->grp_id = IMX_MEDIA_GRP_ID_SENSOR;

		/* set sensor input names if needed */
		for (i = 0; i < imxsd->input.num; i++) {
			if (strlen(imxsd->input.name[i]))
				continue;
			snprintf(imxsd->input.name[i],
				 sizeof(imxsd->input.name[i]),
				 "%s-%d", sd->name, i);
		}
	}

	ret = 0;
out:
	if (ret)
		v4l2_warn(&imxmd->v4l2_dev,
			  "Received unknown subdev %s\n", sd->name);
	else
		v4l2_info(&imxmd->v4l2_dev,
			  "Registered subdev %s\n", sd->name);

	return ret;
}

/*
 * create the media links from the imx-media pads and their links.
 * Called after all subdevs have registered.
 */
static int imx_media_create_links(struct imx_media_dev *imxmd)
{
	struct imx_media_subdev *local_sd;
	struct imx_media_subdev *remote_sd;
	struct v4l2_subdev *source, *sink;
	struct imx_media_link *link;
	struct imx_media_pad *pad;
	u16 source_pad, sink_pad;
	int num_pads, i, j, k;
	int ret = 0;

	for (i = 0; i < imxmd->num_subdevs; i++) {
		local_sd = &imxmd->subdev[i];
		num_pads = local_sd->num_sink_pads + local_sd->num_src_pads;

		for (j = 0; j < num_pads; j++) {
			pad = &local_sd->pad[j];

			for (k = 0; k < pad->num_links; k++) {
				link = &pad->link[k];

				remote_sd = imx_media_find_async_subdev(
					imxmd, link->remote_sd_node,
					link->remote_devname);
				if (!remote_sd) {
					v4l2_warn(&imxmd->v4l2_dev,
						  "%s: no remote for %s:%d\n",
						  __func__, local_sd->sd->name,
						  link->local_pad);
					continue;
				}

				/* only create the source->sink links */
				if (pad->pad.flags & MEDIA_PAD_FL_SINK)
					continue;

				source = local_sd->sd;
				sink = remote_sd->sd;
				source_pad = link->local_pad;
				sink_pad = link->remote_pad;

				v4l2_info(&imxmd->v4l2_dev,
					  "%s: %s:%d -> %s:%d\n", __func__,
					  source->name, source_pad,
					  sink->name, sink_pad);

				ret = media_create_pad_link(&source->entity,
							    source_pad,
							    &sink->entity,
							    sink_pad,
							    0);
				if (ret) {
					v4l2_err(&imxmd->v4l2_dev,
						 "create_pad_link failed: %d\n",
						 ret);
					goto out;
				}
			}
		}
	}

out:
	return ret;
}

/* async subdev complete notifier */
static int imx_media_probe_complete(struct v4l2_async_notifier *notifier)
{
	struct imx_media_dev *imxmd = notifier2dev(notifier);
	int ret;

	mutex_lock(&imxmd->md.graph_mutex);

	ret = imx_media_create_links(imxmd);
	if (ret)
		goto unlock;

	ret = v4l2_device_register_subdev_nodes(&imxmd->v4l2_dev);
unlock:
	mutex_unlock(&imxmd->md.graph_mutex);
	if (ret)
		return ret;

	return media_device_register(&imxmd->md);
}

static int imx_media_link_notify(struct media_link *link, unsigned int flags,
				 unsigned int notification)
{
	struct media_entity *sink = link->sink->entity;
	struct media_entity_graph *graph;
	struct v4l2_subdev *sink_sd;
	struct imx_media_dev *imxmd;
	int ret = 0;

	if (is_media_entity_v4l2_video_device(sink))
		return 0;
	sink_sd = media_entity_to_v4l2_subdev(sink);
	imxmd = dev_get_drvdata(sink_sd->v4l2_dev->dev);
	graph = &imxmd->link_notify_graph;

	if (notification == MEDIA_DEV_NOTIFY_PRE_LINK_CH) {
		ret = media_entity_graph_walk_init(graph, &imxmd->md);
		if (ret)
			return ret;

		if (!(flags & MEDIA_LNK_FL_ENABLED)) {
			/* Before link disconnection */
			ret = imx_media_pipeline_set_power(imxmd, graph,
							   sink, false);
		}
	} else if (notification == MEDIA_DEV_NOTIFY_POST_LINK_CH) {
		if (link->flags & MEDIA_LNK_FL_ENABLED) {
			/* After link activation */
			ret = imx_media_pipeline_set_power(imxmd, graph,
							   sink, true);
		}

		media_entity_graph_walk_cleanup(graph);
	}

	return ret ? -EPIPE : 0;
}

static const struct media_device_ops imx_media_md_ops = {
	.link_notify = imx_media_link_notify,
};

static int imx_media_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct imx_media_subdev *csi[4];
	struct imx_media_dev *imxmd;
	int ret;

	imxmd = devm_kzalloc(dev, sizeof(*imxmd), GFP_KERNEL);
	if (!imxmd)
		return -ENOMEM;

	imxmd->dev = dev;
	dev_set_drvdata(dev, imxmd);

	strlcpy(imxmd->md.model, DEVICE_NAME, sizeof(imxmd->md.model));
	imxmd->md.ops = &imx_media_md_ops;
	imxmd->md.dev = dev;

	imxmd->v4l2_dev.mdev = &imxmd->md;
	strlcpy(imxmd->v4l2_dev.name, DEVICE_NAME,
		sizeof(imxmd->v4l2_dev.name));

	media_device_init(&imxmd->md);

	ret = v4l2_device_register(dev, &imxmd->v4l2_dev);
	if (ret < 0) {
		v4l2_err(&imxmd->v4l2_dev,
			 "Failed to register v4l2_device: %d\n", ret);
		return ret;
	}

	dev_set_drvdata(imxmd->v4l2_dev.dev, imxmd);

	ret = imx_media_of_parse(imxmd, &csi, node);
	if (ret) {
		v4l2_err(&imxmd->v4l2_dev,
			 "imx_media_of_parse failed with %d\n", ret);
		goto unreg_dev;
	}

	ret = imx_media_add_internal_subdevs(imxmd, csi);
	if (ret) {
		v4l2_err(&imxmd->v4l2_dev,
			 "add_internal_subdevs failed with %d\n", ret);
		goto unreg_dev;
	}

	/* no subdevs? just bail for this media device */
	imxmd->num_subdevs = imxmd->subdev_notifier.num_subdevs;
	if (imxmd->num_subdevs == 0) {
		ret = -ENODEV;
		goto unreg_dev;
	}

	/* prepare the async subdev notifier and register it */
	imxmd->subdev_notifier.subdevs = imxmd->async_ptrs;
	imxmd->subdev_notifier.bound = imx_media_subdev_bound;
	imxmd->subdev_notifier.complete = imx_media_probe_complete;
	ret = v4l2_async_notifier_register(&imxmd->v4l2_dev,
					   &imxmd->subdev_notifier);
	if (ret) {
		v4l2_err(&imxmd->v4l2_dev,
			 "v4l2_async_notifier_register failed with %d\n", ret);
		goto unreg_dev;
	}

	return 0;

unreg_dev:
	v4l2_device_unregister(&imxmd->v4l2_dev);
	return ret;
}

static int imx_media_remove(struct platform_device *pdev)
{
	struct imx_media_dev *imxmd =
		(struct imx_media_dev *)platform_get_drvdata(pdev);

	v4l2_info(&imxmd->v4l2_dev, "Removing " DEVICE_NAME "\n");

	v4l2_async_notifier_unregister(&imxmd->subdev_notifier);
	v4l2_device_unregister(&imxmd->v4l2_dev);
	media_device_unregister(&imxmd->md);
	media_device_cleanup(&imxmd->md);

	return 0;
}

static const struct of_device_id imx_media_dt_ids[] = {
	{ .compatible = "fsl,imx-media" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_media_dt_ids);

static struct platform_driver imx_media_pdrv = {
	.probe		= imx_media_probe,
	.remove		= imx_media_remove,
	.driver		= {
		.name	= DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= imx_media_dt_ids,
	},
};

module_platform_driver(imx_media_pdrv);

MODULE_DESCRIPTION("i.MX5/6 v4l2 media controller driver");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
