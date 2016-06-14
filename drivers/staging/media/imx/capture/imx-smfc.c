/*
 * V4L2 Capture SMFC Subdev for Freescale i.MX5/6 SOC
 *
 * This subdevice handles capture of raw/unconverted video frames
 * from the CSI, directly to memory via the Sensor Multi-FIFO Controller.
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
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <video/imx-ipu-v3.h>
#include <media/imx.h>
#include "imx-camif.h"

struct imx_smfc_priv {
	struct imxcam_dev    *dev;
	struct v4l2_subdev    sd;

	struct ipu_soc *ipu;
	struct ipuv3_channel *smfc_ch;
	struct ipu_smfc *smfc;

	struct v4l2_mbus_framefmt inf; /* input sensor format */
	struct v4l2_pix_format outf;   /* output user format */

	/* active (undergoing DMA) buffers, one for each IPU buffer */
	struct imxcam_buffer *active_frame[2];

	struct imxcam_dma_buf underrun_buf;
	int buf_num;

	struct timer_list eof_timeout_timer;
	int eof_irq;
	int nfb4eof_irq;

	bool last_eof;  /* waiting for last EOF at stream off */
	struct completion last_eof_comp;
};

static void imx_smfc_put_ipu_resources(struct imx_smfc_priv *priv)
{
	if (!IS_ERR_OR_NULL(priv->smfc_ch))
		ipu_idmac_put(priv->smfc_ch);
	priv->smfc_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->smfc))
		ipu_smfc_put(priv->smfc);
	priv->smfc = NULL;
}

static int imx_smfc_get_ipu_resources(struct imx_smfc_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	int csi_id = dev->sensor->csi_ep.base.port;
	struct v4l2_subdev *csi_sd = dev->sensor->csi_sd;
	int csi_ch_num, ret;

	priv->ipu = dev_get_drvdata(csi_sd->dev->parent);

	/*
	 * Choose the direct CSI-->SMFC-->MEM channel corresponding
	 * to the IPU and CSI IDs.
	 */
	csi_ch_num = IPUV3_CHANNEL_CSI0 +
		(ipu_get_num(priv->ipu) << 1) + csi_id;

	priv->smfc = ipu_smfc_get(priv->ipu, csi_ch_num);
	if (IS_ERR(priv->smfc)) {
		v4l2_err(&priv->sd, "failed to get SMFC\n");
		ret = PTR_ERR(priv->smfc);
		goto out;
	}

	priv->smfc_ch = ipu_idmac_get(priv->ipu, csi_ch_num);
	if (IS_ERR(priv->smfc_ch)) {
		v4l2_err(&priv->sd, "could not get IDMAC channel %u\n",
			 csi_ch_num);
		ret = PTR_ERR(priv->smfc_ch);
		goto out;
	}

	return 0;
out:
	imx_smfc_put_ipu_resources(priv);
	return ret;
}

