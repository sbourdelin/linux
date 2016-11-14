/**
 * DWC MIPI CSI-2 Host IPK platform device driver
 *
 * Based on Omnivision OV7670 Camera Driver
 * Copyright (C) 2011 - 2013 Samsung Electronics Co., Ltd.
 * Author: Sylwester Nawrocki <s.nawrocki@samsung.com>
 *
 * Copyright (C) 2016 Synopsys, Inc. All rights reserved.
 * Author: Ramiro Oliveira <ramiro.oliveira@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */

#include "plat_ipk.h"
#include "video_device.h"
#include "dw_mipi_csi.h"
#include <media/v4l2-of.h>
#include <media/v4l2-subdev.h>

static int
__plat_ipk_pipeline_s_format(struct plat_ipk_media_pipeline *ep,
			     struct v4l2_subdev_format *fmt)
{

	struct plat_ipk_pipeline *p = to_plat_ipk_pipeline(ep);
	static const u8 seq[IDX_MAX] = {IDX_SENSOR, IDX_CSI, IDX_VDEV};

	fmt->which = V4L2_SUBDEV_FORMAT_ACTIVE;
	v4l2_subdev_call(p->subdevs[seq[IDX_CSI]], pad, set_fmt, NULL, fmt);

	return 0;
}

static void
plat_ipk_pipeline_prepare(struct plat_ipk_pipeline *p, struct media_entity *me)
{
	struct v4l2_subdev *sd;
	int i = 0;

	for (i = 0; i < IDX_MAX; i++)
		p->subdevs[i] = NULL;

	while (1) {
		struct media_pad *pad = NULL;

		for (i = 0; i < me->num_pads; i++) {
			struct media_pad *spad = &me->pads[i];

			if (!(spad->flags & MEDIA_PAD_FL_SINK))
				continue;

			pad = media_entity_remote_pad(spad);
			if (pad)
				break;
		}
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			break;

		sd = media_entity_to_v4l2_subdev(pad->entity);

		switch (sd->grp_id) {
		case GRP_ID_SENSOR:
			p->subdevs[IDX_SENSOR] = sd;
			break;
		case GRP_ID_CSI:
			p->subdevs[IDX_CSI] = sd;
			break;
		case GRP_ID_VIDEODEV:
			p->subdevs[IDX_VDEV] = sd;
			break;
		default:
			break;
		}
		me = &sd->entity;
		if (me->num_pads == 1)
			break;
	}
}

static int __subdev_set_power(struct v4l2_subdev *sd, int on)
{
	int *use_count;
	int ret;

	if (sd == NULL) {
		pr_info("null subdev\n");
		return -ENXIO;
	}
	use_count = &sd->entity.use_count;
	if (on && (*use_count)++ > 0)
		return 0;
	else if (!on && (*use_count == 0 || --(*use_count) > 0))
		return 0;

	pr_debug("%d %s !\n", on, sd->entity.name);
	ret = v4l2_subdev_call(sd, core, s_power, on);

	return ret != -ENOIOCTLCMD ? ret : 0;
}

static int plat_ipk_pipeline_s_power(struct plat_ipk_pipeline *p, bool on)
{
	static const u8 seq[IDX_MAX] = {IDX_CSI, IDX_SENSOR, IDX_VDEV};
	int i, ret = 0;

	for (i = 0; i < IDX_MAX; i++) {
		unsigned int idx = seq[i];

		if (p->subdevs[idx] == NULL)
			pr_info("No device registered on %d\n", idx);
		else {
			ret = __subdev_set_power(p->subdevs[idx], on);
			if (ret < 0 && ret != -ENXIO)
				goto error;
		}
	}
	return 0;
error:
	for (; i >= 0; i--) {
		unsigned int idx = seq[i];

		__subdev_set_power(p->subdevs[idx], !on);
	}
	return ret;
}

static int
__plat_ipk_pipeline_open(struct plat_ipk_media_pipeline *ep,
			 struct media_entity *me, bool prepare)
{
	struct plat_ipk_pipeline *p = to_plat_ipk_pipeline(ep);
	int ret;

	if (WARN_ON(p == NULL || me == NULL))
		return -EINVAL;

	if (prepare)
		plat_ipk_pipeline_prepare(p, me);

