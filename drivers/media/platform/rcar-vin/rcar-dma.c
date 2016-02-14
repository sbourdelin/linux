/*
 * Driver for Renesas R-Car VIN IP
 *
 * Copyright (C) 2016 Renesas Solutions Corp.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "rcar-vin.h"

#define VIN_MAX_WIDTH	2048
#define VIN_MAX_HEIGHT	2048
#define TIMEOUT_MS	100

struct rvin_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

#define to_buf_list(vb2_buffer) (&container_of(vb2_buffer, \
			struct rvin_buffer, \
			vb)->list)

/* -----------------------------------------------------------------------------
 * DMA Functions
 */

static int rvin_get_free_hw_slot(struct rvin_dev *vin)
{
	int slot;

	for (slot = 0; slot < vin->nr_hw_slots; slot++)
		if (vin->queue_buf[slot] == NULL)
			return slot;

	return -1;
}

static int rvin_hw_ready(struct rvin_dev *vin)
{
	/* Ensure all HW slots are filled */
	return rvin_get_free_hw_slot(vin) < 0 ? 1 : 0;
}

/* Moves a buffer from the queue to the HW slots */
static int rvin_fill_hw_slot(struct rvin_dev *vin)
{
	struct rvin_buffer *buf;
	struct vb2_v4l2_buffer *vbuf;
	dma_addr_t phys_addr_top;
	int slot;

	if (list_empty(&vin->buf_list))
		return 0;

	/* Search for free HW slot */
	slot = rvin_get_free_hw_slot(vin);
	if (slot < 0)
		return 0;

	/* Keep track of buffer we give to HW */
	buf = list_entry(vin->buf_list.next, struct rvin_buffer, list);
	vbuf = &buf->vb;
	list_del_init(to_buf_list(vbuf));
	vin->queue_buf[slot] = vbuf;

	/* Setup DMA */
	phys_addr_top = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
	rvin_set_slot_addr(vin, slot, phys_addr_top);

	return 1;
}

/* Need to hold qlock before calling */
static void return_all_buffers(struct rvin_dev *vin,
		enum vb2_buffer_state state)
{
	struct rvin_buffer *buf, *node;

	list_for_each_entry_safe(buf, node, &vin->buf_list, list) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->list);
	}
}

static int rvin_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
		unsigned int *nplanes, unsigned int sizes[], void *alloc_ctxs[])

{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);

	alloc_ctxs[0] = vin->alloc_ctx;

	if (!*nbuffers)
		*nbuffers = 2;
	vin->vb_count = *nbuffers;

	/* Number of hardware slots */
	if (is_continuous_transfer(vin))
		vin->nr_hw_slots = MAX_BUFFER_NUM;
	else
		vin->nr_hw_slots = 1;

	if (*nplanes)
		return sizes[0] < vin->format.sizeimage ? -EINVAL : 0;

	sizes[0] = vin->format.sizeimage;
	*nplanes = 1;

	vin_dbg(vin, "nbuffers=%d, size=%u\n", *nbuffers, sizes[0]);

	return 0;
};

static int rvin_buffer_prepare(struct vb2_buffer *vb)
{
	struct rvin_dev *vin = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = vin->format.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		vin_err(vin, "buffer too small (%lu < %lu)\n",
				vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void rvin_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	unsigned long flags;
	struct rvin_dev *vin = vb2_get_drv_priv(vb->vb2_queue);

	spin_lock_irqsave(&vin->qlock, flags);

	list_add_tail(to_buf_list(vbuf), &vin->buf_list);
	rvin_fill_hw_slot(vin);

	spin_unlock_irqrestore(&vin->qlock, flags);
}

static int rvin_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&vin->qlock, flags);

	ret = rvin_setup(vin);
	if (!ret) {
		vin->request_to_stop = false;
		init_completion(&vin->capture_stop);
		vin->state = RUNNING;
		rvin_capture(vin);
	}

	if (ret) {
		/*
		 * In case of an error, return all active buffers to the
		 * QUEUED state
		 */
		return_all_buffers(vin, VB2_BUF_STATE_QUEUED);
	}

	spin_unlock_irqrestore(&vin->qlock, flags);

	return ret;
}

