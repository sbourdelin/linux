/*
 * V4L2 Capture CSI Subdev for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2014-2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-of.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>
#include <media/imx.h>
#include "imx-media.h"

/*
 * Min/Max supported width and heights.
 *
 * We allow planar output, so we have to align width by 16 pixels
 * to meet IDMAC alignment requirements.
 *
 * TODO: move this into pad format negotiation, if capture device
 * has not requested planar formats, we should allow 8 pixel
 * alignment.
 */
#define MIN_W       176
#define MIN_H       144
#define MAX_W      4096
#define MAX_H      4096
#define W_ALIGN    4 /* multiple of 16 pixels */
#define H_ALIGN    1 /* multiple of 2 lines */
#define S_ALIGN    1 /* multiple of 2 */

struct csi_priv {
	struct device *dev;
	struct ipu_soc *ipu;
	struct imx_media_dev *md;
	struct v4l2_subdev sd;
	struct media_pad pad[CSI_NUM_PADS];
	/* the video device at IDMAC output pad */
	struct imx_media_video_dev *vdev;
	struct imx_media_fim *fim;
	int csi_id;
	int smfc_id;

	/* lock to protect all members below */
	struct mutex lock;

	int active_output_pad;

	struct ipuv3_channel *idmac_ch;
	struct ipu_smfc *smfc;
	struct ipu_csi *csi;

	struct v4l2_mbus_framefmt format_mbus[CSI_NUM_PADS];
	const struct imx_media_pixfmt *cc[CSI_NUM_PADS];
	struct v4l2_rect crop;

	/* active vb2 buffers to send to video dev sink */
	struct imx_media_buffer *active_vb2_buf[2];
	struct imx_media_dma_buf underrun_buf;

	int ipu_buf_num;  /* ipu double buffer index: 0-1 */

	/* the sink for the captured frames */
	struct media_entity *sink;
	enum ipu_csi_dest dest;
	/* the source subdev */
	struct v4l2_subdev *src_sd;

	/* the mipi virtual channel number at link validate */
	int vc_num;

	/* the attached sensor at stream on */
	struct imx_media_subdev *sensor;

	spinlock_t irqlock; /* protect eof_irq handler */
	struct timer_list eof_timeout_timer;
	int eof_irq;
	int nfb4eof_irq;

	struct v4l2_ctrl_handler ctrl_hdlr;

	int power_count; /* power counter */
	bool stream_on;  /* streaming is on */
	bool last_eof;   /* waiting for last EOF at stream off */
	struct completion last_eof_comp;
};

static inline struct csi_priv *sd_to_dev(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct csi_priv, sd);
}

static void csi_idmac_put_ipu_resources(struct csi_priv *priv)
{
	if (!IS_ERR_OR_NULL(priv->idmac_ch))
		ipu_idmac_put(priv->idmac_ch);
	priv->idmac_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->smfc))
		ipu_smfc_put(priv->smfc);
	priv->smfc = NULL;
}

static int csi_idmac_get_ipu_resources(struct csi_priv *priv)
{
	int ch_num, ret;

	ch_num = IPUV3_CHANNEL_CSI0 + priv->smfc_id;

	priv->smfc = ipu_smfc_get(priv->ipu, ch_num);
	if (IS_ERR(priv->smfc)) {
		v4l2_err(&priv->sd, "failed to get SMFC\n");
		ret = PTR_ERR(priv->smfc);
		goto out;
	}

	priv->idmac_ch = ipu_idmac_get(priv->ipu, ch_num);
	if (IS_ERR(priv->idmac_ch)) {
		v4l2_err(&priv->sd, "could not get IDMAC channel %u\n",
			 ch_num);
		ret = PTR_ERR(priv->idmac_ch);
		goto out;
	}

	return 0;
out:
	csi_idmac_put_ipu_resources(priv);
	return ret;
}

static void csi_vb2_buf_done(struct csi_priv *priv)
{
	struct imx_media_video_dev *vdev = priv->vdev;
	struct imx_media_buffer *done, *next;
	struct vb2_buffer *vb;
	dma_addr_t phys;

	done = priv->active_vb2_buf[priv->ipu_buf_num];
	if (done) {
		vb = &done->vbuf.vb2_buf;
		vb->timestamp = ktime_get_ns();
		vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
	}

	/* get next queued buffer */
	next = imx_media_capture_device_next_buf(vdev);
	if (next) {
		phys = vb2_dma_contig_plane_dma_addr(&next->vbuf.vb2_buf, 0);
		priv->active_vb2_buf[priv->ipu_buf_num] = next;
	} else {
		phys = priv->underrun_buf.phys;
		priv->active_vb2_buf[priv->ipu_buf_num] = NULL;
	}

	if (ipu_idmac_buffer_is_ready(priv->idmac_ch, priv->ipu_buf_num))
		ipu_idmac_clear_buffer(priv->idmac_ch, priv->ipu_buf_num);

	ipu_cpmem_set_buffer(priv->idmac_ch, priv->ipu_buf_num, phys);
}