	ret = plat_ipk_pipeline_s_power(p, 1);
	if (!ret)
		return 0;

	return ret;
}

static int __plat_ipk_pipeline_close(struct plat_ipk_media_pipeline *ep)
{
	struct plat_ipk_pipeline *p = to_plat_ipk_pipeline(ep);
	int ret;

	ret = plat_ipk_pipeline_s_power(p, 0);

	return ret == -ENXIO ? 0 : ret;
}

static int
__plat_ipk_pipeline_s_stream(struct plat_ipk_media_pipeline *ep, bool on)
{
	static const u8 seq[IDX_MAX] = {IDX_SENSOR, IDX_CSI, IDX_VDEV};
	struct plat_ipk_pipeline *p = to_plat_ipk_pipeline(ep);
	int i, ret = 0;

	for (i = 0; i < IDX_MAX; i++) {
		unsigned int idx = seq[i];

		if (p->subdevs[idx] == NULL)
			pr_debug("No device registered on %d\n", idx);
		else {
			ret =
			    v4l2_subdev_call(p->subdevs[idx], video, s_stream,
					     on);

			if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
				goto error;
		}
	}
	return 0;
error:
	for (; i >= 0; i--) {
		unsigned int idx = seq[i];

		v4l2_subdev_call(p->subdevs[idx], video, s_stream, !on);
	}
	return ret;
}

static const struct plat_ipk_media_pipeline_ops plat_ipk_pipeline_ops = {
	.open = __plat_ipk_pipeline_open,
	.close = __plat_ipk_pipeline_close,
	.set_format = __plat_ipk_pipeline_s_format,
	.set_stream = __plat_ipk_pipeline_s_stream,
};

static struct plat_ipk_media_pipeline *
plat_ipk_pipeline_create(struct plat_ipk_dev *plat_ipk)
{
	struct plat_ipk_pipeline *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;

	list_add_tail(&p->list, &plat_ipk->pipelines);

	p->ep.ops = &plat_ipk_pipeline_ops;
	return &p->ep;
}

static void
plat_ipk_pipelines_free(struct plat_ipk_dev *plat_ipk)
{
	while (!list_empty(&plat_ipk->pipelines)) {
		struct plat_ipk_pipeline *p;

		p = list_entry(plat_ipk->pipelines.next, typeof(*p), list);
		list_del(&p->list);
		kfree(p);
	}
}

static int
plat_ipk_parse_port_node(struct plat_ipk_dev *plat_ipk,
			 struct device_node *port, unsigned int index)
{
	struct device_node *rem, *ep;
	struct v4l2_of_endpoint endpoint;
	struct plat_ipk_source_info *pd = &plat_ipk->sensor[index].pdata;

	/* Assume here a port node can have only one endpoint node. */
	ep = of_get_next_child(port, NULL);
	if (!ep)
		return 0;

	v4l2_of_parse_endpoint(ep, &endpoint);
	if (WARN_ON(endpoint.base.port == 0) || index >= PLAT_MAX_SENSORS)
		return -EINVAL;

	pd->mux_id = endpoint.base.port - 1;

	rem = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);
	if (rem == NULL) {
		v4l2_info(&plat_ipk->v4l2_dev,
			  "Remote device at %s not found\n", ep->full_name);
		return 0;
	}

	if (WARN_ON(index >= ARRAY_SIZE(plat_ipk->sensor)))
		return -EINVAL;

	plat_ipk->sensor[index].asd.match_type = V4L2_ASYNC_MATCH_OF;
	plat_ipk->sensor[index].asd.match.of.node = rem;
	plat_ipk->async_subdevs[index] = &plat_ipk->sensor[index].asd;

	plat_ipk->num_sensors++;

	of_node_put(rem);
	return 0;
}


static int plat_ipk_register_sensor_entities(struct plat_ipk_dev *plat_ipk)
{
	struct device_node *parent = plat_ipk->pdev->dev.of_node;
	struct device_node *node;
	int index = 0;
	int ret;

	plat_ipk->num_sensors = 0;

	for_each_available_child_of_node(parent, node) {
		struct device_node *port;

		if (of_node_cmp(node->name, "csi2"))
			continue;
		port = of_get_next_child(node, NULL);
		if (!port)
			continue;

		ret = plat_ipk_parse_port_node(plat_ipk, port, index);
		if (ret < 0)
			return ret;
		index++;
	}
	return 0;
}

