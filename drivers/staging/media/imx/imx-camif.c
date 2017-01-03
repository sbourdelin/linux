/*
 * Video Camera Capture Subdev for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2012-2016 Mentor Graphics Inc.
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
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <video/imx-ipu-v3.h>
#include <media/imx.h>
#include "imx-media.h"

#define DEVICE_NAME "imx-media-camif"

#define CAMIF_NUM_PADS 2

#define CAMIF_DQ_TIMEOUT        5000

struct camif_priv;

struct camif_priv {
	struct device         *dev;
	struct video_device    vfd;
	struct media_pipeline  mp;
	struct imx_media_dev  *md;
	struct v4l2_subdev     sd;
	struct media_pad       pad[CAMIF_NUM_PADS];
	struct media_pad       vd_pad;
	int id;
	int input_pad;
	int output_pad;

	struct v4l2_mbus_framefmt format_mbus[CAMIF_NUM_PADS];
	const struct imx_media_pixfmt *cc[CAMIF_NUM_PADS];

	/* dma buffer ring */
	struct imx_media_dma_buf_ring *in_ring;
	struct v4l2_subdev     *src_sd;

	struct mutex           mutex;       /* capture device mutex */
	spinlock_t             q_lock;      /* protect ready_q */

	/* buffer queue used in videobuf2 */
	struct vb2_queue       buffer_queue;

	/* streaming buffer queue */
	struct list_head       ready_q;

	/* controls inherited from subdevs */
	struct v4l2_ctrl_handler ctrl_hdlr;

	/* misc status */
	int                    current_input; /* the current input */
	v4l2_std_id            current_std;   /* current standard */
	bool                   stop;          /* streaming is stopping */
};

/* In bytes, per queue */
#define VID_MEM_LIMIT	SZ_64M

static struct vb2_ops camif_qops;

/*
 * Video ioctls follow
 */

static int vidioc_querycap(struct file *file, void *fh,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, DEVICE_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, DEVICE_NAME, sizeof(cap->card) - 1);
	cap->bus_info[0] = 0;
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int camif_enum_fmt_vid_cap(struct file *file, void *fh,
				  struct v4l2_fmtdesc *f)
{
	const struct imx_media_pixfmt *cc;
	u32 code;
	int ret;

	ret = imx_media_enum_format(&code, f->index, true, true);
	if (ret)
		return ret;
	cc = imx_media_find_format(0, code, true, true);
	if (!cc)
		return -EINVAL;

	f->pixelformat = cc->fourcc;

	return 0;
}

static int camif_g_fmt_vid_cap(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct camif_priv *priv = video_drvdata(file);
	struct v4l2_mbus_framefmt *outfmt;

	/* user format is the same as the format from output pad */
	outfmt = &priv->format_mbus[priv->output_pad];
	return imx_media_mbus_fmt_to_pix_fmt(&f->fmt.pix, outfmt);
}

static int camif_try_fmt_vid_cap(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	return camif_g_fmt_vid_cap(file, fh, f);
}

static int camif_s_fmt_vid_cap(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct camif_priv *priv = video_drvdata(file);

	if (vb2_is_busy(&priv->buffer_queue)) {
		v4l2_err(&priv->sd, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	return camif_try_fmt_vid_cap(file, priv, f);
}

static int camif_querystd(struct file *file, void *fh, v4l2_std_id *std)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	return v4l2_subdev_call(sensor->sd, video, querystd, std);
}

static int camif_g_std(struct file *file, void *fh, v4l2_std_id *std)
{
	struct camif_priv *priv = video_drvdata(file);

	*std = priv->current_std;
	return 0;
}

static int camif_s_std(struct file *file, void *fh, v4l2_std_id std)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;
	int ret;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	if (vb2_is_busy(&priv->buffer_queue))
		return -EBUSY;

	ret = v4l2_subdev_call(sensor->sd, video, s_std, std);
	if (ret < 0)
		return ret;

	priv->current_std = std;
	return 0;
}

static int camif_enum_input(struct file *file, void *fh,
			    struct v4l2_input *input)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;
	int index = input->index;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	if (index >= sensor->input.num)
		return -EINVAL;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	strncpy(input->name, sensor->input.name[index], sizeof(input->name));

	if (index == priv->current_input) {
		v4l2_subdev_call(sensor->sd, video, g_input_status,
				 &input->status);
		v4l2_subdev_call(sensor->sd, video, querystd, &input->std);
	} else {
		input->status = V4L2_IN_ST_NO_SIGNAL;
		input->std = V4L2_STD_UNKNOWN;
	}

	return 0;
}