static void csi_call_fim(struct csi_priv *priv)
{
	if (priv->fim) {
		struct timespec cur_ts;

		ktime_get_ts(&cur_ts);
		/* call frame interval monitor */
		imx_media_fim_eof_monitor(priv->fim, &cur_ts);
	}
}

static irqreturn_t csi_idmac_eof_interrupt(int irq, void *dev_id)
{
	struct csi_priv *priv = dev_id;

	spin_lock(&priv->irqlock);

	if (priv->last_eof) {
		complete(&priv->last_eof_comp);
		priv->last_eof = false;
		goto unlock;
	}

	csi_call_fim(priv);

	csi_vb2_buf_done(priv);

	/* select new IPU buf */
	ipu_idmac_select_buffer(priv->idmac_ch, priv->ipu_buf_num);
	/* toggle IPU double-buffer index */
	priv->ipu_buf_num ^= 1;

	/* bump the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMX_MEDIA_EOF_TIMEOUT));

unlock:
	spin_unlock(&priv->irqlock);
	return IRQ_HANDLED;
}

static irqreturn_t csi_idmac_nfb4eof_interrupt(int irq, void *dev_id)
{
	struct csi_priv *priv = dev_id;
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_NEW_FRAME_BEFORE_EOF,
	};

	v4l2_err(&priv->sd, "NFB4EOF\n");

	v4l2_subdev_notify_event(&priv->sd, &ev);

	return IRQ_HANDLED;
}

/*
 * EOF timeout timer function. This is an unrecoverable condition
 * without a stream restart.
 */
static void csi_idmac_eof_timeout(unsigned long data)
{
	struct csi_priv *priv = (struct csi_priv *)data;
	struct imx_media_video_dev *vdev = priv->vdev;

	v4l2_err(&priv->sd, "EOF timeout\n");

	/* signal a fatal error to capture device */
	imx_media_capture_device_error(vdev);
}

static void csi_idmac_setup_vb2_buf(struct csi_priv *priv, dma_addr_t *phys)
{
	struct imx_media_video_dev *vdev = priv->vdev;
	struct imx_media_buffer *buf;
	int i;

	for (i = 0; i < 2; i++) {
		buf = imx_media_capture_device_next_buf(vdev);
		priv->active_vb2_buf[i] = buf;
		phys[i] = vb2_dma_contig_plane_dma_addr(&buf->vbuf.vb2_buf, 0);
	}
}

static void csi_idmac_unsetup_vb2_buf(struct csi_priv *priv,
				      enum vb2_buffer_state return_state)
{
	struct imx_media_buffer *buf;
	int i;

	/* return any remaining active frames with error */
	for (i = 0; i < 2; i++) {
		buf = priv->active_vb2_buf[i];
		if (buf) {
			struct vb2_buffer *vb = &buf->vbuf.vb2_buf;

			vb->timestamp = ktime_get_ns();
			vb2_buffer_done(vb, return_state);
		}
	}
}

/* init the SMFC IDMAC channel */
static int csi_idmac_setup_channel(struct csi_priv *priv)
{
	struct imx_media_video_dev *vdev = priv->vdev;
	struct v4l2_of_endpoint *sensor_ep;
	struct v4l2_mbus_framefmt *infmt;
	struct ipu_image image;
	u32 passthrough_bits;
	dma_addr_t phys[2];
	bool passthrough;
	u32 burst_size;
	int ret;

	infmt = &priv->format_mbus[CSI_SINK_PAD];
	sensor_ep = &priv->sensor->sensor_ep;

	ipu_cpmem_zero(priv->idmac_ch);

	memset(&image, 0, sizeof(image));
	image.pix = vdev->fmt.fmt.pix;
	image.rect.width = image.pix.width;
	image.rect.height = image.pix.height;

	csi_idmac_setup_vb2_buf(priv, phys);

	image.phys0 = phys[0];
	image.phys1 = phys[1];

	ret = ipu_cpmem_set_image(priv->idmac_ch, &image);
	if (ret)
		return ret;

	burst_size = (image.pix.width & 0xf) ? 8 : 16;

	ipu_cpmem_set_burstsize(priv->idmac_ch, burst_size);

	/*
	 * Check for conditions that require the IPU to handle the
	 * data internally as generic data, aka passthrough mode:
	 * - raw bayer formats
	 * - the sensor bus is 16-bit parallel
	 */
	switch (image.pix.pixelformat) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		burst_size = 8;
		passthrough = true;
		passthrough_bits = 8;
		break;
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SGBRG16:
	case V4L2_PIX_FMT_SGRBG16:
	case V4L2_PIX_FMT_SRGGB16:
		burst_size = 4;
		passthrough = true;
		passthrough_bits = 16;
		break;
	default:
		passthrough = (sensor_ep->bus_type != V4L2_MBUS_CSI2 &&
			       sensor_ep->bus.parallel.bus_width >= 16);
		passthrough_bits = 16;
		break;
	}

	if (passthrough)
		ipu_cpmem_set_format_passthrough(priv->idmac_ch,
						 passthrough_bits);

	/*
	 * Set the channel for the direct CSI-->memory via SMFC
	 * use-case to very high priority, by enabling the watermark
	 * signal in the SMFC, enabling WM in the channel, and setting
	 * the channel priority to high.
	 *
	 * Refer to the i.mx6 rev. D TRM Table 36-8: Calculated priority
	 * value.
	 *
	 * The WM's are set very low by intention here to ensure that
	 * the SMFC FIFOs do not overflow.
	 */
	ipu_smfc_set_watermark(priv->smfc, 0x02, 0x01);
	ipu_cpmem_set_high_priority(priv->idmac_ch);
	ipu_idmac_enable_watermark(priv->idmac_ch, true);
	ipu_cpmem_set_axi_id(priv->idmac_ch, 0);
	ipu_idmac_lock_enable(priv->idmac_ch, 8);

	burst_size = ipu_cpmem_get_burstsize(priv->idmac_ch);
	burst_size = passthrough ?
		(burst_size >> 3) - 1 : (burst_size >> 2) - 1;

	ipu_smfc_set_burstsize(priv->smfc, burst_size);

	if (image.pix.field == V4L2_FIELD_NONE &&
	    V4L2_FIELD_HAS_BOTH(infmt->field))
		ipu_cpmem_interlaced_scan(priv->idmac_ch,
					  image.pix.bytesperline);

	ipu_idmac_set_double_buffer(priv->idmac_ch, true);

	return 0;
}