static void rvin_stop_streaming(struct vb2_queue *vq)
{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&vin->qlock, flags);

	rvin_request_capture_stop(vin);

	/* Wait for streaming to stop */
	while (vin->state != STOPPED) {
		/* issue stop if running */
		if (vin->state == RUNNING)
			rvin_request_capture_stop(vin);

		/* Wait until capturing has been stopped */
		if (vin->state == STOPPING) {
			vin->request_to_stop = true;
			spin_unlock_irqrestore(&vin->qlock, flags);
			if (!wait_for_completion_timeout(&vin->capture_stop,
						msecs_to_jiffies(TIMEOUT_MS)))
				vin->state = STOPPED;
			spin_lock_irqsave(&vin->qlock, flags);
		}
	}

	for (i = 0; i < MAX_BUFFER_NUM; i++) {
		if (vin->queue_buf[i]) {
			vb2_buffer_done(&vin->queue_buf[i]->vb2_buf,
					VB2_BUF_STATE_ERROR);
			vin->queue_buf[i] = NULL;
		}
	}

	/* Release all active buffers */
	return_all_buffers(vin, VB2_BUF_STATE_ERROR);

	spin_unlock_irqrestore(&vin->qlock, flags);
}

static struct vb2_ops rvin_qops = {
	.queue_setup		= rvin_queue_setup,
	.buf_prepare		= rvin_buffer_prepare,
	.buf_queue		= rvin_buffer_queue,
	.start_streaming	= rvin_start_streaming,
	.stop_streaming		= rvin_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static irqreturn_t rvin_irq(int irq, void *data)
{
	struct rvin_dev *vin = data;
	u32 int_status;
	bool hw_stopped;
	int slot;
	unsigned int handled = 0;
	unsigned long flags;

	spin_lock_irqsave(&vin->qlock, flags);

	int_status = rvin_get_interrupt_status(vin);
	if (!int_status)
		goto done;
	rvin_ack_interrupt(vin);
	handled = 1;

	/* Nothing to do if capture status is 'STOPPED' */
	if (vin->state == STOPPED)
		goto done;

	hw_stopped = !rvin_capture_active(vin);

	if (hw_stopped) {
		vin->state = STOPPED;
		vin->request_to_stop = false;
		complete(&vin->capture_stop);
		goto done;
	}

	if (!rvin_hw_ready(vin))
		goto done;

	slot = rvin_get_active_slot(vin);

	vin->queue_buf[slot]->field = vin->format.field;
	vin->queue_buf[slot]->sequence = vin->sequence++;
	vin->queue_buf[slot]->vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&vin->queue_buf[slot]->vb2_buf, VB2_BUF_STATE_DONE);
	vin->queue_buf[slot] = NULL;

	rvin_fill_hw_slot(vin);

done:
	spin_unlock_irqrestore(&vin->qlock, flags);

	return IRQ_RETVAL(handled);
}

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int __rvin_dma_try_format_sensor(struct rvin_dev *vin,
		u32 which,
		struct v4l2_pix_format *pix,
		const struct rvin_video_format *info,
		struct rvin_sensor *sensor)
{
	struct v4l2_subdev *sd;
	struct v4l2_subdev_pad_config pad_cfg;
	struct v4l2_subdev_format format = {
		.which = which,
	};
	u32 rwidth, rheight, swidth, sheight;
	int ret;

	sd = vin_to_sd(vin);

	/* Requested */
	rwidth = pix->width;
	rheight = pix->height;

	v4l2_fill_mbus_format(&format.format, pix, info->code);
	ret = v4l2_device_call_until_err(sd->v4l2_dev, 0, pad, set_fmt,
			&pad_cfg, &format);
	if (ret < 0)
		return ret;
	v4l2_fill_pix_format(pix, &format.format);

	/* Sensor */
	swidth = pix->width;
	sheight = pix->height;

	vin_dbg(vin, "sensor format: %ux%u requested format: %ux%u\n", swidth,
			sheight, rwidth, rheight);