static int camif_g_input(struct file *file, void *fh, unsigned int *index)
{
	struct camif_priv *priv = video_drvdata(file);

	*index = priv->current_input;
	return 0;
}

static int camif_s_input(struct file *file, void *fh, unsigned int index)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;
	int ret;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	if (index >= sensor->input.num)
		return -EINVAL;

	if (index == priv->current_input)
		return 0;

	/* select the sensor's input */
	ret = v4l2_subdev_call(sensor->sd, video, s_routing,
			       sensor->input.value[index], 0, 0);
	if (!ret)
		priv->current_input = index;

	return ret;
}

static int camif_g_parm(struct file *file, void *fh,
			struct v4l2_streamparm *a)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return v4l2_subdev_call(sensor->sd, video, g_parm, a);
}

static int camif_s_parm(struct file *file, void *fh,
			struct v4l2_streamparm *a)
{
	struct camif_priv *priv = video_drvdata(file);
	struct imx_media_subdev *sensor;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return v4l2_subdev_call(sensor->sd, video, s_parm, a);
}

static const struct v4l2_ioctl_ops camif_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap        = camif_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap           = camif_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap         = camif_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap           = camif_s_fmt_vid_cap,

	.vidioc_querystd        = camif_querystd,
	.vidioc_g_std           = camif_g_std,
	.vidioc_s_std           = camif_s_std,

	.vidioc_enum_input      = camif_enum_input,
	.vidioc_g_input         = camif_g_input,
	.vidioc_s_input         = camif_s_input,

	.vidioc_g_parm          = camif_g_parm,
	.vidioc_s_parm          = camif_s_parm,

	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_create_bufs     = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf     = vb2_ioctl_prepare_buf,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_expbuf		= vb2_ioctl_expbuf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
};

/*
 * Queue operations
 */

static u32 camif_get_sizeimage(struct camif_priv *priv)
{
	struct v4l2_mbus_framefmt *outfmt;
	const struct imx_media_pixfmt *outcc;

	outfmt = &priv->format_mbus[priv->output_pad];
	outcc = priv->cc[priv->output_pad];
	return (outfmt->width * outfmt->height * outcc->bpp) >> 3;
}