static void csi_idmac_unsetup(struct csi_priv *priv,
			      enum vb2_buffer_state state)
{
	ipu_idmac_disable_channel(priv->idmac_ch);
	ipu_smfc_disable(priv->smfc);

	csi_idmac_unsetup_vb2_buf(priv, state);
}

static int csi_idmac_setup(struct csi_priv *priv)
{
	int ret;

	ret = csi_idmac_setup_channel(priv);
	if (ret)
		return ret;

	ipu_cpmem_dump(priv->idmac_ch);
	ipu_dump(priv->ipu);

	ipu_smfc_enable(priv->smfc);

	/* set buffers ready */
	ipu_idmac_select_buffer(priv->idmac_ch, 0);
	ipu_idmac_select_buffer(priv->idmac_ch, 1);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->idmac_ch);

	return 0;
}

static int csi_idmac_start(struct csi_priv *priv)
{
	struct imx_media_video_dev *vdev = priv->vdev;
	struct v4l2_pix_format *outfmt;
	int ret;

	ret = csi_idmac_get_ipu_resources(priv);
	if (ret)
		return ret;

	ipu_smfc_map_channel(priv->smfc, priv->csi_id, priv->vc_num);

	outfmt = &vdev->fmt.fmt.pix;

	ret = imx_media_alloc_dma_buf(priv->md, &priv->underrun_buf,
				      outfmt->sizeimage);
	if (ret)
		goto out_put_ipu;

	priv->ipu_buf_num = 0;

	/* init EOF completion waitq */
	init_completion(&priv->last_eof_comp);
	priv->last_eof = false;

	ret = csi_idmac_setup(priv);
	if (ret) {
		v4l2_err(&priv->sd, "csi_idmac_setup failed: %d\n", ret);
		goto out_free_dma_buf;
	}

	priv->nfb4eof_irq = ipu_idmac_channel_irq(priv->ipu,
						 priv->idmac_ch,
						 IPU_IRQ_NFB4EOF);
	ret = devm_request_irq(priv->dev, priv->nfb4eof_irq,
			       csi_idmac_nfb4eof_interrupt, 0,
			       "imx-smfc-nfb4eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering NFB4EOF irq: %d\n", ret);
		goto out_unsetup;
	}

	priv->eof_irq = ipu_idmac_channel_irq(priv->ipu, priv->idmac_ch,
					      IPU_IRQ_EOF);

	ret = devm_request_irq(priv->dev, priv->eof_irq,
			       csi_idmac_eof_interrupt, 0,
			       "imx-smfc-eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering eof irq: %d\n", ret);
		goto out_free_nfb4eof_irq;
	}

	/* start the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMX_MEDIA_EOF_TIMEOUT));

	return 0;

out_free_nfb4eof_irq:
	devm_free_irq(priv->dev, priv->nfb4eof_irq, priv);
out_unsetup:
	csi_idmac_unsetup(priv, VB2_BUF_STATE_QUEUED);
out_free_dma_buf:
	imx_media_free_dma_buf(priv->md, &priv->underrun_buf);
out_put_ipu:
	csi_idmac_put_ipu_resources(priv);
	return ret;
}

static void csi_idmac_stop(struct csi_priv *priv)
{
	unsigned long flags;
	int ret;

	/* mark next EOF interrupt as the last before stream off */
	spin_lock_irqsave(&priv->irqlock, flags);
	priv->last_eof = true;
	spin_unlock_irqrestore(&priv->irqlock, flags);

	/*
	 * and then wait for interrupt handler to mark completion.
	 */
	ret = wait_for_completion_timeout(
		&priv->last_eof_comp, msecs_to_jiffies(IMX_MEDIA_EOF_TIMEOUT));
	if (ret == 0)
		v4l2_warn(&priv->sd, "wait last EOF timeout\n");

	devm_free_irq(priv->dev, priv->eof_irq, priv);
	devm_free_irq(priv->dev, priv->nfb4eof_irq, priv);

	csi_idmac_unsetup(priv, VB2_BUF_STATE_ERROR);

	imx_media_free_dma_buf(priv->md, &priv->underrun_buf);

	/* cancel the EOF timeout timer */
	del_timer_sync(&priv->eof_timeout_timer);

	csi_idmac_put_ipu_resources(priv);
}