	if (swidth != rwidth || sheight != rheight) {
		vin_dbg(vin, "sensor format mismatch, see if we can scale\n");
		ret = rvin_scale_try(vin, pix, rwidth, rheight);
		if (ret)
			return ret;
	}

	/* Store sensor output format */
	if (sensor) {
		sensor->width = swidth;
		sensor->height = sheight;
	}

	return 0;
}

static int __rvin_dma_try_format(struct rvin_dev *vin,
		u32 which,
		struct v4l2_pix_format *pix,
		const struct rvin_video_format **fmtinfo,
		struct rvin_sensor *sensor)
{
	const struct rvin_video_format *info;
	struct v4l2_subdev *sd;
	v4l2_std_id std;
	int ret;

	sd = vin_to_sd(vin);

	/* Retrieve format information and select the current format if the
	 * requested format isn't supported.
	 */
	info = rvin_get_format_by_fourcc(vin, pix->pixelformat);
	if (!info) {
		info = vin->fmtinfo;
		vin_dbg(vin, "Format %x not found, keeping %x\n",
				pix->pixelformat, vin->fmtinfo->fourcc);
		pix->pixelformat = vin->format.pixelformat;
		pix->colorspace = vin->format.colorspace;
		pix->bytesperline = vin->format.bytesperline;
		pix->sizeimage = vin->format.sizeimage;
		pix->field = vin->format.field;
	}

	/* FIXME: calculate using depth and bus width */
	v4l_bound_align_image(&pix->width, 2, VIN_MAX_WIDTH, 1,
			&pix->height, 4, VIN_MAX_HEIGHT, 2, 0);

	/* Limit to sensor capabilities */
	ret = __rvin_dma_try_format_sensor(vin, which, pix, info, sensor);
	if (ret)
		return ret;

	switch (pix->field) {
	default:
		pix->field = V4L2_FIELD_NONE;
		/* fall-through */
	case V4L2_FIELD_NONE:
	case V4L2_FIELD_TOP:
	case V4L2_FIELD_BOTTOM:
	case V4L2_FIELD_INTERLACED_TB:
	case V4L2_FIELD_INTERLACED_BT:
		break;
	case V4L2_FIELD_INTERLACED:
		/* Query for standard if not explicitly mentioned _TB/_BT */
		ret = v4l2_subdev_call(sd, video, querystd, &std);
		if (ret < 0) {
			if (ret != -ENOIOCTLCMD)
				return ret;
			pix->field = V4L2_FIELD_NONE;
		} else {
			pix->field = std & V4L2_STD_625_50 ?
				V4L2_FIELD_INTERLACED_TB :
				V4L2_FIELD_INTERLACED_BT;
		}
		break;
	}

	ret = rvin_bytes_per_line(info, pix->width);
	if (ret < 0)
		return ret;
	pix->bytesperline = max_t(u32, pix->bytesperline, ret);

	ret = rvin_image_size(info, pix->bytesperline, pix->height);
	if (ret < 0)
		return ret;
	pix->sizeimage = max_t(u32, pix->sizeimage, ret);

	if (fmtinfo)
		*fmtinfo = info;

	return 0;
}

static int rvin_querycap(struct file *file, void *priv,
		struct v4l2_capability *cap)
{
	struct rvin_dev *vin = video_drvdata(file);

	strlcpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strlcpy(cap->card, "R_Car_VIN", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
			dev_name(vin->dev));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int rvin_try_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);

	/* Only single-plane capture is supported so far */
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return __rvin_dma_try_format(vin, V4L2_SUBDEV_FORMAT_TRY, &f->fmt.pix,
			NULL, NULL);
}

static int rvin_s_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);
	const struct rvin_video_format *info;
	struct rvin_sensor sensor;
	int ret;

	/* Only single-plane capture is supported so far */
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_is_busy(&vin->queue))
		return -EBUSY;

	ret = __rvin_dma_try_format(vin, V4L2_SUBDEV_FORMAT_ACTIVE, &f->fmt.pix,
			&info, &sensor);
	if (ret)
		return ret;

	vin->format = f->fmt.pix;
	vin->fmtinfo = info;
	vin->sensor.width = sensor.width;
	vin->sensor.height = sensor.height;

	vin_dbg(vin, "set width: %d height: %d\n", vin->format.width,
			vin->format.height);

	return 0;
}