static int camif_queue_setup(struct vb2_queue *vq,
			     unsigned int *nbuffers, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_devs[])
{
	struct camif_priv *priv = vb2_get_drv_priv(vq);
	unsigned int count = *nbuffers;
	u32 sizeimage;

	if (!priv->src_sd)
		return -EPIPE;

	if (vq->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sizeimage = camif_get_sizeimage(priv);

	count = min_t(unsigned int, count, IMX_MEDIA_MAX_RING_BUFS);
	count = max_t(unsigned int, count, IMX_MEDIA_MIN_RING_BUFS);

	while (sizeimage * count > VID_MEM_LIMIT)
		count--;

	if (count < IMX_MEDIA_MIN_RING_BUFS)
		return -EINVAL;

	*nplanes = 1;
	*nbuffers = count;
	sizes[0] = sizeimage;

	return 0;
}

static int camif_buf_init(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct camif_priv *priv = vb2_get_drv_priv(vq);
	struct imx_media_buffer *buf = to_imx_media_vb(vb);

	if (!priv->src_sd)
		return -EPIPE;

	INIT_LIST_HEAD(&buf->list);

	return 0;
}

static int camif_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct camif_priv *priv = vb2_get_drv_priv(vq);
	u32 sizeimage = camif_get_sizeimage(priv);
	int ret;

	if (!priv->src_sd)
		return -EPIPE;

	if (vb2_plane_size(vb, 0) < sizeimage) {
		v4l2_err(&priv->sd,
			 "data will not fit into plane (%lu < %lu)\n",
			 vb2_plane_size(vb, 0), (long)sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, sizeimage);

	if (!priv->in_ring) {
		priv->in_ring = imx_media_alloc_dma_buf_ring(
			priv->md, &priv->src_sd->entity, &priv->sd.entity,
			sizeimage, vq->num_buffers, false);
		if (IS_ERR(priv->in_ring)) {
			v4l2_err(&priv->sd, "failed to alloc dma-buf ring\n");
			ret = PTR_ERR(priv->in_ring);
			priv->in_ring = NULL;
			return ret;
		}
	}

	ret = imx_media_dma_buf_queue_from_vb(priv->in_ring, vb);
	if (ret)
		goto free_ring;

	return 0;

free_ring:
	imx_media_free_dma_buf_ring(priv->in_ring);
	priv->in_ring = NULL;
	return ret;
}

static void camif_buf_queue(struct vb2_buffer *vb)
{
	struct camif_priv *priv = vb2_get_drv_priv(vb->vb2_queue);
	struct imx_media_buffer *buf = to_imx_media_vb(vb);
	unsigned long flags;

	spin_lock_irqsave(&priv->q_lock, flags);

	list_add_tail(&buf->list, &priv->ready_q);

	spin_unlock_irqrestore(&priv->q_lock, flags);
}

static int camif_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct camif_priv *priv = vb2_get_drv_priv(vq);
	u32 sizeimage = camif_get_sizeimage(priv);
	struct imx_media_buffer *buf, *tmp;
	unsigned long flags;
	int ret;

	if (vb2_is_streaming(vq))
		return 0;

	if (!priv->src_sd)
		return -EPIPE;

	if (!priv->in_ring) {
		priv->in_ring = imx_media_alloc_dma_buf_ring(
			priv->md, &priv->src_sd->entity, &priv->sd.entity,
			sizeimage, vq->num_buffers, false);
		if (IS_ERR(priv->in_ring)) {
			v4l2_err(&priv->sd, "failed to alloc dma-buf ring\n");
			ret = PTR_ERR(priv->in_ring);
			priv->in_ring = NULL;
			goto return_bufs;
		}
	}

	ret = imx_media_pipeline_set_stream(priv->md, &priv->sd.entity,
					    &priv->mp, true);
	if (ret) {
		v4l2_err(&priv->sd, "pipeline_set_stream failed with %d\n",
			 ret);
		goto free_ring;
	}

	priv->stop = false;

	return 0;

free_ring:
	imx_media_free_dma_buf_ring(priv->in_ring);
	priv->in_ring = NULL;
return_bufs:
	spin_lock_irqsave(&priv->q_lock, flags);
	list_for_each_entry_safe(buf, tmp, &priv->ready_q, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_QUEUED);
	}
	spin_unlock_irqrestore(&priv->q_lock, flags);
	return ret;
}