/* Update the CSI whole sensor and active windows */
static int csi_setup(struct csi_priv *priv)
{
	struct v4l2_mbus_framefmt *infmt, *outfmt;
	struct v4l2_mbus_config sensor_mbus_cfg;
	struct v4l2_of_endpoint *sensor_ep;
	struct v4l2_mbus_framefmt if_fmt;

	infmt = &priv->format_mbus[CSI_SINK_PAD];
	outfmt = &priv->format_mbus[priv->active_output_pad];
	sensor_ep = &priv->sensor->sensor_ep;

	/* compose mbus_config from sensor endpoint */
	sensor_mbus_cfg.type = sensor_ep->bus_type;
	sensor_mbus_cfg.flags = (sensor_ep->bus_type == V4L2_MBUS_CSI2) ?
		sensor_ep->bus.mipi_csi2.flags :
		sensor_ep->bus.parallel.flags;

	/*
	 * we need to pass input sensor frame to CSI interface, but
	 * with translated field type from output format
	 */
	if_fmt = *infmt;
	if_fmt.field = outfmt->field;

	ipu_csi_set_window(priv->csi, &priv->crop);

	ipu_csi_set_downsize(priv->csi,
			     priv->crop.width == 2 * outfmt->width,
			     priv->crop.height == 2 * outfmt->height);

	ipu_csi_init_interface(priv->csi, &sensor_mbus_cfg, &if_fmt);

	ipu_csi_set_dest(priv->csi, priv->dest);

	ipu_csi_dump(priv->csi);

	return 0;
}

static int csi_start(struct csi_priv *priv)
{
	int ret;

	if (!priv->sensor) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return -EINVAL;
	}

	if (priv->dest == IPU_CSI_DEST_IDMAC) {
		ret = csi_idmac_start(priv);
		if (ret)
			return ret;
	}

	ret = csi_setup(priv);
	if (ret)
		goto idmac_stop;

	/* start the frame interval monitor */
	if (priv->fim) {
		ret = imx_media_fim_set_stream(priv->fim, priv->sensor, true);
		if (ret)
			goto idmac_stop;
	}

	ret = ipu_csi_enable(priv->csi);
	if (ret) {
		v4l2_err(&priv->sd, "CSI enable error: %d\n", ret);
		goto fim_off;
	}

	return 0;

fim_off:
	if (priv->fim)
		imx_media_fim_set_stream(priv->fim, priv->sensor, false);
idmac_stop:
	if (priv->dest == IPU_CSI_DEST_IDMAC)
		csi_idmac_stop(priv);
	return ret;
}

static void csi_stop(struct csi_priv *priv)
{
	if (priv->dest == IPU_CSI_DEST_IDMAC)
		csi_idmac_stop(priv);

	/* stop the frame interval monitor */
	if (priv->fim)
		imx_media_fim_set_stream(priv->fim, priv->sensor, false);

	ipu_csi_disable(priv->csi);
}

/*
 * V4L2 subdev operations.
 */