static int rvin_g_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	f->fmt.pix = vin->format;

	return 0;
}

static int rvin_enum_fmt_vid_cap(struct file *file, void *priv,
		struct v4l2_fmtdesc *f)
{
	struct rvin_dev *vin = video_drvdata(file);

	if (f->index >= vin->sensor.num_formats)
		return -EINVAL;

	f->pixelformat = vin->sensor.formats[f->index].fourcc;
	strlcpy(f->description, vin->sensor.formats[f->index].name,
			sizeof(f->description));
	return 0;
}

static int rvin_enum_input(struct file *file, void *priv,
		struct v4l2_input *i)
{
	struct rvin_dev *vin = video_drvdata(file);

	if (i->index != 0)
		return -EINVAL;

	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->std = vin->vdev.tvnorms;
	strlcpy(i->name, "Camera", sizeof(i->name));

	return 0;
}

static int rvin_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int rvin_s_input(struct file *file, void *priv, unsigned int i)
{
	if (i > 0)
		return -EINVAL;
	return 0;
}

static int rvin_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	int ret;
	struct rvin_dev *vin = video_drvdata(file);
	struct v4l2_subdev *sd = vin_to_sd(vin);

	WARN_ON(priv != file->private_data);

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	ret = vb2_streamon(&vin->queue, i);
	if (!ret)
		v4l2_subdev_call(sd, video, s_stream, 1);

	return ret;
}

static int rvin_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	int ret;
	struct rvin_dev *vin = video_drvdata(file);
	struct v4l2_subdev *sd = vin_to_sd(vin);

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	/*
	 * This calls buf_release from host driver's videobuf_queue_ops for all
	 * remaining buffers. When the last buffer is freed, stop capture
	 */
	ret = vb2_streamoff(&vin->queue, i);

	v4l2_subdev_call(sd, video, s_stream, 0);

	return ret;
}

static const struct v4l2_ioctl_ops rvin_ioctl_ops = {
	.vidioc_querycap		= rvin_querycap,
	.vidioc_try_fmt_vid_cap		= rvin_try_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= rvin_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= rvin_s_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap	= rvin_enum_fmt_vid_cap,

	/* TODO:
	 * .vidioc_g_selection		= rvin_g_selection,
	 * .vidioc_s_selection		= rvin_s_selection,
	 */

	.vidioc_enum_input		= rvin_enum_input,
	.vidioc_g_input			= rvin_g_input,
	.vidioc_s_input			= rvin_s_input,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,

	.vidioc_streamon		= rvin_streamon,
	.vidioc_streamoff		= rvin_streamoff,

	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/* -----------------------------------------------------------------------------
 * File Operations
 */

static int __rvin_power_on(struct rvin_dev *vin)
{
	int ret;
	struct v4l2_subdev *sd = vin_to_sd(vin);

	ret = v4l2_subdev_call(sd, core, s_power, 1);
	if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
		return ret;
	return 0;
}

static int __rvin_power_off(struct rvin_dev *vin)
{
	int ret;
	struct v4l2_subdev *sd = vin_to_sd(vin);

	ret = v4l2_subdev_call(sd, core, s_power, 0);
	if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
		return ret;
	return 0;
}
static int rvin_add_device(struct rvin_dev *vin)
{
	int i;

	for (i = 0; i < MAX_BUFFER_NUM; i++)
		vin->queue_buf[i] = NULL;

	pm_runtime_get_sync(vin->v4l2_dev.dev);

	return 0;
}

static int rvin_remove_device(struct rvin_dev *vin)
{
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;
	int i;

	/* disable capture, disable interrupts */
	rvin_disable_capture(vin);
	rvin_disable_interrupts(vin);

	vin->state = STOPPED;
	vin->request_to_stop = false;

	spin_lock_irqsave(&vin->qlock, flags);
	for (i = 0; i < MAX_BUFFER_NUM; i++) {
		vbuf = vin->queue_buf[i];
		if (vbuf) {
			list_del_init(to_buf_list(vbuf));
			vb2_buffer_done(&vbuf->vb2_buf, VB2_BUF_STATE_ERROR);
		}
	}
	spin_unlock_irqrestore(&vin->qlock, flags);

	pm_runtime_put(vin->v4l2_dev.dev);

	return 0;
}

static int rvin_initialize_device(struct file *file)
{
	struct rvin_dev *vin = video_drvdata(file);
	int ret;

	struct v4l2_format f = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width		= vin->format.width,
			.height		= vin->format.height,
			.field		= vin->format.field,
			.colorspace	= vin->format.colorspace,
			.pixelformat	= vin->fmtinfo->fourcc,
		},
	};

	rvin_add_device(vin);

	/* Power on subdevice */
	ret = __rvin_power_on(vin);
	if (ret < 0)
		goto epower;

	pm_runtime_enable(&vin->vdev.dev);
	ret = pm_runtime_resume(&vin->vdev.dev);
	if (ret < 0 && ret != -ENOSYS)
		goto eresume;

	/*
	 * Try to configure with default parameters. Notice: this is the
	 * very first open, so, we cannot race against other calls,
	 * apart from someone else calling open() simultaneously, but
	 * .host_lock is protecting us against it.
	 */
	ret = rvin_s_fmt_vid_cap(file, NULL, &f);
	if (ret < 0)
		goto esfmt;

	v4l2_ctrl_handler_setup(&vin->ctrl_handler);


	return 0;