static void camif_stop_streaming(struct vb2_queue *vq)
{
	struct camif_priv *priv = vb2_get_drv_priv(vq);
	struct imx_media_buffer *frame;
	unsigned long flags;
	int ret;

	if (!vb2_is_streaming(vq))
		return;

	spin_lock_irqsave(&priv->q_lock, flags);
	priv->stop = true;
	spin_unlock_irqrestore(&priv->q_lock, flags);

	ret = imx_media_pipeline_set_stream(priv->md, &priv->sd.entity,
					    &priv->mp, false);
	if (ret)
		v4l2_warn(&priv->sd, "pipeline_set_stream failed with %d\n",
			  ret);

	if (priv->in_ring) {
		v4l2_warn(&priv->sd, "%s: in_ring was not freed\n",
			  __func__);
		imx_media_free_dma_buf_ring(priv->in_ring);
		priv->in_ring = NULL;
	}

	/* release all active buffers */
	spin_lock_irqsave(&priv->q_lock, flags);
	while (!list_empty(&priv->ready_q)) {
		frame = list_entry(priv->ready_q.next,
				   struct imx_media_buffer, list);
		list_del(&frame->list);
		vb2_buffer_done(&frame->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&priv->q_lock, flags);
}

static struct vb2_ops camif_qops = {
	.queue_setup	 = camif_queue_setup,
	.buf_init        = camif_buf_init,
	.buf_prepare	 = camif_buf_prepare,
	.buf_queue	 = camif_buf_queue,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
	.start_streaming = camif_start_streaming,
	.stop_streaming  = camif_stop_streaming,
};

/*
 * File operations
 */
static int camif_open(struct file *file)
{
	struct camif_priv *priv = video_drvdata(file);
	int ret;

	if (mutex_lock_interruptible(&priv->mutex))
		return -ERESTARTSYS;

	ret = v4l2_fh_open(file);
	if (ret)
		v4l2_err(&priv->sd, "v4l2_fh_open failed\n");

	mutex_unlock(&priv->mutex);
	return ret;
}

static int camif_release(struct file *file)
{
	struct camif_priv *priv = video_drvdata(file);
	struct vb2_queue *vq = &priv->buffer_queue;
	int ret = 0;

	mutex_lock(&priv->mutex);

	if (file->private_data == vq->owner) {
		vb2_queue_release(vq);
		vq->owner = NULL;
	}

	v4l2_fh_release(file);
	mutex_unlock(&priv->mutex);
	return ret;
}

static const struct v4l2_file_operations camif_fops = {
	.owner		= THIS_MODULE,
	.open		= camif_open,
	.release	= camif_release,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= vb2_fop_mmap,
};

static struct video_device camif_videodev = {
	.fops		= &camif_fops,
	.ioctl_ops	= &camif_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_RX,
	.tvnorms	= V4L2_STD_NTSC | V4L2_STD_PAL | V4L2_STD_SECAM,
};

/*
 * Subdev and media entity operations
 */

static void camif_new_dma_buf(struct camif_priv *priv)
{
	struct imx_media_dma_buf *dmabuf;
	struct imx_media_buffer *buf;
	enum vb2_buffer_state state;
	struct vb2_buffer *vb;
	unsigned long flags;

	spin_lock_irqsave(&priv->q_lock, flags);

	if (priv->stop || list_empty(&priv->ready_q))
		goto unlock;

	dmabuf = imx_media_dma_buf_dequeue(priv->in_ring);
	if (!dmabuf)
		goto unlock;

	vb = dmabuf->vb;
	buf = to_imx_media_vb(vb);
	if (list_empty(&buf->list)) {
		dev_dbg(priv->dev, "%s: buf%d not queued\n", __func__,
			vb->index);
		goto unlock;
	}

	dev_dbg(priv->dev, "%s: new buf%d\n", __func__, vb->index);
	vb->timestamp = ktime_get_ns();
	list_del_init(&buf->list);
	state = dmabuf->status == IMX_MEDIA_BUF_STATUS_DONE ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
	vb2_buffer_done(vb, state);
unlock:
	spin_unlock_irqrestore(&priv->q_lock, flags);
}

static long camif_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct camif_priv *priv = v4l2_get_subdevdata(sd);
	struct imx_media_dma_buf_ring **ring;

	switch (cmd) {
	case IMX_MEDIA_REQ_DMA_BUF_SINK_RING:
		if (!priv->in_ring)
			return -EINVAL;
		ring = (struct imx_media_dma_buf_ring **)arg;
		*ring = priv->in_ring;
		break;
	case IMX_MEDIA_NEW_DMA_BUF:
		camif_new_dma_buf(priv);
		break;
	case IMX_MEDIA_REL_DMA_BUF_SINK_RING:
		/* src indicates sink buffer ring can be freed */
		if (!priv->in_ring)
			return 0;
		v4l2_info(sd, "%s: freeing sink ring\n", __func__);
		imx_media_free_dma_buf_ring(priv->in_ring);
		priv->in_ring = NULL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int camif_link_setup(struct media_entity *entity,
			    const struct media_pad *local,
			    const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct camif_priv *priv = v4l2_get_subdevdata(sd);
	struct video_device *vfd = &priv->vfd;
	struct v4l2_subdev *remote_sd;
	int ret = 0;

	dev_dbg(priv->dev, "link setup %s -> %s", remote->entity->name,
		local->entity->name);

	if (is_media_entity_v4l2_video_device(remote->entity))
		return 0;

	WARN_ON(local->flags & MEDIA_PAD_FL_SOURCE);

	remote_sd = media_entity_to_v4l2_subdev(remote->entity);

	/* reset controls to refresh with inherited from subdevs */
	v4l2_ctrl_handler_free(vfd->ctrl_handler);
	v4l2_ctrl_handler_init(vfd->ctrl_handler, 0);

	if (flags & MEDIA_LNK_FL_ENABLED) {
		if (priv->src_sd)
			return -EBUSY;
		priv->src_sd = remote_sd;
		ret = imx_media_inherit_controls(priv->md, vfd,
						 &priv->src_sd->entity);
	} else {
		priv->src_sd = NULL;
	}

	return ret;
}

static int camif_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= CAMIF_NUM_PADS)
		return -EINVAL;

	return imx_media_enum_format(&code->code, code->index, true, true);
}