static int csi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&priv->lock);

	if (!priv->src_sd || !priv->sink) {
		ret = -EPIPE;
		goto out;
	}

	dev_dbg(priv->dev, "stream %s\n", enable ? "ON" : "OFF");

	if (enable && !priv->stream_on)
		ret = csi_start(priv);
	else if (!enable && priv->stream_on)
		csi_stop(priv);

	if (!ret)
		priv->stream_on = enable;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_s_power(struct v4l2_subdev *sd, int on)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	int ret = 0;

	dev_dbg(priv->dev, "power %s\n", on ? "ON" : "OFF");

	mutex_lock(&priv->lock);

	/*
	 * If the power count is modified from 0 to != 0 or from != 0 to 0,
	 * update the power state.
	 */
	if (priv->power_count == !on) {
		if (priv->fim) {
			ret = imx_media_fim_set_power(priv->fim, on);
			if (ret)
				goto out;
		}
	}

	/* Update the power count. */
	priv->power_count += on ? 1 : -1;
	WARN_ON(priv->power_count < 0);
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_link_setup(struct media_entity *entity,
			  const struct media_pad *local,
			  const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev *remote_sd;
	int ret = 0;

	dev_dbg(priv->dev, "link setup %s -> %s\n", remote->entity->name,
		local->entity->name);

	mutex_lock(&priv->lock);

	if (local->flags & MEDIA_PAD_FL_SINK) {
		if (!is_media_entity_v4l2_subdev(remote->entity)) {
			ret = -EINVAL;
			goto out;
		}

		remote_sd = media_entity_to_v4l2_subdev(remote->entity);

		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (priv->src_sd) {
				ret = -EBUSY;
				goto out;
			}
			priv->src_sd = remote_sd;
		} else {
			priv->src_sd = NULL;
		}

		goto out;
	}

	/* this is a source pad */

	if (flags & MEDIA_LNK_FL_ENABLED) {
		if (priv->sink) {
			ret = -EBUSY;
			goto out;
		}
	} else {
		priv->sink = NULL;
		goto out;
	}

	/* record which output pad is now active */
	priv->active_output_pad = local->index;

	/* set CSI destination */
	if (local->index == CSI_SRC_PAD_IDMAC) {
		if (!is_media_entity_v4l2_video_device(remote->entity)) {
			ret = -EINVAL;
			goto out;
		}

		priv->dest = IPU_CSI_DEST_IDMAC;
	} else {
		if (!is_media_entity_v4l2_subdev(remote->entity)) {
			ret = -EINVAL;
			goto out;
		}

		remote_sd = media_entity_to_v4l2_subdev(remote->entity);
		switch (remote_sd->grp_id) {
		case IMX_MEDIA_GRP_ID_VDIC:
			priv->dest = IPU_CSI_DEST_VDIC;
			break;
		case IMX_MEDIA_GRP_ID_IC_PRP:
			priv->dest = IPU_CSI_DEST_IC;
			break;
		default:
			ret = -EINVAL;
			goto out;
		}
	}

	priv->sink = remote->entity;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_link_validate(struct v4l2_subdev *sd,
			     struct media_link *link,
			     struct v4l2_subdev_format *source_fmt,
			     struct v4l2_subdev_format *sink_fmt)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	const struct imx_media_pixfmt *incc;
	struct v4l2_of_endpoint *sensor_ep;
	struct imx_media_subdev *sensor;
	bool is_csi2;
	int ret;

	ret = v4l2_subdev_link_validate_default(sd, link,
						source_fmt, sink_fmt);
	if (ret)
		return ret;

	ret = v4l2_subdev_link_validate_frame_interval(link);
	if (ret)
		return ret;

	sensor = __imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(priv->sensor);
	}

	mutex_lock(&priv->lock);

	priv->sensor = sensor;
	sensor_ep = &priv->sensor->sensor_ep;
	is_csi2 = (sensor_ep->bus_type == V4L2_MBUS_CSI2);
	incc = priv->cc[CSI_SINK_PAD];

	if (priv->dest != IPU_CSI_DEST_IDMAC &&
	    (incc->bayer || (!is_csi2 &&
			     sensor_ep->bus.parallel.bus_width >= 16))) {
		v4l2_err(&priv->sd,
			 "bayer/16-bit parallel buses must go to IDMAC pad\n");
		ret = -EINVAL;
		goto out;
	}

	if (is_csi2) {
		int vc_num = 0;
		/*
		 * NOTE! It seems the virtual channels from the mipi csi-2
		 * receiver are used only for routing by the video mux's,
		 * or for hard-wired routing to the CSI's. Once the stream
		 * enters the CSI's however, they are treated internally
		 * in the IPU as virtual channel 0.
		 */
#if 0
		mutex_unlock(&priv->lock);
		vc_num = imx_media_find_mipi_csi2_channel(priv->md,
							  &priv->sd.entity);
		if (vc_num < 0)
			return vc_num;
		mutex_lock(&priv->lock);
#endif
		ipu_csi_set_mipi_datatype(priv->csi, vc_num,
					  &priv->format_mbus[CSI_SINK_PAD]);
	}

	/* select either parallel or MIPI-CSI2 as input to CSI */
	ipu_set_csi_src_mux(priv->ipu, priv->csi_id, is_csi2);
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_eof_isr(struct v4l2_subdev *sd, u32 status, bool *handled)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	unsigned long flags;

	spin_lock_irqsave(&priv->irqlock, flags);
	csi_call_fim(priv);
	spin_unlock_irqrestore(&priv->irqlock, flags);

	return 0;
}