esfmt:
	pm_runtime_disable(&vin->vdev.dev);
eresume:
	__rvin_power_off(vin);
epower:
	rvin_remove_device(vin);

	return ret;
}

static int rvin_open(struct file *file)
{
	struct rvin_dev *vin = video_drvdata(file);
	int ret;

	mutex_lock(&vin->lock);

	file->private_data = vin;

	ret = v4l2_fh_open(file);
	if (ret)
		goto unlock;

	if (!v4l2_fh_is_singular_file(file))
		goto unlock;

	if (rvin_initialize_device(file)) {
		v4l2_fh_release(file);
		ret = -ENODEV;
	}

unlock:
	mutex_unlock(&vin->lock);
	return ret;
}

static int rvin_release(struct file *file)
{
	struct rvin_dev *vin = video_drvdata(file);
	bool fh_singular;
	int ret;

	mutex_lock(&vin->lock);

	/* Save the singular status before we call the clean-up helper */
	fh_singular = v4l2_fh_is_singular_file(file);

	/* the release helper will cleanup any on-going streaming */
	ret = _vb2_fop_release(file, NULL);

	/*
	 * If this was the last open file.
	 * Then de-initialize hw module.
	 */
	if (fh_singular) {
		pm_runtime_suspend(&vin->vdev.dev);
		pm_runtime_disable(&vin->vdev.dev);

		__rvin_power_off(vin);

		rvin_remove_device(vin);
	}

	mutex_unlock(&vin->lock);

	return ret;
}

static const struct v4l2_file_operations rvin_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= rvin_open,
	.release	= rvin_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

/* -----------------------------------------------------------------------------
 * DMA Core
 */

void rvin_dma_cleanup(struct rvin_dev *vin)
{
	if (video_is_registered(&vin->vdev)) {
		v4l2_info(&vin->v4l2_dev, "Removing /dev/video%d\n",
				vin->vdev.num);
		video_unregister_device(&vin->vdev);
	}

	if (!IS_ERR_OR_NULL(vin->alloc_ctx))
		vb2_dma_contig_cleanup_ctx(vin->alloc_ctx);

	/* Checks internaly if handlers have been init or not */
	v4l2_ctrl_handler_free(&vin->ctrl_handler);

	mutex_destroy(&vin->lock);
}

