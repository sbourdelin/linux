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

static inline struct uvc_buffer *to_uvc_buffer(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct uvc_buffer, buf);
}

/* -----------------------------------------------------------------------------
 * videobuf2 Queue Operations
 */

/*
 * Actually 253 bytes, but 256 is just a nicer number. We keep the buffer size
 * constant and just set .usedbytes accordingly
 */
#define UVC_PAYLOAD_HEADER_MAX_SIZE 256

static int meta_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			    unsigned int *nplanes, unsigned int sizes[],
			    void *alloc_ctxs[])
{
	if (*nplanes) {
		if (*nplanes != 1)
			return -EINVAL;

		if (sizes[0] < UVC_PAYLOAD_HEADER_MAX_SIZE)
			return -EINVAL;

		return 0;
	}

	*nplanes = 1;
	sizes[0] = UVC_PAYLOAD_HEADER_MAX_SIZE;

	return 0;
}

static int meta_buffer_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct uvc_buffer *buf = to_uvc_buffer(vbuf);

	if (vb->num_planes != 1)
		return -EINVAL;

	if (vb2_plane_size(vb, 0) < UVC_PAYLOAD_HEADER_MAX_SIZE)
		return -EINVAL;

	buf->state = UVC_BUF_STATE_QUEUED;
	buf->error = 0;
	buf->mem = vb2_plane_vaddr(vb, 0);
	buf->length = vb2_plane_size(vb, 0);
	buf->bytesused = 0;

	return 0;
}

static void meta_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct uvc_video_queue *queue = vb2_get_drv_priv(vb->vb2_queue);
	struct uvc_buffer *buf = to_uvc_buffer(vbuf);
	unsigned long flags;

	spin_lock_irqsave(&queue->irqlock, flags);
	list_add_tail(&buf->queue, &queue->irqqueue);
	spin_unlock_irqrestore(&queue->irqlock, flags);
}

static int meta_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	return 0;
}

static void meta_stop_streaming(struct vb2_queue *vq)
{
	struct uvc_video_queue *queue = vb2_get_drv_priv(vq);
	struct uvc_buffer *buffer;
	unsigned long flags;

	spin_lock_irqsave(&queue->irqlock, flags);

	/* Remove all buffers from the IRQ queue. */
	list_for_each_entry(buffer, &queue->irqqueue, queue)
		vb2_buffer_done(&buffer->buf.vb2_buf, VB2_BUF_STATE_ERROR);
	INIT_LIST_HEAD(&queue->irqqueue);

	spin_unlock_irqrestore(&queue->irqlock, flags);
}

static struct vb2_ops uvc_meta_queue_ops = {
	.queue_setup = meta_queue_setup,
	.buf_prepare = meta_buffer_prepare,
	.buf_queue = meta_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = meta_start_streaming,
	.stop_streaming = meta_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int meta_v4l2_querycap(struct file *file, void *fh,
			      struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct uvc_streaming *stream = video_get_drvdata(vfh->vdev);

	cap->device_caps = V4L2_CAP_META_CAPTURE
			 | V4L2_CAP_STREAMING;
	cap->capabilities = V4L2_CAP_DEVICE_CAPS | cap->device_caps
			  | stream->chain->caps;

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
	fmt->buffersize = UVC_PAYLOAD_HEADER_MAX_SIZE;

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
	struct uvc_meta_dev *meta = &stream->meta;
	struct video_device *vdev = &meta->vdev;
	struct uvc_video_queue *quvc = &meta->queue;
	struct vb2_queue *queue = &quvc->queue;
	int ret;

	vdev->v4l2_dev = &dev->vdev;
	vdev->fops = &uvc_meta_fops;
	vdev->ioctl_ops = &uvc_meta_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->prio = &stream->chain->prio;
	vdev->vfl_dir = VFL_DIR_RX;
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

	mutex_init(&quvc->mutex);
	spin_lock_init(&quvc->irqlock);
	INIT_LIST_HEAD(&quvc->irqqueue);

	vdev->queue = queue;

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		uvc_printk(KERN_ERR, "Failed to register metadata device (%d).\n", ret);

	return ret;
}
