/*
 * Driver for Renesas R-Car VIN
 *
 * Copyright (C) 2016 Renesas Electronics Corp.
 * Copyright (C) 2011-2013 Renesas Solutions Corp.
 * Copyright (C) 2013 Cogent Embedded, Inc., <source@cogentembedded.com>
 * Copyright (C) 2008 Magnus Damm
 *
 * Based on the soc-camera rcar_vin driver
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "rcar-vin.h"

#define RVIN_DEFAULT_FORMAT		V4L2_PIX_FMT_YUYV
#define RVIN_MAX_WIDTH		2048
#define RVIN_MAX_HEIGHT		2048
#define RVIN_TIMEOUT_MS		100

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

/* Moves a buffer from the queue to the HW slots */
static bool rvin_fill_hw_slot(struct rvin_dev *vin, int slot)
{
	struct rvin_buffer *buf;
	struct vb2_v4l2_buffer *vbuf;
	dma_addr_t phys_addr_top;

	if (vin->queue_buf[slot] != NULL)
		return true;

	if (list_empty(&vin->buf_list))
		return false;

	vin_dbg(vin, "Filling HW slot: %d\n", slot);

	/* Keep track of buffer we give to HW */
	buf = list_entry(vin->buf_list.next, struct rvin_buffer, list);
	vbuf = &buf->vb;
	list_del_init(to_buf_list(vbuf));
	vin->queue_buf[slot] = vbuf;

	/* Setup DMA */
	phys_addr_top = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
	rvin_set_slot_addr(vin, slot, phys_addr_top);

	return true;
}

static bool rvin_fill_hw(struct rvin_dev *vin)
{
	int slot, limit;

	limit = vin->continuous ? HW_BUFFER_NUM : 1;

	for (slot = 0; slot < limit; slot++)
		if (!rvin_fill_hw_slot(vin, slot))
			return false;
	return true;
}

static irqreturn_t rvin_irq(int irq, void *data)
{
	struct rvin_dev *vin = data;
	u32 int_status;
	int slot;
	unsigned int handled = 0;
	unsigned long flags;
	unsigned sequence;

	spin_lock_irqsave(&vin->qlock, flags);

	int_status = rvin_get_interrupt_status(vin);
	if (!int_status)
		goto done;

	rvin_ack_interrupt(vin);
	handled = 1;

	/* Nothing to do if capture status is 'STOPPED' */
	if (vin->state == STOPPED) {
		vin_dbg(vin, "IRQ state stopped\n");
		goto done;
	}

	/* Wait for HW to shutdown */
	if (vin->state == STOPPING) {
		if (!rvin_capture_active(vin)) {
			vin_dbg(vin, "IRQ hw stopped and we are stopping\n");
			complete(&vin->capture_stop);
		}
		goto done;
	}

	/* Prepare for capture and update state */
	slot = rvin_get_active_slot(vin);
	sequence = vin->sequence++;

	vin_dbg(vin, "IRQ %02d: %d\tbuf0: %c buf1: %c buf2: %c\tmore: %d\n",
		sequence, slot,
		slot == 0 ? 'x' : vin->queue_buf[0] != NULL ? '1' : '0',
		slot == 1 ? 'x' : vin->queue_buf[1] != NULL ? '1' : '0',
		slot == 2 ? 'x' : vin->queue_buf[2] != NULL ? '1' : '0',
		!list_empty(&vin->buf_list));

	/* HW have written to a slot that is not prepared we are in trouble */
	if (WARN_ON((vin->queue_buf[slot] == NULL)))
		goto done;

	/* Capture frame */
	vin->queue_buf[slot]->field = vin->format.field;
	vin->queue_buf[slot]->sequence = sequence;
	vin->queue_buf[slot]->vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&vin->queue_buf[slot]->vb2_buf, VB2_BUF_STATE_DONE);
	vin->queue_buf[slot] = NULL;

	/* Prepare for next frame */
	if (!rvin_fill_hw(vin)) {

		/*
		 * Can't supply HW with new buffers fast enough. Halt
		 * capture until more buffers are available.
		 */
		vin->state = STALLED;

		/*
		 * The continuous capturing requires an explicit stop
		 * operation when there is no buffer to be set into
		 * the VnMBm registers.
		 */
		if (vin->continuous) {
			rvin_capture_off(vin);
			vin_dbg(vin, "IRQ %02d: hw not ready stop\n", sequence);
		}
	} else {
		/*
		 * The single capturing requires an explicit capture
		 * operation to fetch the next frame.
		 */
		if (!vin->continuous)
			rvin_capture_on(vin);
	}