int rvin_dma_init(struct rvin_dev *vin, int irq)
{
	struct video_device *vdev = &vin->vdev;
	struct vb2_queue *q = &vin->queue;
	int ret;

	mutex_init(&vin->lock);
	INIT_LIST_HEAD(&vin->buf_list);
	spin_lock_init(&vin->qlock);

	/* TODO: add more default vin->format and vin->fmts */
	vin->state = STOPPED;
	vin->format.width = VIN_MAX_WIDTH;
	vin->format.height = VIN_MAX_HEIGHT;

	/* Add the controls */
	/*
	 * Currently the subdev with the largest number of controls (13) is
	 * ov6550. So let's pick 16 as a hint for the control handler. Note
	 * that this is a hint only: too large and you waste some memory, too
	 * small and there is a (very) small performance hit when looking up
	 * controls in the internal hash.
	 */
	ret = v4l2_ctrl_handler_init(&vin->ctrl_handler, 16);
	if (ret < 0)
		goto error;

	/* video node */
	vdev->fops = &rvin_fops;
	vdev->v4l2_dev = &vin->v4l2_dev;
	vdev->queue = &vin->queue;
	strlcpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &rvin_ioctl_ops;
	vdev->lock = &vin->lock;
	vdev->ctrl_handler = &vin->ctrl_handler;

	/* buffer queue */
	vin->alloc_ctx = vb2_dma_contig_init_ctx(vin->dev);
	if (IS_ERR(vin->alloc_ctx)) {
		ret = PTR_ERR(vin->alloc_ctx);
		goto error;
	}

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR;
	q->lock = &vin->lock;
	q->drv_priv = vin;
	q->buf_struct_size = sizeof(struct rvin_buffer);
	q->ops = &rvin_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	ret = vb2_queue_init(q);
	if (ret < 0) {
		vin_err(vin, "failed to initialize VB2 queue\n");
		goto error;
	}

	/* irq */
	ret = devm_request_irq(vin->dev, irq,
			rvin_irq, IRQF_SHARED, KBUILD_MODNAME, vin);
	if (ret) {
		vin_err(vin, "failed to request irq\n");
		goto error;
	}

	return 0;
error:
	rvin_dma_cleanup(vin);
	return ret;
}

int rvin_dma_on(struct rvin_dev *vin)
{
	struct v4l2_subdev *sd;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_mbus_framefmt *mf = &fmt.format;
	int ret;

	sd = vin_to_sd(vin);

	/* Pick first format as default format */
	vin->fmtinfo = &vin->sensor.formats[0];

	sd->grp_id = 0;
	v4l2_set_subdev_hostdata(sd, vin);

	ret = v4l2_subdev_call(sd, video, g_tvnorms, &vin->vdev.tvnorms);
	if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
		return ret;

	if (vin->vdev.tvnorms == 0) {
		/* Disable the STD API if there are no tvnorms defined */
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_G_STD);
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_S_STD);
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_ENUMSTD);
	}

	ret = v4l2_ctrl_add_handler(&vin->ctrl_handler, sd->ctrl_handler, NULL);
	if (ret < 0)
		return ret;

	ret = rvin_add_device(vin);
	if (ret < 0) {
		vin_err(vin, "Couldn't activate the camera: %d\n", ret);
		return ret;
	}

	/* TODO: ret = rvin_sensor_setup(vin, pix, ...); */

	vin->format.field = V4L2_FIELD_ANY;

	video_set_drvdata(&vin->vdev, vin);

	ret = video_register_device(&vin->vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		vin_err(vin, "Failed to register video device\n");
		goto remove_device;
	}

	v4l2_info(&vin->v4l2_dev, "Device registered as /dev/video%d\n",
			vin->vdev.num);

	/* Try to improve our guess of a reasonable window format */
	if (!v4l2_subdev_call(sd, pad, get_fmt, NULL, &fmt)) {
		vin->format.width	= mf->width;
		vin->format.height	= mf->height;
		vin->format.colorspace	= mf->colorspace;
		vin->format.field	= mf->field;
	}

remove_device:
	rvin_remove_device(vin);

	return ret;
}