static int
__of_get_port_id(struct device_node *np)
{
	u32 reg = 0;

	np = of_get_child_by_name(np, "port");
	if (!np)
		return -EINVAL;
	of_property_read_u32(np, "reg", &reg);

	return reg - 1;
}

static int register_videodev_entity(struct plat_ipk_dev *plat_ipk,
			 struct video_device_dev *vid_dev)
{
	struct v4l2_subdev *sd;
	struct plat_ipk_media_pipeline *ep;
	int ret;

	sd = &vid_dev->subdev;
	sd->grp_id = GRP_ID_VIDEODEV;

	ep = plat_ipk_pipeline_create(plat_ipk);
	if (!ep)
		return -ENOMEM;

	v4l2_set_subdev_hostdata(sd, ep);

	ret = v4l2_device_register_subdev(&plat_ipk->v4l2_dev, sd);
	if (!ret)
		plat_ipk->vid_dev = vid_dev;
	else
		v4l2_err(&plat_ipk->v4l2_dev,
			 "Failed to register Video Device\n");
	return ret;
}

static int register_mipi_csi_entity(struct plat_ipk_dev *plat_ipk,
			 struct platform_device *pdev, struct v4l2_subdev *sd)
{
	struct device_node *node = pdev->dev.of_node;
	int id, ret;

	id = node ? __of_get_port_id(node) : max(0, pdev->id);

	if (WARN_ON(id < 0 || id >= CSI_MAX_ENTITIES))
		return -ENOENT;

	if (WARN_ON(plat_ipk->mipi_csi[id].sd))
		return -EBUSY;

	sd->grp_id = GRP_ID_CSI;
	ret = v4l2_device_register_subdev(&plat_ipk->v4l2_dev, sd);

	if (!ret)
		plat_ipk->mipi_csi[id].sd = sd;
	else
		v4l2_err(&plat_ipk->v4l2_dev,
			 "Failed to register MIPI-CSI.%d (%d)\n", id, ret);
	return ret;
}

static int plat_ipk_register_platform_entity(struct plat_ipk_dev *plat_ipk,
				struct platform_device *pdev, int plat_entity)
{
	struct device *dev = &pdev->dev;
	int ret = -EPROBE_DEFER;
	void *drvdata;


	device_lock(dev);
	if (!dev->driver || !try_module_get(dev->driver->owner))
		goto dev_unlock;

	drvdata = dev_get_drvdata(dev);

	if (drvdata) {
		switch (plat_entity) {
		case IDX_VDEV:
			ret = register_videodev_entity(plat_ipk, drvdata);
			break;
		case IDX_CSI:
			ret = register_mipi_csi_entity(plat_ipk, pdev, drvdata);
			break;
		default:
			ret = -ENODEV;
		}
	} else
		dev_err(&plat_ipk->pdev->dev, "%s no drvdata\n", dev_name(dev));
	module_put(dev->driver->owner);
dev_unlock:
	device_unlock(dev);
	if (ret == -EPROBE_DEFER)
		dev_info(&plat_ipk->pdev->dev,
			 "deferring %s device registration\n", dev_name(dev));
	else if (ret < 0)
		dev_err(&plat_ipk->pdev->dev,
			"%s device registration failed (%d)\n", dev_name(dev),
			ret);
	return ret;
}

static int
plat_ipk_register_platform_entities(struct plat_ipk_dev *plat_ipk,
				    struct device_node *parent)
{
	struct device_node *node;
	int ret = 0;

	for_each_available_child_of_node(parent, node) {
		struct platform_device *pdev;
		int plat_entity = -1;

		pdev = of_find_device_by_node(node);
		if (!pdev)
			continue;

		if (!strcmp(node->name, VIDEODEV_OF_NODE_NAME))
			plat_entity = IDX_VDEV;
		else if (!strcmp(node->name, CSI_OF_NODE_NAME))
			plat_entity = IDX_CSI;

		if (plat_entity >= 0)
			ret = plat_ipk_register_platform_entity(plat_ipk, pdev,
								plat_entity);
		put_device(&pdev->dev);
		if (ret < 0)
			break;
	}

	return ret;
}