static int camif_get_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_pad_config *cfg,
			 struct v4l2_subdev_format *sdformat)
{
	struct camif_priv *priv = v4l2_get_subdevdata(sd);

	if (sdformat->pad >= CAMIF_NUM_PADS)
		return -EINVAL;

	sdformat->format = priv->format_mbus[sdformat->pad];

	return 0;
}

static int camif_set_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_pad_config *cfg,
			 struct v4l2_subdev_format *sdformat)
{
	struct camif_priv *priv = v4l2_get_subdevdata(sd);
	const struct imx_media_pixfmt *cc;
	u32 code;

	if (sdformat->pad >= CAMIF_NUM_PADS)
		return -EINVAL;

	if (vb2_is_busy(&priv->buffer_queue)) {
		v4l2_err(&priv->sd, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	cc = imx_media_find_format(0, sdformat->format.code, true, true);
	if (!cc) {
		imx_media_enum_format(&code, 0, true, true);
		cc = imx_media_find_format(0, code, true, true);
		sdformat->format.code = cc->codes[0];
	}

	/* Output pad mirrors input pad, no limitations on input pads */
	if (sdformat->pad == priv->output_pad)
		sdformat->format = priv->format_mbus[priv->input_pad];

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = sdformat->format;
	} else {
		priv->format_mbus[sdformat->pad] = sdformat->format;
		priv->cc[sdformat->pad] = cc;
	}

	return 0;
}

static int camif_init_pads(struct camif_priv *priv)
{
	struct video_device *vfd = &priv->vfd;
	struct imx_media_subdev *imxsd;
	struct imx_media_pad *pad;
	int i, ret;

	imxsd = imx_media_find_subdev_by_sd(priv->md, &priv->sd);
	if (IS_ERR(imxsd))
		return PTR_ERR(imxsd);

	if (imxsd->num_sink_pads != 1 || imxsd->num_src_pads != 1) {
		v4l2_err(&priv->sd, "invalid num pads %d/%d\n",
			 imxsd->num_sink_pads, imxsd->num_src_pads);
		return -EINVAL;
	}

	priv->vd_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vfd->entity, 1, &priv->vd_pad);
	if (ret) {
		v4l2_err(&priv->sd, "failed to init device node pad\n");
		return ret;
	}

	for (i = 0; i < CAMIF_NUM_PADS; i++) {
		pad = &imxsd->pad[i];
		priv->pad[i] = pad->pad;
		if (priv->pad[i].flags & MEDIA_PAD_FL_SINK)
			priv->input_pad = i;
		else
			priv->output_pad = i;

		/* set a default mbus format  */
		ret = imx_media_init_mbus_fmt(&priv->format_mbus[i],
					      640, 480, 0, V4L2_FIELD_NONE,
					      &priv->cc[i]);
		if (ret)
			return ret;
	}

	return media_entity_pads_init(&priv->sd.entity, CAMIF_NUM_PADS,
				      priv->pad);
}

static int camif_registered(struct v4l2_subdev *sd)
{
	struct camif_priv *priv = v4l2_get_subdevdata(sd);
	struct vb2_queue *vq = &priv->buffer_queue;
	struct video_device *vfd = &priv->vfd;
	struct v4l2_mbus_framefmt *infmt, *outfmt;
	int ret;

	/* get media device */
	priv->md = dev_get_drvdata(sd->v4l2_dev->dev);

	vfd->v4l2_dev = sd->v4l2_dev;

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(sd, "Failed to register video device\n");
		return ret;
	}

	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_DMABUF;
	vq->drv_priv = priv;
	vq->buf_struct_size = sizeof(struct imx_media_buffer);
	vq->ops = &camif_qops;
	vq->mem_ops = &vb2_dma_contig_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->lock = &priv->mutex;
	vq->min_buffers_needed = 2;
	vq->dev = priv->dev;

	ret = vb2_queue_init(vq);
	if (ret) {
		v4l2_err(sd, "vb2_queue_init failed\n");
		goto unreg;
	}

	INIT_LIST_HEAD(&priv->ready_q);

	ret = camif_init_pads(priv);
	if (ret) {
		v4l2_err(sd, "camif_init_pads failed\n");
		goto unreg;
	}

	/* create the link to our device node */
	ret = media_create_pad_link(&sd->entity, priv->output_pad,
				    &vfd->entity, 0,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		v4l2_err(sd, "failed to create link to device node\n");
		goto unreg;
	}

	/* setup default pad formats */
	infmt = &priv->format_mbus[priv->input_pad];
	outfmt = &priv->format_mbus[priv->output_pad];
	ret = imx_media_init_mbus_fmt(outfmt, 640, 480, 0, V4L2_FIELD_NONE,
				      &priv->cc[priv->output_pad]);
	if (ret)
		goto unreg;

	*infmt = *outfmt;
	priv->cc[priv->input_pad] = priv->cc[priv->output_pad];

	priv->current_std = V4L2_STD_UNKNOWN;

	v4l2_info(sd, "Registered %s as /dev/%s\n", vfd->name,
		  video_device_node_name(vfd));

	vfd->ctrl_handler = &priv->ctrl_hdlr;

	return 0;