static irqreturn_t imx_smfc_eof_interrupt(int irq, void *dev_id)
{
	struct imx_smfc_priv *priv = dev_id;
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	struct imxcam_buffer *frame;
	enum vb2_buffer_state state;
	struct timeval cur_timeval;
	unsigned long flags;
	u64 cur_time_ns;
	dma_addr_t phys;

	spin_lock_irqsave(&dev->irqlock, flags);

	cur_time_ns = ktime_get_ns();
	cur_timeval = ns_to_timeval(cur_time_ns);

	/* timestamp and return the completed frame */
	frame = priv->active_frame[priv->buf_num];
	if (frame) {
		frame->vb.timestamp = cur_time_ns;
		state = (dev->signal_locked &&
			 !atomic_read(&dev->pending_restart)) ?
			VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
		vb2_buffer_done(&frame->vb, state);
	}

	if (priv->last_eof) {
		complete(&priv->last_eof_comp);
		priv->active_frame[priv->buf_num] = NULL;
		priv->last_eof = false;
		goto unlock;
	}

	if (dev->fim.eof && dev->fim.eof(dev, &cur_timeval))
		v4l2_subdev_notify(&priv->sd, IMXCAM_FRAME_INTERVAL_NOTIFY,
				   NULL);

	/* bump the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));

	if (!list_empty(&ctx->ready_q)) {
		frame = list_entry(ctx->ready_q.next,
				   struct imxcam_buffer, list);
		phys = vb2_dma_contig_plane_dma_addr(&frame->vb, 0);
		list_del(&frame->list);
		priv->active_frame[priv->buf_num] = frame;
	} else {
		phys = priv->underrun_buf.phys;
		priv->active_frame[priv->buf_num] = NULL;
	}

	if (ipu_idmac_buffer_is_ready(priv->smfc_ch, priv->buf_num))
		ipu_idmac_clear_buffer(priv->smfc_ch, priv->buf_num);

	ipu_cpmem_set_buffer(priv->smfc_ch, priv->buf_num, phys);
	ipu_idmac_select_buffer(priv->smfc_ch, priv->buf_num);

	priv->buf_num ^= 1;

unlock:
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return IRQ_HANDLED;
}

static irqreturn_t imx_smfc_nfb4eof_interrupt(int irq, void *dev_id)
{
	struct imx_smfc_priv *priv = dev_id;

	v4l2_err(&priv->sd, "NFB4EOF\n");

	v4l2_subdev_notify(&priv->sd, IMXCAM_NFB4EOF_NOTIFY, NULL);

	return IRQ_HANDLED;
}

/*
 * EOF timeout timer function.
 */
static void imx_smfc_eof_timeout(unsigned long data)
{
	struct imx_smfc_priv *priv = (struct imx_smfc_priv *)data;

	v4l2_err(&priv->sd, "EOF timeout\n");

	v4l2_subdev_notify(&priv->sd, IMXCAM_EOF_TIMEOUT_NOTIFY, NULL);
}

static void imx_smfc_free_dma_buf(struct imx_smfc_priv *priv,
				 struct imxcam_dma_buf *buf)
{
	struct imxcam_dev *dev = priv->dev;

	if (buf->virt)
		dma_free_coherent(dev->dev, buf->len, buf->virt, buf->phys);

	buf->virt = NULL;
	buf->phys = 0;
}

static int imx_smfc_alloc_dma_buf(struct imx_smfc_priv *priv,
				 struct imxcam_dma_buf *buf,
				 int size)
{
	struct imxcam_dev *dev = priv->dev;

	imx_smfc_free_dma_buf(priv, buf);

	buf->len = PAGE_ALIGN(size);
	buf->virt = dma_alloc_coherent(dev->dev, buf->len, &buf->phys,
				       GFP_DMA | GFP_KERNEL);
	if (!buf->virt) {
		v4l2_err(&priv->sd, "failed to alloc dma buffer\n");
		return -ENOMEM;
	}

	return 0;
}

/* init the IC PRPENC-->MEM IDMAC channel */
static void imx_smfc_setup_channel(struct imx_smfc_priv *priv,
				  dma_addr_t addr0, dma_addr_t addr1,
				  bool rot_swap_width_height)
{
	struct imxcam_dev *dev = priv->dev;
	int csi_id = dev->sensor->csi_ep.base.port;
	int vc_num = dev->sensor->csi_ep.base.id;
	u32 width, height, stride;
	unsigned int burst_size;
	struct ipu_image image;
	bool passthrough;

	width = priv->outf.width;
	height = priv->outf.height;

	stride = dev->user_pixfmt->y_depth ?
		(width * dev->user_pixfmt->y_depth) >> 3 :
		(width * dev->user_pixfmt->bpp) >> 3;

	ipu_cpmem_zero(priv->smfc_ch);

	memset(&image, 0, sizeof(image));
	image.pix.width = image.rect.width = width;
	image.pix.height = image.rect.height = height;
	image.pix.bytesperline = stride;
	image.pix.pixelformat = priv->outf.pixelformat;
	image.phys0 = addr0;
	image.phys1 = addr1;
	ipu_cpmem_set_image(priv->smfc_ch, &image);

	burst_size = (width & 0xf) ? 8 : 16;

	ipu_cpmem_set_burstsize(priv->smfc_ch, burst_size);

	/*
	 * If the sensor uses 16-bit parallel CSI bus, we must handle
	 * the data internally in the IPU as 16-bit generic, aka
	 * passthrough mode.
	 */
	passthrough = (dev->sensor->ep.bus_type != V4L2_MBUS_CSI2 &&
		       dev->sensor->ep.bus.parallel.bus_width >= 16);

	if (passthrough)
		ipu_cpmem_set_format_passthrough(priv->smfc_ch, 16);

	if (dev->sensor->ep.bus_type == V4L2_MBUS_CSI2)
		ipu_smfc_map_channel(priv->smfc, csi_id, vc_num);
	else
		ipu_smfc_map_channel(priv->smfc, csi_id, 0);

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
	ipu_cpmem_set_high_priority(priv->smfc_ch);
	ipu_idmac_enable_watermark(priv->smfc_ch, true);
	ipu_cpmem_set_axi_id(priv->smfc_ch, 0);
	ipu_idmac_lock_enable(priv->smfc_ch, 8);

	burst_size = ipu_cpmem_get_burstsize(priv->smfc_ch);
	burst_size = passthrough ? (burst_size >> 3) - 1 : (burst_size >> 2) - 1;

	ipu_smfc_set_burstsize(priv->smfc, burst_size);

	if (V4L2_FIELD_HAS_BOTH(priv->inf.field))
		ipu_cpmem_interlaced_scan(priv->smfc_ch, stride);

	ipu_idmac_set_double_buffer(priv->smfc_ch, true);
}

static int imx_smfc_setup(struct imx_smfc_priv *priv,
			  dma_addr_t phys0, dma_addr_t phys1)
{
	int ret;

	ret = imx_smfc_alloc_dma_buf(priv, &priv->underrun_buf,
				    priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc underrun_buf, %d\n", ret);
		return ret;
	}

	imx_smfc_setup_channel(priv, phys0, phys1, false);

	ipu_cpmem_dump(priv->smfc_ch);
	ipu_dump(priv->ipu);

	ipu_smfc_enable(priv->smfc);

	/* set buffers ready */
	ipu_idmac_select_buffer(priv->smfc_ch, 0);
	ipu_idmac_select_buffer(priv->smfc_ch, 1);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->smfc_ch);

	return 0;
}