static void
plat_ipk_unregister_entities(struct plat_ipk_dev *plat_ipk)
{
	int i;
	struct video_device_dev *dev = plat_ipk->vid_dev;

	if (dev == NULL)
		return;
	v4l2_device_unregister_subdev(&dev->subdev);
	dev->ve.pipe = NULL;
	plat_ipk->vid_dev = NULL;

	for (i = 0; i < CSI_MAX_ENTITIES; i++) {
		if (plat_ipk->mipi_csi[i].sd == NULL)
			continue;
		v4l2_device_unregister_subdev(plat_ipk->mipi_csi[i].sd);
		plat_ipk->mipi_csi[i].sd = NULL;
	}

	v4l2_info(&plat_ipk->v4l2_dev, "Unregistered all entities\n");
}

static int
__plat_ipk_create_videodev_sink_links(struct plat_ipk_dev *plat_ipk,
				      struct media_entity *source,
				      int pad)
{
	struct media_entity *sink;
	int ret = 0;

	if (!plat_ipk->vid_dev)
		return 0;

	sink = &plat_ipk->vid_dev->subdev.entity;
	ret = media_create_pad_link(source, pad, sink,
				    CSI_PAD_SOURCE, MEDIA_LNK_FL_ENABLED);
	if (ret)
		return ret;

	ret = media_entity_call(sink, link_setup, &sink->pads[0],
				&source->pads[pad], 0);
	if (ret)
		return 0;

	v4l2_info(&plat_ipk->v4l2_dev, "created link [%s] -> [%s]\n",
		  source->name, sink->name);

	return 0;
}


static int
__plat_ipk_create_videodev_source_links(struct plat_ipk_dev *plat_ipk)
{
	struct media_entity *source, *sink;
	int ret = 0;

	struct video_device_dev *vid_dev = plat_ipk->vid_dev;

	if (vid_dev == NULL)
		return -ENODEV;

	source = &vid_dev->subdev.entity;
	sink = &vid_dev->ve.vdev.entity;

	ret = media_create_pad_link(source, VIDEO_DEV_SD_PAD_SOURCE_DMA,
				    sink, 0, MEDIA_LNK_FL_ENABLED);

	v4l2_info(&plat_ipk->v4l2_dev, "created link [%s] -> [%s]\n",
		  source->name, sink->name);
	return ret;
}

static int
plat_ipk_create_links(struct plat_ipk_dev *plat_ipk)
{
	struct v4l2_subdev *csi_sensor[CSI_MAX_ENTITIES] = { NULL };
	struct v4l2_subdev *sensor, *csi;
	struct media_entity *source;
	struct plat_ipk_source_info *pdata;
	int i, pad, ret = 0;

	for (i = 0; i < plat_ipk->num_sensors; i++) {
		if (plat_ipk->sensor[i].subdev == NULL)
			continue;

		sensor = plat_ipk->sensor[i].subdev;
		pdata = v4l2_get_subdev_hostdata(sensor);
		if (!pdata)
			continue;

		source = NULL;

		csi = plat_ipk->mipi_csi[pdata->mux_id].sd;
		if (WARN(csi == NULL, "MIPI-CSI interface specified but	dw-mipi-csi module is not loaded!\n"))
			return -EINVAL;

		pad = sensor->entity.num_pads - 1;
		ret = media_create_pad_link(&sensor->entity, pad,
					    &csi->entity, CSI_PAD_SINK,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);

		if (ret)
			return ret;
		v4l2_info(&plat_ipk->v4l2_dev, "created link [%s] -> [%s]\n",
			  sensor->entity.name, csi->entity.name);

		csi_sensor[pdata->mux_id] = sensor;
	}

	for (i = 0; i < CSI_MAX_ENTITIES; i++) {
		if (plat_ipk->mipi_csi[i].sd == NULL) {
			pr_info("no link\n");
			continue;
		}

		source = &plat_ipk->mipi_csi[i].sd->entity;
		pad = VIDEO_DEV_SD_PAD_SINK_CSI;

		ret = __plat_ipk_create_videodev_sink_links(plat_ipk, source,
								pad);
	}

	ret = __plat_ipk_create_videodev_source_links(plat_ipk);
	if (ret < 0)
		return ret;

	return ret;
}

