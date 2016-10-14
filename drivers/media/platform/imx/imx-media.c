/*
 * i.MX V4L2 Capture Driver
 *
 * Copyright (C) 2016, Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-of.h>

#define IMX_MEDIA_MAX_PORTS	4

struct imx_media {
	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier subdev_notifier;
	struct v4l2_async_subdev subdevs[IMX_MEDIA_MAX_PORTS];
};

static int v4l2_of_create_pad_link(struct v4l2_subdev *sd,
				   struct v4l2_of_link *link)
{
	struct v4l2_subdev *remote_sd, *src_sd, *sink_sd;
	struct media_link *mlink;
	int src_port, sink_port;

	if (link->local_port >= sd->entity.num_pads)
		return -EINVAL;

	remote_sd = v4l2_find_subdev_by_node(sd->v4l2_dev, link->remote_node);
	if (!remote_sd)
		return 0;

	if (sd->entity.pads[link->local_port].flags & MEDIA_PAD_FL_SINK) {
		src_sd = remote_sd;
		src_port = link->remote_port;
		sink_sd = sd;
		sink_port = link->local_port;
	} else {
		src_sd = sd;
		src_port = link->local_port;
		sink_sd = remote_sd;
		sink_port = link->remote_port;
	}

	mlink = media_entity_find_link(&src_sd->entity.pads[src_port],
				       &sink_sd->entity.pads[sink_port]);
	if (mlink)
		return 0;

	return media_create_pad_link(&src_sd->entity, src_port,
			&sink_sd->entity, sink_port, 0);
}

static int imx_media_complete(struct v4l2_async_notifier *notifier)
{
	struct imx_media *im = container_of(notifier, struct imx_media,
					    subdev_notifier);
	struct media_device *mdev = &im->mdev;
	struct v4l2_subdev *sd;
	struct device_node *ep;

	mutex_lock(&mdev->graph_mutex);

	/*
	 * Link all subdevices
	 */
	list_for_each_entry(sd, &notifier->done, async_list) {
		struct v4l2_of_link link;
		int ret;

		/*
		 * The IPU port nodes 0 and 1 correspond to the CSI subdevices.
		 * We can't use for_each_endpoint_of_node here, which would
		 * iterate over all endpoints of the IPU, including output ones.
		 */
		if (strcmp(sd->of_node->name, "port") == 0) {
			/* There should be one endpoint in the CSI port */
			ep = of_get_next_child(sd->of_node, NULL);
			if (!ep)
				continue;

			ret = v4l2_of_parse_link(ep, &link);
			if (ret) {
				of_node_put(ep);
				continue;
			}

			/*
			 * The IPU port id in the device tree does not
			 * correspond to the CSI pad id. Always connect
			 * the source to the CSI input pad (pad 0).
			 */
			link.local_port = 0;

			v4l2_of_create_pad_link(sd, &link);

			v4l2_of_put_link(&link);

			continue;
		}

		/*
		 * TODO: for now assume that all subdevices other than CSI have
		 * a 1:1 relationship between device_node and v4l2_subdev and
		 * between of_graph port number and media_entity pad index.
		 */
		for_each_endpoint_of_node(sd->of_node, ep) {
			ret = v4l2_of_parse_link(ep, &link);
			if (ret)
				continue;

			v4l2_of_create_pad_link(sd, &link);

			v4l2_of_put_link(&link);
		}
	}

	mutex_unlock(&mdev->graph_mutex);

	return v4l2_device_register_subdev_nodes(&im->v4l2_dev);
}

static void imx_media_register_notifier(struct imx_media *im)
{
	struct v4l2_async_notifier *notifier = &im->subdev_notifier;
	struct v4l2_async_subdev *asd = &im->subdevs[0];
	struct device_node *port;
	int i;

	/*
	 * ports correspond to the CSI subdevices that terminate the media
	 * pipelines.
	 */
	for (i = 0; i < IMX_MEDIA_MAX_PORTS; i++) {
		port = of_parse_phandle(im->mdev.dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		asd[i].match_type = V4L2_ASYNC_MATCH_OF;
		asd[i].match.of.node = port;
		of_node_put(port);
	}
	notifier->num_subdevs = i;
	notifier->subdevs = devm_kcalloc(im->mdev.dev, i,
					 sizeof(*notifier->subdevs),
					 GFP_KERNEL);
	for (i = 0; i < notifier->num_subdevs; i++)
		notifier->subdevs[i] = &im->subdevs[i];

	notifier->complete = imx_media_complete;

	v4l2_async_notifier_register(&im->v4l2_dev, notifier);
}

static int imx_media_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct media_device *mdev;
	struct imx_media *im;
	int ret;

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	im = devm_kzalloc(dev, sizeof(*im), GFP_KERNEL);
	if (!im)
		return -ENOMEM;

	mdev = &im->mdev;
	mdev->dev = dev;
	strlcpy(mdev->model, "i.MX IPUv3", sizeof(mdev->model));
	media_device_init(mdev);

	im->v4l2_dev.mdev = mdev;
	ret = v4l2_device_register(dev, &im->v4l2_dev);
	if (ret) {
		dev_err(dev, "Failed to register v4l2 device: %d\n", ret);
		return ret;
	}

	imx_media_register_notifier(im);

	ret = media_device_register(mdev);
	if (ret) {
		dev_err(dev, "Failed to register media controller device: %d\n",
			ret);
		goto err;
	}

	platform_set_drvdata(pdev, mdev);

	return 0;

err:
	media_device_cleanup(mdev);
	return ret;
}

static int imx_media_remove(struct platform_device *pdev)
{
	struct media_device *mdev = platform_get_drvdata(pdev);
	struct imx_media *im = container_of(mdev, struct imx_media, mdev);

	media_device_unregister(mdev);
	v4l2_device_unregister(&im->v4l2_dev);
	media_device_cleanup(mdev);

	return 0;
}

static const struct of_device_id imx_media_dt_ids[] = {
	{ .compatible = "fsl,imx-capture-subsystem", },
	{ },
};
MODULE_DEVICE_TABLE(of, imx_media_dt_ids);

static struct platform_driver imx_media_driver = {
	.probe = imx_media_probe,
	.remove = imx_media_remove,
	.driver = {
		.name = "imx-media",
		.of_match_table = imx_media_dt_ids,
	},
};
module_platform_driver(imx_media_driver);

MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_AUTHOR("Philipp Zabel <p.zabel@pengutronix.de>");
MODULE_DESCRIPTION("i.MX SoC wide media device");
MODULE_LICENSE("GPL");