static int csi_try_crop(struct csi_priv *priv, struct v4l2_rect *crop,
			struct imx_media_subdev *sensor)
{
	struct v4l2_of_endpoint *sensor_ep;
	struct v4l2_mbus_framefmt *infmt;

	infmt = &priv->format_mbus[CSI_SINK_PAD];
	sensor_ep = &sensor->sensor_ep;

	crop->width = min_t(__u32, infmt->width, crop->width);
	if (crop->left + crop->width > infmt->width)
		crop->left = infmt->width - crop->width;
	/* adjust crop left/width to h/w alignment restrictions */
	crop->left &= ~0x3;
	crop->width &= ~0x7;

	/*
	 * FIXME: not sure why yet, but on interlaced bt.656,
	 * changing the vertical cropping causes loss of vertical
	 * sync, so fix it to NTSC/PAL active lines. NTSC contains
	 * 2 extra lines of active video that need to be cropped.
	 */
	if (sensor_ep->bus_type == V4L2_MBUS_BT656 &&
	    (V4L2_FIELD_HAS_BOTH(infmt->field) ||
	     infmt->field == V4L2_FIELD_ALTERNATE)) {
		crop->height = infmt->height;
		crop->top = (infmt->height == 480) ? 2 : 0;
	} else {
		crop->height = min_t(__u32, infmt->height, crop->height);
		if (crop->top + crop->height > infmt->height)
			crop->top = infmt->height - crop->height;
	}

	return 0;
}

static int csi_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= CSI_NUM_PADS)
		return -EINVAL;

	if (code->pad == CSI_SRC_PAD_DIRECT)
		return imx_media_enum_ipu_format(NULL, &code->code,
						 code->index, true);

	return imx_media_enum_format(NULL, &code->code, code->index,
				     true, false);
}

static int csi_get_fmt(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_format *sdformat)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);

	if (sdformat->pad >= CSI_NUM_PADS)
		return -EINVAL;

	mutex_lock(&priv->lock);

	sdformat->format = priv->format_mbus[sdformat->pad];

	mutex_unlock(&priv->lock);

	return 0;
}