unreg:
	video_unregister_device(vfd);
	return ret;
}

static void camif_unregistered(struct v4l2_subdev *sd)
{
	struct camif_priv *priv = v4l2_get_subdevdata(sd);
	struct video_device *vfd = &priv->vfd;

	mutex_lock(&priv->mutex);

	if (video_is_registered(vfd)) {
		video_unregister_device(vfd);
		media_entity_cleanup(&vfd->entity);
	}

	mutex_unlock(&priv->mutex);
}

static const struct v4l2_subdev_internal_ops camif_internal_ops = {
	.registered = camif_registered,
	.unregistered = camif_unregistered,
};

static struct v4l2_subdev_core_ops camif_core_ops = {
	.ioctl = camif_ioctl,
};

static struct media_entity_operations camif_entity_ops = {
	.link_setup = camif_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_subdev_pad_ops camif_pad_ops = {
	.enum_mbus_code = camif_enum_mbus_code,
	.get_fmt = camif_get_fmt,
	.set_fmt = camif_set_fmt,
};

static struct v4l2_subdev_ops camif_subdev_ops = {
	.pad = &camif_pad_ops,
	.core = &camif_core_ops,
};

static int camif_probe(struct platform_device *pdev)
{
	struct imx_media_internal_sd_platformdata *pdata;
	struct camif_priv *priv;
	struct video_device *vfd;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = &pdev->dev;

	pdata = priv->dev->platform_data;

	mutex_init(&priv->mutex);
	spin_lock_init(&priv->q_lock);

	v4l2_subdev_init(&priv->sd, &camif_subdev_ops);
	v4l2_set_subdevdata(&priv->sd, priv);
	priv->sd.internal_ops = &camif_internal_ops;
	priv->sd.entity.ops = &camif_entity_ops;
	priv->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	priv->sd.dev = &pdev->dev;
	priv->sd.owner = THIS_MODULE;
	/* get our group id and camif id */
	priv->sd.grp_id = pdata->grp_id;
	priv->id = (pdata->grp_id >> IMX_MEDIA_GRP_ID_CAMIF_BIT) - 1;
	strncpy(priv->sd.name, pdata->sd_name, sizeof(priv->sd.name));
	snprintf(camif_videodev.name, sizeof(camif_videodev.name),
		 "%s devnode", pdata->sd_name);

	priv->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;

	vfd = &priv->vfd;
	*vfd = camif_videodev;
	vfd->lock = &priv->mutex;
	vfd->queue = &priv->buffer_queue;

	video_set_drvdata(vfd, priv);

	v4l2_ctrl_handler_init(&priv->ctrl_hdlr, 0);

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret)
		goto free_ctrls;

	return 0;
free_ctrls:
	v4l2_ctrl_handler_free(&priv->ctrl_hdlr);
	return ret;
}

static int camif_remove(struct platform_device *pdev)
{
	struct camif_priv *priv =
		(struct camif_priv *)platform_get_drvdata(pdev);

	v4l2_ctrl_handler_free(&priv->ctrl_hdlr);
	v4l2_async_unregister_subdev(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_device_unregister_subdev(&priv->sd);

	return 0;
}

static const struct platform_device_id camif_ids[] = {
	{ .name = DEVICE_NAME },
	{ },
};
MODULE_DEVICE_TABLE(platform, camif_ids);

static struct platform_driver imx_camif_driver = {
	.probe		= camif_probe,
	.remove		= camif_remove,
	.driver		= {
		.name	= DEVICE_NAME,
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(imx_camif_driver);

MODULE_DESCRIPTION("i.MX camera interface subdev driver");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
