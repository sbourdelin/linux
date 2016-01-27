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

#ifndef __V4L2_MC_H__
#define __V4L2_MC_H__

#include <linux/types.h>

struct media_entity;

/**
 * v4l2_pipeline_pm_use - Update the use count of an entity
 * @entity: The entity
 * @use: Use (1) or stop using (0) the entity
 *
 * Update the use count of all entities in the pipeline and power entities on or
 * off accordingly.
 *
 * This function is intended to be called in video node open (use ==
 * 1) and release (use == 0). It uses struct media_entity.use_count to
 * track the power status. The use of this function should be paired
 * with v4l2_pipeline_link_notify().
 *
 * Return 0 on success or a negative error code on failure. Powering entities
 * off is assumed to never fail. No failure can occur when the use parameter is
 * set to 0.
 */
int v4l2_pipeline_pm_use(struct media_entity *entity, int use);


/**
 * v4l2_pipeline_link_notify - Link management notification callback
 * @link: The link
 * @flags: New link flags that will be applied
 * @notification: The link's state change notification type (MEDIA_DEV_NOTIFY_*)
 *
 * React to link management on powered pipelines by updating the use count of
 * all entities in the source and sink sides of the link. Entities are powered
 * on or off accordingly. The use of this function should be paired
 * with v4l2_pipeline_pm_use().
 *
 * Return 0 on success or a negative error code on failure. Powering entities
 * off is assumed to never fail. This function will not fail for disconnection
 * events.
 */
int v4l2_pipeline_link_notify(struct media_link *link, u32 flags,
			      unsigned int notification);


#endif /* __V4L2_MC_H__ */