done:
	spin_unlock_irqrestore(&vin->qlock, flags);

	return IRQ_RETVAL(handled);
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
			    unsigned int *nplanes, unsigned int sizes[],
			    void *alloc_ctxs[])

{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);

	alloc_ctxs[0] = vin->alloc_ctx;
	/* Make sure the image size is large enough. */
	if (*nplanes)
		return sizes[0] < vin->format.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = vin->format.sizeimage;

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
	struct rvin_dev *vin = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&vin->qlock, flags);

	list_add_tail(to_buf_list(vbuf), &vin->buf_list);

	/*
	 * If capture is stalled add buffer to HW and restart
	 * capturing if HW is ready to continue.
	 */
	if (vin->state == STALLED)
		if (rvin_fill_hw(vin))
			rvin_capture_on(vin);

	spin_unlock_irqrestore(&vin->qlock, flags);
}

static int rvin_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);
	struct v4l2_subdev *sd;
	unsigned long flags;

	sd = vin_to_sd(vin);
	v4l2_subdev_call(sd, video, s_stream, 1);

	spin_lock_irqsave(&vin->qlock, flags);

	vin->state = RUNNING;
	vin->sequence = 0;
	init_completion(&vin->capture_stop);

	/* Continuous capture requires more buffers then there is HW slots */
	vin->continuous = count > HW_BUFFER_NUM;

	/* Wait until HW is ready to start capturing */
	while (!rvin_fill_hw(vin))
		msleep(20);

	rvin_capture_start(vin);

	spin_unlock_irqrestore(&vin->qlock, flags);

	return 0;
}

static void rvin_stop_streaming(struct vb2_queue *vq)
{
	struct rvin_dev *vin = vb2_get_drv_priv(vq);
	struct v4l2_subdev *sd;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&vin->qlock, flags);

	/* Wait for streaming to stop */
	while (vin->state != STOPPED) {

		vin->state = STOPPING;

		rvin_capture_stop(vin);

		/* Wait until capturing has been stopped */
		spin_unlock_irqrestore(&vin->qlock, flags);
		if (!wait_for_completion_timeout(&vin->capture_stop,
					msecs_to_jiffies(RVIN_TIMEOUT_MS)))
			vin->state = STOPPED;
		spin_lock_irqsave(&vin->qlock, flags);
	}

	for (i = 0; i < HW_BUFFER_NUM; i++) {
		if (vin->queue_buf[i]) {
			vb2_buffer_done(&vin->queue_buf[i]->vb2_buf,
					VB2_BUF_STATE_ERROR);
			vin->queue_buf[i] = NULL;
		}
	}

	/* Release all active buffers */
	return_all_buffers(vin, VB2_BUF_STATE_ERROR);

	spin_unlock_irqrestore(&vin->qlock, flags);

	sd = vin_to_sd(vin);
	v4l2_subdev_call(sd, video, s_stream, 0);
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

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int __rvin_dma_try_format_sensor(struct rvin_dev *vin,
					u32 which,
					struct v4l2_pix_format *pix,
					struct rvin_sensor *sensor)
{
	struct v4l2_subdev *sd;
	struct v4l2_subdev_pad_config pad_cfg;
	struct v4l2_subdev_format format = {
		.which = which,
	};
	int ret;

	sd = vin_to_sd(vin);

	v4l2_fill_mbus_format(&format.format, pix, vin->sensor.code);