static int imx_smfc_start(struct imx_smfc_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	struct imxcam_buffer *frame, *tmp;
	dma_addr_t phys[2] = {0};
	int i = 0, ret;

	ret = imx_smfc_get_ipu_resources(priv);
	if (ret)
		return ret;

	list_for_each_entry_safe(frame, tmp, &ctx->ready_q, list) {
		phys[i] = vb2_dma_contig_plane_dma_addr(&frame->vb, 0);
		list_del(&frame->list);
		priv->active_frame[i++] = frame;
		if (i >= 2)
			break;
	}

	priv->inf = dev->sensor_fmt;
	priv->inf.width = dev->crop.width;
	priv->inf.height = dev->crop.height;
	priv->outf = dev->user_fmt.fmt.pix;

	priv->buf_num = 0;

	/* init EOF completion waitq */
	init_completion(&priv->last_eof_comp);
	priv->last_eof = false;

	ret = imx_smfc_setup(priv, phys[0], phys[1]);
	if (ret)
		goto out_put_ipu;

	priv->nfb4eof_irq = ipu_idmac_channel_irq(priv->ipu,
						 priv->smfc_ch,
						 IPU_IRQ_NFB4EOF);
	ret = devm_request_irq(dev->dev, priv->nfb4eof_irq,
			       imx_smfc_nfb4eof_interrupt, 0,
			       "imxcam-enc-nfb4eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering encode NFB4EOF irq: %d\n", ret);
		goto out_put_ipu;
	}

	priv->eof_irq = ipu_idmac_channel_irq(priv->ipu, priv->smfc_ch,
					      IPU_IRQ_EOF);

	ret = devm_request_irq(dev->dev, priv->eof_irq,
			       imx_smfc_eof_interrupt, 0,
			       "imxcam-enc-eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering encode eof irq: %d\n", ret);
		goto out_free_nfb4eof_irq;
	}

	/* sensor stream on */
	ret = dev->sensor_set_stream(dev, 1);
	if (ret) {
		v4l2_err(&priv->sd, "sensor stream on failed\n");
		goto out_free_eof_irq;
	}

	/* start the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));

	return 0;

out_free_eof_irq:
	devm_free_irq(dev->dev, priv->eof_irq, priv);
out_free_nfb4eof_irq:
	devm_free_irq(dev->dev, priv->nfb4eof_irq, priv);
out_put_ipu:
	imx_smfc_put_ipu_resources(priv);
	for (i = 0; i < 2; i++) {
		frame = priv->active_frame[i];
		vb2_buffer_done(&frame->vb, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static int imx_smfc_stop(struct imx_smfc_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_buffer *frame;
	unsigned long flags;
	int i, ret;

	/* mark next EOF interrupt as the last before stream off */
	spin_lock_irqsave(&dev->irqlock, flags);
	priv->last_eof = true;
	spin_unlock_irqrestore(&dev->irqlock, flags);

	/*
	 * and then wait for interrupt handler to mark completion.
	 */
	ret = wait_for_completion_timeout(&priv->last_eof_comp,
					  msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));
	if (ret == 0)
		v4l2_warn(&priv->sd, "wait last encode EOF timeout\n");

	/* sensor stream off */
	ret = dev->sensor_set_stream(dev, 0);
	if (ret)
		v4l2_warn(&priv->sd, "sensor stream off failed\n");

	devm_free_irq(dev->dev, priv->eof_irq, priv);
	devm_free_irq(dev->dev, priv->nfb4eof_irq, priv);

	ipu_idmac_disable_channel(priv->smfc_ch);

	ipu_smfc_disable(priv->smfc);

	imx_smfc_free_dma_buf(priv, &priv->underrun_buf);

	imx_smfc_put_ipu_resources(priv);

	/* cancel the EOF timeout timer */
	del_timer_sync(&priv->eof_timeout_timer);

	/* return any remaining active frames with error */
	for (i = 0; i < 2; i++) {
		frame = priv->active_frame[i];
		if (frame && frame->vb.state == VB2_BUF_STATE_ACTIVE) {
			frame->vb.timestamp = ktime_get_ns();
			vb2_buffer_done(&frame->vb, VB2_BUF_STATE_ERROR);
		}
	}

	return 0;
}

static int imx_smfc_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx_smfc_priv *priv = v4l2_get_subdevdata(sd);

	return enable ? imx_smfc_start(priv) : imx_smfc_stop(priv);
}

static struct v4l2_subdev_video_ops imx_smfc_video_ops = {
	.s_stream = imx_smfc_s_stream,
};

static struct v4l2_subdev_ops imx_smfc_subdev_ops = {
	.video = &imx_smfc_video_ops,
};

struct v4l2_subdev *imxcam_smfc_init(struct imxcam_dev *dev)
{
	struct imx_smfc_priv *priv;

	priv = devm_kzalloc(dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	init_timer(&priv->eof_timeout_timer);
	priv->eof_timeout_timer.data = (unsigned long)priv;
	priv->eof_timeout_timer.function = imx_smfc_eof_timeout;

	v4l2_subdev_init(&priv->sd, &imx_smfc_subdev_ops);
	strlcpy(priv->sd.name, "imx-camera-smfc", sizeof(priv->sd.name));
	v4l2_set_subdevdata(&priv->sd, priv);

	priv->dev = dev;
	return &priv->sd;
}
