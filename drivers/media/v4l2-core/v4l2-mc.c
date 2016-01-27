/*
 * V4L2 Media controller support
 *
 * Copyright (C) 2006--2010 Nokia Corporation
 * Copyright (c) 2016 Intel Corporation.
 *
 * Author: Sakari Ailus <sakari.ailus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-subdev.h>
#include <linux/module.h>

/* -----------------------------------------------------------------------------
 * Pipeline power management
 *
 * Entities must be powered up when part of a pipeline that contains at least
 * one open video device node.
 *
 * To achieve this use the entity use_count field to track the number of users.
 * For entities corresponding to video device nodes the use_count field stores
 * the users count of the node. For entities corresponding to subdevs the
 * use_count field stores the total number of users of all video device nodes
 * in the pipeline.
 *
 * The v4l2_pipeline_pm_use() function must be called in the open() and
 * close() handlers of video device nodes. It increments or decrements the use
 * count of all subdev entities in the pipeline.
 *
 * To react to link management on powered pipelines, the link setup notification
 * callback updates the use count of all entities in the source and sink sides
 * of the link.
 */

/*
 * pipeline_pm_use_count - Count the number of users of a pipeline
 * @entity: The entity
 *
 * Return the total number of users of all video device nodes in the pipeline.
 */
static int pipeline_pm_use_count(struct media_entity *entity,
	struct media_entity_graph *graph)
{
	int use = 0;

	media_entity_graph_walk_start(graph, entity);

	while ((entity = media_entity_graph_walk_next(graph))) {
		if (is_media_entity_v4l2_io(entity))
			use += entity->use_count;
	}

	return use;
}

/*
 * pipeline_pm_power_one - Apply power change to an entity
 * @entity: The entity
 * @change: Use count change
 *
 * Change the entity use count by @change. If the entity is a subdev update its
 * power state by calling the core::s_power operation when the use count goes
 * from 0 to != 0 or from != 0 to 0.
 *
 * Return 0 on success or a negative error code on failure.
 */
static int pipeline_pm_power_one(struct media_entity *entity, int change)
{
	struct v4l2_subdev *subdev;
	int ret;

	subdev = is_media_entity_v4l2_subdev(entity)
	       ? media_entity_to_v4l2_subdev(entity) : NULL;

	if (entity->use_count == 0 && change > 0 && subdev != NULL) {
		ret = v4l2_subdev_call(subdev, core, s_power, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			return ret;
	}

	entity->use_count += change;
	WARN_ON(entity->use_count < 0);

	if (entity->use_count == 0 && change < 0 && subdev != NULL)
		v4l2_subdev_call(subdev, core, s_power, 0);

	return 0;
}

/*
 * pipeline_pm_power - Apply power change to all entities in a pipeline
 * @entity: The entity
 * @change: Use count change
 *
 * Walk the pipeline to update the use count and the power state of all non-node
 * entities.
 *
 * Return 0 on success or a negative error code on failure.
 */
static int pipeline_pm_power(struct media_entity *entity, int change,
	struct media_entity_graph *graph)
{
	struct media_entity *first = entity;
	int ret = 0;

	if (!change)
		return 0;

	media_entity_graph_walk_start(graph, entity);

	while (!ret && (entity = media_entity_graph_walk_next(graph)))
		if (is_media_entity_v4l2_subdev(entity))
			ret = pipeline_pm_power_one(entity, change);

	if (!ret)
		return ret;

	media_entity_graph_walk_start(graph, first);

	while ((first = media_entity_graph_walk_next(graph))
	       && first != entity)
		if (is_media_entity_v4l2_subdev(first))
			pipeline_pm_power_one(first, -change);

	return ret;
}

int v4l2_pipeline_pm_use(struct media_entity *entity, int use)
{
	struct media_device *mdev = entity->graph_obj.mdev;
	int change = use ? 1 : -1;
	int ret;

	mutex_lock(&mdev->graph_mutex);

	/* Apply use count to node. */
	entity->use_count += change;
	WARN_ON(entity->use_count < 0);

	/* Apply power change to connected non-nodes. */
	ret = pipeline_pm_power(entity, change, &mdev->pm_count_walk);
	if (ret < 0)
		entity->use_count -= change;

	mutex_unlock(&mdev->graph_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(v4l2_pipeline_pm_use);

int v4l2_pipeline_link_notify(struct media_link *link, u32 flags,
			      unsigned int notification)
{
	struct media_entity_graph *graph = &link->graph_obj.mdev->pm_count_walk;
	struct media_entity *source = link->source->entity;
	struct media_entity *sink = link->sink->entity;
	int source_use;
	int sink_use;
	int ret = 0;

	source_use = pipeline_pm_use_count(source, graph);
	sink_use = pipeline_pm_use_count(sink, graph);

	if (notification == MEDIA_DEV_NOTIFY_POST_LINK_CH &&
	    !(flags & MEDIA_LNK_FL_ENABLED)) {
		/* Powering off entities is assumed to never fail. */
		pipeline_pm_power(source, -sink_use, graph);
		pipeline_pm_power(sink, -source_use, graph);
		return 0;
	}

	if (notification == MEDIA_DEV_NOTIFY_PRE_LINK_CH &&
		(flags & MEDIA_LNK_FL_ENABLED)) {

		ret = pipeline_pm_power(source, sink_use, graph);
		if (ret < 0)
			return ret;

		ret = pipeline_pm_power(sink, source_use, graph);
		if (ret < 0)
			pipeline_pm_power(source, -sink_use, graph);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(v4l2_pipeline_link_notify);

MODULE_AUTHOR("Sakari Ailus <sakari.ailus@linux.intel.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("V4L2 Media controller support");