static int __plat_ipk_modify_pipeline(struct media_entity *entity, bool enable)
{
	struct plat_ipk_video_entity *ve;
	struct plat_ipk_pipeline *p;
	struct video_device *vdev;
	int ret;

	vdev = media_entity_to_video_device(entity);

	if (vdev->entity.use_count == 0)
		return 0;

	ve = vdev_to_plat_ipk_video_entity(vdev);
	p = to_plat_ipk_pipeline(ve->pipe);

	if (enable)
		ret = __plat_ipk_pipeline_open(ve->pipe, entity, true);
	else
		ret = __plat_ipk_pipeline_close(ve->pipe);

	if (ret == 0 && !enable)
		memset(p->subdevs, 0, sizeof(p->subdevs));

	return ret;
}


static int
__plat_ipk_modify_pipelines(struct media_entity *entity, bool enable,
			    struct media_entity_graph *graph)
{
	struct media_entity *entity_err = entity;
	int ret;

	media_entity_graph_walk_start(graph, entity);

	while ((entity = media_entity_graph_walk_next(graph))) {
		if (!is_media_entity_v4l2_video_device(entity))
			continue;

		ret = __plat_ipk_modify_pipeline(entity, enable);

		if (ret < 0)
			goto err;
	}

	return 0;

err:
	media_entity_graph_walk_start(graph, entity_err);

	while ((entity_err = media_entity_graph_walk_next(graph))) {
		if (!is_media_entity_v4l2_video_device(entity_err))
			continue;

		__plat_ipk_modify_pipeline(entity_err, !enable);

		if (entity_err == entity)
			break;
	}

	return ret;
}

static int
plat_ipk_link_notify(struct media_link *link, unsigned int flags,
		     unsigned int notification)
{
	struct media_entity_graph *graph =
	    &container_of(link->graph_obj.mdev, struct plat_ipk_dev,
			  media_dev)->link_setup_graph;
	struct media_entity *sink = link->sink->entity;
	int ret = 0;

	pr_debug("Link notify\n");

	if (notification == MEDIA_DEV_NOTIFY_PRE_LINK_CH) {
		ret = media_entity_graph_walk_init(graph, link->graph_obj.mdev);
		if (ret)
			return ret;
		if (!(flags & MEDIA_LNK_FL_ENABLED))
			ret = __plat_ipk_modify_pipelines(sink, false, graph);

	} else if (notification == MEDIA_DEV_NOTIFY_POST_LINK_CH) {
		if (link->flags & MEDIA_LNK_FL_ENABLED)
			ret = __plat_ipk_modify_pipelines(sink, true, graph);
		media_entity_graph_walk_cleanup(graph);
	}

	return ret ? -EPIPE : 0;
}
static const struct media_device_ops plat_ipk_media_ops = {
	.link_notify = plat_ipk_link_notify,
};


static int
subdev_notifier_bound(struct v4l2_async_notifier *notifier,
		      struct v4l2_subdev *subdev, struct v4l2_async_subdev *asd)
{
	struct plat_ipk_dev *plat_ipk = notifier_to_plat_ipk(notifier);
	struct plat_ipk_sensor_info *si = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(plat_ipk->sensor); i++)
		if (plat_ipk->sensor[i].asd.match.of.node ==
		    subdev->dev->of_node)
			si = &plat_ipk->sensor[i];

	if (si == NULL)
		return -EINVAL;

	v4l2_set_subdev_hostdata(subdev, &si->pdata);

	subdev->grp_id = GRP_ID_SENSOR;

	si->subdev = subdev;

	v4l2_info(&plat_ipk->v4l2_dev, "Registered sensor subdevice: %s (%d)\n",
		  subdev->name, plat_ipk->num_sensors);

	plat_ipk->num_sensors++;

	return 0;
}

static int
subdev_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct plat_ipk_dev *plat_ipk = notifier_to_plat_ipk(notifier);
	int ret;

	mutex_lock(&plat_ipk->media_dev.graph_mutex);

	ret = plat_ipk_create_links(plat_ipk);
	if (ret < 0)
		goto unlock;

	ret = v4l2_device_register_subdev_nodes(&plat_ipk->v4l2_dev);
unlock:
	mutex_unlock(&plat_ipk->media_dev.graph_mutex);
	if (ret < 0)
		return ret;

	return media_device_register(&plat_ipk->media_dev);
}

