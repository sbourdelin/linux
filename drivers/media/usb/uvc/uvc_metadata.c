/*
 *      uvc_metadata.c  --  USB Video Class driver - Metadata handling
 *
 *      Copyright (C) 2016
 *          Guennadi Liakhovetski (guennadi.liakhovetski@intel.com)
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/videodev2.h>

#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "uvcvideo.h"

/* -----------------------------------------------------------------------------
 * videobuf2 Queue Operations
 */

static struct vb2_ops uvc_meta_queue_ops = {
	.queue_setup = uvc_queue_setup,
	.buf_prepare = uvc_buffer_prepare,
	.buf_queue = uvc_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.stop_streaming = uvc_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int meta_v4l2_querycap(struct file *file, void *fh,
			      struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct uvc_streaming *stream = video_get_drvdata(vfh->vdev);

	strlcpy(cap->driver, "uvcvideo", sizeof(cap->driver));
	strlcpy(cap->card, vfh->vdev->name, sizeof(cap->card));
	usb_make_path(stream->dev->udev, cap->bus_info, sizeof(cap->bus_info));

	return 0;
}

static int meta_v4l2_get_format(struct file *file, void *fh,
				struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct v4l2_meta_format *fmt = &format->fmt.meta;

	if (format->type != vfh->vdev->queue->type)
		return -EINVAL;

	memset(fmt, 0, sizeof(*fmt));

	fmt->dataformat = V4L2_META_FMT_UVC;
	fmt->buffersize = UVC_METATADA_BUF_SIZE;

	return 0;
}

static const struct v4l2_ioctl_ops uvc_meta_ioctl_ops = {
	.vidioc_querycap		= meta_v4l2_querycap,
	.vidioc_g_fmt_meta_cap		= meta_v4l2_get_format,
	.vidioc_s_fmt_meta_cap		= meta_v4l2_get_format,
	.vidioc_try_fmt_meta_cap	= meta_v4l2_get_format,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

/* -----------------------------------------------------------------------------
 * V4L2 File Operations
 */

static struct v4l2_file_operations uvc_meta_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

int uvc_meta_register(struct uvc_streaming *stream)
{
	struct uvc_device *dev = stream->dev;
	struct uvc_meta_device *meta = &stream->meta;
	struct video_device *vdev = &meta->vdev;
	struct uvc_video_queue *quvc = &meta->queue;
	struct vb2_queue *queue = &quvc->queue;
	int ret;

	mutex_init(&quvc->mutex);
	spin_lock_init(&quvc->irqlock);
	INIT_LIST_HEAD(&quvc->irqqueue);

	/*
	 * We register metadata device nodes only if the METADATA_NODE quirk is
	 * set and only on interfaces with bulk endpoints. To meaningfully
	 * support interfaces with isochronous endpoints, we need to collect
	 * headers from all payloads, comprising a single frame. For that we
	 * need to know the maximum number of such payloads per frame to be able
	 * to calculate the buffer size. Currently this information is
	 * unavailable. A proposal should be made to the UVC committee to add
	 * this information to camera descriptors.
	 */
	if (!(dev->quirks & UVC_QUIRK_METADATA_NODE) ||
	    dev->quirks & UVC_QUIRK_BUILTIN_ISIGHT)
		return 0;

	vdev->v4l2_dev = &dev->vdev;
	vdev->fops = &uvc_meta_fops;
	vdev->ioctl_ops = &uvc_meta_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->prio = &stream->chain->prio;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->queue = queue;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	strlcpy(vdev->name, dev->name, sizeof(vdev->name));

	video_set_drvdata(vdev, stream);

	/* Initialize the video buffer queue. */
	queue->type = V4L2_BUF_TYPE_META_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_USERPTR;
	queue->drv_priv = quvc;
	queue->buf_struct_size = sizeof(struct uvc_buffer);
	queue->ops = &uvc_meta_queue_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
		| V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	queue->lock = &quvc->mutex;
	ret = vb2_queue_init(queue);
	if (ret < 0)
		return ret;

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		uvc_printk(KERN_ERR, "Failed to register metadata device (%d).\n", ret);

	return ret;
}