	ret = v4l2_device_call_until_err(sd->v4l2_dev, 0, pad, set_fmt,
					 &pad_cfg, &format);
	if (ret < 0)
		return ret;

	v4l2_fill_pix_format(pix, &format.format);

	sensor->width = pix->width;
	sensor->height = pix->height;

	vin_dbg(vin, "Sensor format: %ux%u\n", sensor->width, sensor->height);

	return 0;
}

static int __rvin_dma_try_format(struct rvin_dev *vin,
				 u32 which,
				 struct v4l2_pix_format *pix,
				 struct rvin_sensor *sensor)
{
	const struct rvin_video_format *info;
	u32 rwidth, rheight;

	/* Requested */
	rwidth = pix->width;
	rheight = pix->height;

	/*
	 * Retrieve format information and select the current format if the
	 * requested format isn't supported.
	 */
	info = rvin_format_from_pixel(pix->pixelformat);
	if (!info) {
		vin_dbg(vin, "Format %x not found, keeping %x\n",
			pix->pixelformat, vin->format.pixelformat);
		pix->pixelformat = vin->format.pixelformat;
		pix->colorspace = vin->format.colorspace;
		pix->field = vin->format.field;
	}

	/* Always recalculate */
	pix->bytesperline = 0;
	pix->sizeimage = 0;

	/* Limit to sensor capabilities */
	__rvin_dma_try_format_sensor(vin, which, pix, sensor);

	/* If sensor can't match format try if VIN can scale */
	if (sensor->width != rwidth || sensor->height != rheight)
		rvin_scale_try(vin, pix, rwidth, rheight);

	/* Limit to VIN capabilities */
	v4l_bound_align_image(&pix->width, 2, RVIN_MAX_WIDTH, 1,
			      &pix->height, 4, RVIN_MAX_HEIGHT, 2, 0);

	switch (pix->field) {
	case V4L2_FIELD_NONE:
	case V4L2_FIELD_TOP:
	case V4L2_FIELD_BOTTOM:
	case V4L2_FIELD_INTERLACED_TB:
	case V4L2_FIELD_INTERLACED_BT:
	case V4L2_FIELD_INTERLACED:
		break;
	default:
		pix->field = V4L2_FIELD_NONE;
		break;
	}

	pix->bytesperline = max_t(u32, pix->bytesperline,
				  rvin_format_bytesperline(pix));
	pix->sizeimage = max_t(u32, pix->sizeimage,
			       rvin_format_sizeimage(pix));

	vin_dbg(vin, "Requested %ux%u Got %ux%u bpl: %d size: %d\n",
		rwidth, rheight, pix->width, pix->height,
		pix->bytesperline, pix->sizeimage);

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
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
		V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int rvin_try_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);
	struct rvin_sensor sensor;

	return __rvin_dma_try_format(vin, V4L2_SUBDEV_FORMAT_TRY, &f->fmt.pix,
				     &sensor);
}

static int rvin_s_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);
	struct rvin_sensor sensor;
	int ret;

	if (vb2_is_busy(&vin->queue))
		return -EBUSY;

	ret = __rvin_dma_try_format(vin, V4L2_SUBDEV_FORMAT_ACTIVE, &f->fmt.pix,
				    &sensor);
	if (ret)
		return ret;

	vin->sensor.width = sensor.width;
	vin->sensor.height = sensor.height;

	vin->format = f->fmt.pix;

	return 0;
}

static int rvin_g_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct rvin_dev *vin = video_drvdata(file);

	f->fmt.pix = vin->format;

	return 0;
}

static int rvin_enum_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_fmtdesc *f)
{
	const struct rvin_video_format *fmt = rvin_format_from_num(f->index);

	if (!fmt)
		return -EINVAL;

	f->pixelformat = fmt->fourcc;

	return 0;
}

static int rvin_g_selection(struct file *file, void *fh,
			    struct v4l2_selection *s)
{
	struct rvin_dev *vin = video_drvdata(file);