static int csi_set_fmt(struct v4l2_subdev *sd,
		       struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_format *sdformat)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	const struct imx_media_pixfmt *cc, *incc;
	struct v4l2_mbus_framefmt *infmt;
	struct imx_media_subdev *sensor;
	struct v4l2_rect crop;
	int ret = 0;
	u32 code;

	if (sdformat->pad >= CSI_NUM_PADS)
		return -EINVAL;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	mutex_lock(&priv->lock);

	if (priv->stream_on) {
		ret = -EBUSY;
		goto out;
	}

	infmt = &priv->format_mbus[CSI_SINK_PAD];

	v4l_bound_align_image(&sdformat->format.width, MIN_W, MAX_W,
			      W_ALIGN, &sdformat->format.height,
			      MIN_H, MAX_H, H_ALIGN, S_ALIGN);

	switch (sdformat->pad) {
	case CSI_SRC_PAD_DIRECT:
	case CSI_SRC_PAD_IDMAC:
		if (sdformat->format.width < priv->crop.width * 3 / 4)
			sdformat->format.width = priv->crop.width / 2;
		else
			sdformat->format.width = priv->crop.width;

		if (sdformat->format.height < priv->crop.height * 3 / 4)
			sdformat->format.height = priv->crop.height / 2;
		else
			sdformat->format.height = priv->crop.height;

		if (sdformat->pad == CSI_SRC_PAD_IDMAC) {
			cc = imx_media_find_format(0, sdformat->format.code,
						   true, false);
			if (!cc) {
				imx_media_enum_format(NULL, &code, 0,
						      true, false);
				cc = imx_media_find_format(0, code,
							   true, false);
				sdformat->format.code = cc->codes[0];
			}

			incc = priv->cc[CSI_SINK_PAD];
			if (cc->cs != incc->cs) {
				sdformat->format.code = infmt->code;
				cc = imx_media_find_format(
					0, sdformat->format.code,
					true, false);
			}

			if (sdformat->format.field != V4L2_FIELD_NONE)
				sdformat->format.field = infmt->field;
		} else {
			cc = imx_media_find_ipu_format(0, sdformat->format.code,
						       true);
			if (!cc) {
				imx_media_enum_ipu_format(NULL, &code, 0, true);
				cc = imx_media_find_ipu_format(0, code, true);
				sdformat->format.code = cc->codes[0];
			}

			sdformat->format.field = infmt->field;
		}

		/*
		 * translate V4L2_FIELD_ALTERNATE to SEQ_TB or SEQ_BT
		 * depending on input height (assume NTSC top-bottom
		 * order if 480 lines, otherwise PAL bottom-top order).
		 */
		if (sdformat->format.field == V4L2_FIELD_ALTERNATE) {
			sdformat->format.field =  (infmt->height == 480) ?
				V4L2_FIELD_SEQ_TB : V4L2_FIELD_SEQ_BT;
		}
		break;
	case CSI_SINK_PAD:
		crop.left = 0;
		crop.top = 0;
		crop.width = sdformat->format.width;
		crop.height = sdformat->format.height;
		ret = csi_try_crop(priv, &crop, sensor);
		if (ret)
			goto out;

		cc = imx_media_find_format(0, sdformat->format.code,
					   true, false);
		if (!cc) {
			imx_media_enum_format(NULL, &code, 0, true, false);
			cc = imx_media_find_format(0, code, true, false);
			sdformat->format.code = cc->codes[0];
		}
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	if (sdformat->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = sdformat->format;
	} else {
		priv->format_mbus[sdformat->pad] = sdformat->format;
		priv->cc[sdformat->pad] = cc;
		/* Reset the crop window if this is the input pad */
		if (sdformat->pad == CSI_SINK_PAD)
			priv->crop = crop;
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_get_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_selection *sel)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *infmt;
	int ret = 0;

	if (sel->pad >= CSI_NUM_PADS || sel->pad == CSI_SINK_PAD)
		return -EINVAL;

	mutex_lock(&priv->lock);

	infmt = &priv->format_mbus[CSI_SINK_PAD];

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = infmt->width;
		sel->r.height = infmt->height;
		break;
	case V4L2_SEL_TGT_CROP:
		sel->r = priv->crop;
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_set_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg,
			     struct v4l2_subdev_selection *sel)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *outfmt;
	struct imx_media_subdev *sensor;
	int ret = 0;

	if (sel->pad >= CSI_NUM_PADS ||
	    sel->pad == CSI_SINK_PAD ||
	    sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	sensor = imx_media_find_sensor(priv->md, &priv->sd.entity);
	if (IS_ERR(sensor)) {
		v4l2_err(&priv->sd, "no sensor attached\n");
		return PTR_ERR(sensor);
	}

	mutex_lock(&priv->lock);

	if (priv->stream_on) {
		ret = -EBUSY;
		goto out;
	}

	/*
	 * Modifying the crop rectangle always changes the format on the source
	 * pad. If the KEEP_CONFIG flag is set, just return the current crop
	 * rectangle.
	 */
	if (sel->flags & V4L2_SEL_FLAG_KEEP_CONFIG) {
		sel->r = priv->crop;
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY)
			cfg->try_crop = sel->r;
		goto out;
	}

	outfmt = &priv->format_mbus[sel->pad];

	ret = csi_try_crop(priv, &sel->r, sensor);
	if (ret)
		goto out;

	if (sel->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_crop = sel->r;
	} else {
		priv->crop = sel->r;
		/* Update the source format */
		outfmt->width = sel->r.width;
		outfmt->height = sel->r.height;
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csi_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			       struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_NEW_FRAME_BEFORE_EOF &&
	    sub->type != V4L2_EVENT_FRAME_INTERVAL_ERROR)
		return -EINVAL;
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static int csi_unsubscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
				 struct v4l2_event_subscription *sub)
{
	return v4l2_event_unsubscribe(fh, sub);
}

/*
 * retrieve our pads parsed from the OF graph by the media device
 */
static int csi_registered(struct v4l2_subdev *sd)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);
	int i, ret;
	u32 code;

	/* get media device */
	priv->md = dev_get_drvdata(sd->v4l2_dev->dev);

	/* get handle to IPU CSI */
	priv->csi = ipu_csi_get(priv->ipu, priv->csi_id);
	if (IS_ERR(priv->csi)) {
		v4l2_err(&priv->sd, "failed to get CSI%d\n", priv->csi_id);
		return PTR_ERR(priv->csi);
	}

	for (i = 0; i < CSI_NUM_PADS; i++) {
		priv->pad[i].flags = (i == CSI_SINK_PAD) ?
			MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;

		code = 0;
		if (i == CSI_SRC_PAD_DIRECT)
			imx_media_enum_ipu_format(NULL, &code, 0, true);

		/* set a default mbus format  */
		ret = imx_media_init_mbus_fmt(&priv->format_mbus[i],
					      640, 480, code, V4L2_FIELD_NONE,
					      &priv->cc[i]);
		if (ret)
			goto put_csi;
	}

	priv->fim = imx_media_fim_init(&priv->sd);
	if (IS_ERR(priv->fim)) {
		ret = PTR_ERR(priv->fim);
		goto put_csi;
	}

	ret = media_entity_pads_init(&sd->entity, CSI_NUM_PADS, priv->pad);
	if (ret)
		goto free_fim;

	ret = imx_media_capture_device_register(priv->vdev);
	if (ret)
		goto free_fim;

	return 0;

free_fim:
	if (priv->fim)
		imx_media_fim_free(priv->fim);
put_csi:
	ipu_csi_put(priv->csi);
	return ret;
}