static const struct of_device_id plat_ipk_of_match[];

static int plat_ipk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_device *v4l2_dev;
	struct plat_ipk_dev *plat_ipk;
	int ret;

	dev_info(dev, "Installing DW MIPI CSI-2 IPK Platform module\n");

	plat_ipk = devm_kzalloc(dev, sizeof(*plat_ipk), GFP_KERNEL);
	if (!plat_ipk)
		return -ENOMEM;

	spin_lock_init(&plat_ipk->slock);
	INIT_LIST_HEAD(&plat_ipk->pipelines);
	plat_ipk->pdev = pdev;

	strlcpy(plat_ipk->media_dev.model, "SNPS IPK Platform",
		sizeof(plat_ipk->media_dev.model));
	plat_ipk->media_dev.ops = &plat_ipk_media_ops;
	plat_ipk->media_dev.dev = dev;

	v4l2_dev = &plat_ipk->v4l2_dev;
	v4l2_dev->mdev = &plat_ipk->media_dev;
	strlcpy(v4l2_dev->name, "plat-ipk", sizeof(v4l2_dev->name));

	media_device_init(&plat_ipk->media_dev);

	ret = v4l2_device_register(dev, &plat_ipk->v4l2_dev);
	if (ret < 0) {
		v4l2_err(v4l2_dev, "Failed to register v4l2_device: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, plat_ipk);

	ret = plat_ipk_register_platform_entities(plat_ipk, dev->of_node);
	if (ret)
		goto err_m_ent;

	ret = plat_ipk_register_sensor_entities(plat_ipk);
	if (ret)
		goto err_m_ent;

	if (plat_ipk->num_sensors > 0) {
		plat_ipk->subdev_notifier.subdevs = plat_ipk->async_subdevs;
		plat_ipk->subdev_notifier.num_subdevs = plat_ipk->num_sensors;
		plat_ipk->subdev_notifier.bound = subdev_notifier_bound;
		plat_ipk->subdev_notifier.complete = subdev_notifier_complete;
		plat_ipk->num_sensors = 0;

		ret = v4l2_async_notifier_register(&plat_ipk->v4l2_dev,
						   &plat_ipk->subdev_notifier);
		if (ret)
			goto err_m_ent;
	}

	return 0;

err_m_ent:
	plat_ipk_unregister_entities(plat_ipk);
	media_device_unregister(&plat_ipk->media_dev);
	media_device_cleanup(&plat_ipk->media_dev);
	v4l2_device_unregister(&plat_ipk->v4l2_dev);
	return ret;
}

static int plat_ipk_remove(struct platform_device *pdev)
{
	struct plat_ipk_dev *dev = platform_get_drvdata(pdev);

	if (!dev)
		return 0;

	v4l2_async_notifier_unregister(&dev->subdev_notifier);

	v4l2_device_unregister(&dev->v4l2_dev);
	plat_ipk_unregister_entities(dev);
	plat_ipk_pipelines_free(dev);
	media_device_unregister(&dev->media_dev);
	media_device_cleanup(&dev->media_dev);

	dev_info(&pdev->dev, "Driver removed\n");

	return 0;
}

/**
 * @short of_device_id structure
 */
static const struct of_device_id plat_ipk_of_match[] = {
	{.compatible = "snps,plat-ipk"},
	{}
};

MODULE_DEVICE_TABLE(of, plat_ipk_of_match);

/**
 * @short Platform driver structure
 */
static struct platform_driver plat_ipk_pdrv = {
	.remove = plat_ipk_remove,
	.probe = plat_ipk_probe,
	.driver = {
		   .name = "snps,plat-ipk",
		   .owner = THIS_MODULE,
		   .of_match_table = plat_ipk_of_match,
		   },
};

static int __init
plat_ipk_init(void)
{
	request_module("dw-mipi-csi");

	return platform_driver_register(&plat_ipk_pdrv);
}

static void __exit
plat_ipk_exit(void)
{
	platform_driver_unregister(&plat_ipk_pdrv);
}

module_init(plat_ipk_init);
module_exit(plat_ipk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ramiro Oliveira <roliveir@synopsys.com>");
MODULE_DESCRIPTION("Platform driver for MIPI CSI-2 Host IPK");