	switch (s->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		s->r.left = s->r.top = 0;
		s->r.width = vin->sensor.width;
		s->r.height = vin->sensor.height;
		break;
	case V4L2_SEL_TGT_CROP:
		s->r = vin->crop;
		break;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r.left = s->r.top = 0;
		s->r.width = vin->format.width;
		s->r.height = vin->format.height;
		break;
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		s->r = vin->compose;
		/*
		 * FIXME: Is this correct? v4l2-compliance test fails
		 * if top and left are not 0.
		 */
		s->r.top = 0;
		s->r.left = 0;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		s->r = vin->compose;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void rect_set_min_size(struct v4l2_rect *r,
			      const struct v4l2_rect *min_size)
{
	if (r->width < min_size->width)
		r->width = min_size->width;
	if (r->height < min_size->height)
		r->height = min_size->height;
}

static void rect_set_max_size(struct v4l2_rect *r,
			      const struct v4l2_rect *max_size)
{
	if (r->width > max_size->width)
		r->width = max_size->width;
	if (r->height > max_size->height)
		r->height = max_size->height;
}

static void rect_map_inside(struct v4l2_rect *r,
			    const struct v4l2_rect *boundary)
{
	rect_set_max_size(r, boundary);

	if (r->left < boundary->left)
		r->left = boundary->left;
	if (r->top < boundary->top)
		r->top = boundary->top;
	if (r->left + r->width > boundary->width)
		r->left = boundary->width - r->width;
	if (r->top + r->height > boundary->height)
		r->top = boundary->height - r->height;
}

static int rvin_s_selection(struct file *file, void *fh,
			    struct v4l2_selection *s)
{
	struct rvin_dev *vin = video_drvdata(file);
	const struct rvin_video_format *fmt;
	struct v4l2_rect r = s->r;
	struct v4l2_rect max_rect;
	struct v4l2_rect min_rect = {
		.width = 6,
		.height = 2,
	};

	rect_set_min_size(&r, &min_rect);

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		/* Can't crop outside of sensor input */
		max_rect.top = max_rect.left = 0;
		max_rect.width = vin->sensor.width;
		max_rect.height = vin->sensor.height;
		rect_map_inside(&r, &max_rect);

		v4l_bound_align_image(&r.width, 2, vin->sensor.width, 1,
				      &r.height, 4, vin->sensor.height, 2, 0);

		r.top  = clamp_t(s32, r.top, 0, vin->sensor.height - r.height);
		r.left = clamp_t(s32, r.left, 0, vin->sensor.width - r.width);

		vin->crop = s->r = r;

		vin_dbg(vin, "Cropped %dx%d@%d:%d of %dx%d\n",
			 r.width, r.height, r.left, r.top,
			 vin->sensor.width, vin->sensor.height);
		break;
	case V4L2_SEL_TGT_COMPOSE:
		/* Make sure compose rect fits inside output format */
		max_rect.top = max_rect.left = 0;
		max_rect.width = vin->format.width;
		max_rect.height = vin->format.height;
		rect_map_inside(&r, &max_rect);

		/*
		 * Composing is done by adding a offset to the buffer address,
		 * the HW wants this address to be aligned to HW_BUFFER_MASK.
		 * Make sure the top and left values meets this requirement.
		 */
		while ((r.top * vin->format.bytesperline) & HW_BUFFER_MASK)
			r.top--;

		fmt = rvin_format_from_pixel(vin->format.pixelformat);
		while ((r.left * fmt->bpp) & HW_BUFFER_MASK)
			r.left--;

		vin->compose = s->r = r;

		vin_dbg(vin, "Compose %dx%d@%d:%d in %dx%d\n",
			 r.width, r.height, r.left, r.top,
			 vin->format.width, vin->format.height);
		break;
	default:
		return -EINVAL;
	}

	/* HW supports modifying configuration while running */
	rvin_crop_scale_comp(vin);

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

static int rvin_querystd(struct file *file, void *priv, v4l2_std_id *a)
{
	struct rvin_dev *vin = video_drvdata(file);
	struct v4l2_subdev *sd = vin_to_sd(vin);

	return v4l2_subdev_call(sd, video, querystd, a);
}

static int rvin_s_std(struct file *file, void *priv, v4l2_std_id a)
{
	struct rvin_dev *vin = video_drvdata(file);
	struct v4l2_subdev *sd = vin_to_sd(vin);

	return v4l2_subdev_call(sd, video, s_std, a);
}

static int rvin_g_std(struct file *file, void *priv, v4l2_std_id *a)
{
	struct rvin_dev *vin = video_drvdata(file);
	struct v4l2_subdev *sd = vin_to_sd(vin);

	return v4l2_subdev_call(sd, video, g_std, a);
}

static const struct v4l2_ioctl_ops rvin_ioctl_ops = {
	.vidioc_querycap		= rvin_querycap,
	.vidioc_try_fmt_vid_cap		= rvin_try_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= rvin_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= rvin_s_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap	= rvin_enum_fmt_vid_cap,

	.vidioc_g_selection		= rvin_g_selection,
	.vidioc_s_selection		= rvin_s_selection,

	.vidioc_enum_input		= rvin_enum_input,
	.vidioc_g_input			= rvin_g_input,
	.vidioc_s_input			= rvin_s_input,

	.vidioc_querystd		= rvin_querystd,
	.vidioc_g_std			= rvin_g_std,
	.vidioc_s_std			= rvin_s_std,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

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

	for (i = 0; i < HW_BUFFER_NUM; i++)
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
	rvin_capture_stop(vin);
	rvin_disable_interrupts(vin);

	vin->state = STOPPED;

	spin_lock_irqsave(&vin->qlock, flags);
	for (i = 0; i < HW_BUFFER_NUM; i++) {
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
			.pixelformat	= vin->format.pixelformat,
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
	.read		= vb2_fop_read,
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

	vin->state = STOPPED;

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
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	q->lock = &vin->lock;
	q->drv_priv = vin;
	q->buf_struct_size = sizeof(struct rvin_buffer);
	q->ops = &rvin_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_buffers_needed = 2;

	ret = vb2_queue_init(q);
	if (ret < 0) {
		vin_err(vin, "failed to initialize VB2 queue\n");
		goto error;
	}

	/* irq */
	ret = devm_request_irq(vin->dev, irq, rvin_irq, IRQF_SHARED,
			       KBUILD_MODNAME, vin);
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

	/* Set default format */
	vin->format.pixelformat = RVIN_DEFAULT_FORMAT;

	sd->grp_id = 0;
	v4l2_set_subdev_hostdata(sd, vin);

	ret = v4l2_subdev_call(sd, video, g_tvnorms, &vin->vdev.tvnorms);
	if (ret < 0 && ret != -ENOIOCTLCMD && ret != -ENODEV)
		return ret;

	if (vin->vdev.tvnorms == 0) {
		/* Disable the STD API if there are no tvnorms defined */
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_G_STD);
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_S_STD);
		v4l2_disable_ioctl(&vin->vdev, VIDIOC_QUERYSTD);
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

	vin->format.field = V4L2_FIELD_ANY;

	/* Try to improve our guess of a reasonable window format */
	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &fmt);
	if (ret) {
		vin_err(vin, "Failed to get initial format\n");
		goto remove_device;
	}

	vin->format.width	= mf->width;
	vin->format.height	= mf->height;
	vin->format.colorspace	= mf->colorspace;
	vin->format.field	= mf->field;

	/* Set initial crop and compose */
	vin->crop.top = vin->crop.left = 0;
	vin->crop.width = mf->width;
	vin->crop.height = mf->height;

	vin->compose.top = vin->compose.left = 0;
	vin->compose.width = mf->width;
	vin->compose.height = mf->height;

	ret = video_register_device(&vin->vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		vin_err(vin, "Failed to register video device\n");
		goto remove_device;
	}

	video_set_drvdata(&vin->vdev, vin);

	v4l2_info(&vin->v4l2_dev, "Device registered as /dev/video%d\n",
		  vin->vdev.num);

remove_device:
	rvin_remove_device(vin);

	return ret;
}
