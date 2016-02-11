/*
 * v4l2-mc.c - Media Controller V4L2 Common Interfaces
 *
 * Copyright (C) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <media/v4l2-mc.h>
#include <media/media-device.h>
#include <media/videobuf2-core.h>
#include <media/v4l2-fh.h>

int v4l_enable_media_source(struct video_device *vdev)
{
	struct media_device *mdev = vdev->entity.graph_obj.mdev;
	int ret;

	if (!mdev || !mdev->enable_source)
		return 0;
	ret = mdev->enable_source(&vdev->entity, &vdev->pipe);
	if (ret)
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL_GPL(v4l_enable_media_source);

void v4l_disable_media_source(struct video_device *vdev)
{
	struct media_device *mdev = vdev->entity.graph_obj.mdev;

	if (mdev && mdev->disable_source)
		mdev->disable_source(&vdev->entity);
}
EXPORT_SYMBOL_GPL(v4l_disable_media_source);

int v4l_vb2q_enable_media_source(struct vb2_queue *q)
{
	struct v4l2_fh *fh = q->owner;

	return v4l_enable_media_source(fh->vdev);
}
EXPORT_SYMBOL_GPL(v4l_vb2q_enable_media_source);