static void csi_unregistered(struct v4l2_subdev *sd)
{
	struct csi_priv *priv = v4l2_get_subdevdata(sd);

	imx_media_capture_device_unregister(priv->vdev);

	if (priv->fim)
		imx_media_fim_free(priv->fim);

	if (!IS_ERR_OR_NULL(priv->csi))
		ipu_csi_put(priv->csi);
}

static struct media_entity_operations csi_entity_ops = {
	.link_setup = csi_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

static struct v4l2_subdev_core_ops csi_core_ops = {
	.s_power = csi_s_power,
	.interrupt_service_routine = csi_eof_isr,
	.subscribe_event = csi_subscribe_event,
	.unsubscribe_event = csi_unsubscribe_event,
};

static struct v4l2_subdev_video_ops csi_video_ops = {
	.s_stream = csi_s_stream,
};

static struct v4l2_subdev_pad_ops csi_pad_ops = {
	.enum_mbus_code = csi_enum_mbus_code,
	.get_fmt = csi_get_fmt,
	.set_fmt = csi_set_fmt,
	.get_selection = csi_get_selection,
	.set_selection = csi_set_selection,
	.link_validate = csi_link_validate,
};

static struct v4l2_subdev_ops csi_subdev_ops = {
	.core = &csi_core_ops,
	.video = &csi_video_ops,
	.pad = &csi_pad_ops,
};

static struct v4l2_subdev_internal_ops csi_internal_ops = {
	.registered = csi_registered,
	.unregistered = csi_unregistered,
};

static int imx_csi_probe(struct platform_device *pdev)
{
	struct ipu_client_platformdata *pdata;
	struct pinctrl *pinctrl;
	struct csi_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, &priv->sd);
	priv->dev = &pdev->dev;

	ret = dma_set_coherent_mask(priv->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* get parent IPU */
	priv->ipu = dev_get_drvdata(priv->dev->parent);

	/* get our CSI id */
	pdata = priv->dev->platform_data;
	priv->csi_id = pdata->csi;
	priv->smfc_id = (priv->csi_id == 0) ? 0 : 2;

	init_timer(&priv->eof_timeout_timer);
	priv->eof_timeout_timer.data = (unsigned long)priv;
	priv->eof_timeout_timer.function = csi_idmac_eof_timeout;
	spin_lock_init(&priv->irqlock);

	v4l2_subdev_init(&priv->sd, &csi_subdev_ops);
	v4l2_set_subdevdata(&priv->sd, priv);
	priv->sd.internal_ops = &csi_internal_ops;
	priv->sd.entity.ops = &csi_entity_ops;
	priv->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	priv->sd.dev = &pdev->dev;
	priv->sd.of_node = pdata->of_node;
	priv->sd.owner = THIS_MODULE;
	priv->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	priv->sd.grp_id = priv->csi_id ?
		IMX_MEDIA_GRP_ID_CSI1 : IMX_MEDIA_GRP_ID_CSI0;
	imx_media_grp_id_to_sd_name(priv->sd.name, sizeof(priv->sd.name),
				    priv->sd.grp_id, ipu_get_num(priv->ipu));

	priv->vdev = imx_media_capture_device_init(&priv->sd,
						   CSI_SRC_PAD_IDMAC);
	if (IS_ERR(priv->vdev))
		return PTR_ERR(priv->vdev);

	v4l2_ctrl_handler_init(&priv->ctrl_hdlr, 0);
	priv->sd.ctrl_handler = &priv->ctrl_hdlr;

	/*
	 * The IPUv3 driver did not assign an of_node to this
	 * device. As a result, pinctrl does not automatically
	 * configure our pin groups, so we need to do that manually
	 * here, after setting this device's of_node.
	 */
	priv->dev->of_node = pdata->of_node;
	pinctrl = devm_pinctrl_get_select_default(priv->dev);

	mutex_init(&priv->lock);

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret)
		goto free;

	return 0;
free:
	v4l2_ctrl_handler_free(&priv->ctrl_hdlr);
	mutex_destroy(&priv->lock);
	imx_media_capture_device_remove(priv->vdev);
	return ret;
}

static int imx_csi_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct csi_priv *priv = sd_to_dev(sd);

	v4l2_ctrl_handler_free(&priv->ctrl_hdlr);
	mutex_destroy(&priv->lock);
	imx_media_capture_device_remove(priv->vdev);
	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static const struct platform_device_id imx_csi_ids[] = {
	{ .name = "imx-ipuv3-csi" },
	{ },
};
MODULE_DEVICE_TABLE(platform, imx_csi_ids);

static struct platform_driver imx_csi_driver = {
	.probe = imx_csi_probe,
	.remove = imx_csi_remove,
	.id_table = imx_csi_ids,
	.driver = {
		.name = "imx-ipuv3-csi",
	},
};
module_platform_driver(imx_csi_driver);

MODULE_DESCRIPTION("i.MX CSI subdev driver");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:imx-ipuv3-csi");
