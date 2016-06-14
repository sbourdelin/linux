/*
 * V4L2 Capture Encoder Subdev for Freescale i.MX5/6 SOC
 *
 * This subdevice handles capture of video frames from the CSI, which
 * routed directly to the Image Converter preprocess encode task, for
 * resizing, colorspace conversion, and rotation.
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

struct prpenc_priv {
	struct imxcam_dev    *dev;
	struct v4l2_subdev    sd;

	struct ipu_soc       *ipu;
	struct ipuv3_channel *enc_ch;
	struct ipuv3_channel *enc_rot_in_ch;
	struct ipuv3_channel *enc_rot_out_ch;
	struct ipu_ic *ic_enc;
	struct ipu_smfc *smfc;

	struct v4l2_mbus_framefmt inf; /* input sensor format */
	struct v4l2_pix_format outf;   /* output user format */
	enum ipu_color_space in_cs;    /* input colorspace */
	enum ipu_color_space out_cs;   /* output colorspace */

	/* active (undergoing DMA) buffers, one for each IPU buffer */
	struct imxcam_buffer *active_frame[2];

	struct imxcam_dma_buf rot_buf[2];
	struct imxcam_dma_buf underrun_buf;
	int buf_num;

	struct timer_list eof_timeout_timer;
	int eof_irq;
	int nfb4eof_irq;

	bool last_eof;  /* waiting for last EOF at stream off */
	struct completion last_eof_comp;
};

static void prpenc_put_ipu_resources(struct prpenc_priv *priv)
{
	if (!IS_ERR_OR_NULL(priv->ic_enc))
		ipu_ic_put(priv->ic_enc);
	priv->ic_enc = NULL;

	if (!IS_ERR_OR_NULL(priv->enc_ch))
		ipu_idmac_put(priv->enc_ch);
	priv->enc_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->enc_rot_in_ch))
		ipu_idmac_put(priv->enc_rot_in_ch);
	priv->enc_rot_in_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->enc_rot_out_ch))
		ipu_idmac_put(priv->enc_rot_out_ch);
	priv->enc_rot_out_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->smfc))
		ipu_smfc_put(priv->smfc);
	priv->smfc = NULL;
}

static int prpenc_get_ipu_resources(struct prpenc_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct v4l2_subdev *csi_sd = dev->sensor->csi_sd;
	int ret;

	priv->ipu = dev_get_drvdata(csi_sd->dev->parent);

	priv->ic_enc = ipu_ic_get(priv->ipu, IC_TASK_ENCODER);
	if (IS_ERR(priv->ic_enc)) {
		v4l2_err(&priv->sd, "failed to get IC ENC\n");
		ret = PTR_ERR(priv->ic_enc);
		goto out;
	}

	priv->enc_ch = ipu_idmac_get(priv->ipu,
				     IPUV3_CHANNEL_IC_PRP_ENC_MEM);
	if (IS_ERR(priv->enc_ch)) {
		v4l2_err(&priv->sd, "could not get IDMAC channel %u\n",
			 IPUV3_CHANNEL_IC_PRP_ENC_MEM);
		ret = PTR_ERR(priv->enc_ch);
		goto out;
	}

	priv->enc_rot_in_ch = ipu_idmac_get(priv->ipu,
					    IPUV3_CHANNEL_MEM_ROT_ENC);
	if (IS_ERR(priv->enc_rot_in_ch)) {
		v4l2_err(&priv->sd, "could not get IDMAC channel %u\n",
			 IPUV3_CHANNEL_MEM_ROT_ENC);
		ret = PTR_ERR(priv->enc_rot_in_ch);
		goto out;
	}

	priv->enc_rot_out_ch = ipu_idmac_get(priv->ipu,
					     IPUV3_CHANNEL_ROT_ENC_MEM);
	if (IS_ERR(priv->enc_rot_out_ch)) {
		v4l2_err(&priv->sd, "could not get IDMAC channel %u\n",
			 IPUV3_CHANNEL_ROT_ENC_MEM);
		ret = PTR_ERR(priv->enc_rot_out_ch);
		goto out;
	}

	return 0;
out:
	prpenc_put_ipu_resources(priv);
	return ret;
}

static irqreturn_t prpenc_eof_interrupt(int irq, void *dev_id)
{
	struct prpenc_priv *priv = dev_id;
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	struct imxcam_buffer *frame;
	struct ipuv3_channel *channel;
	enum vb2_buffer_state state;
	struct timeval cur_timeval;
	u64 cur_time_ns;
	unsigned long flags;
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

	channel = (ipu_rot_mode_is_irt(dev->rot_mode)) ?
		priv->enc_rot_out_ch : priv->enc_ch;

	if (ipu_idmac_buffer_is_ready(channel, priv->buf_num))
		ipu_idmac_clear_buffer(channel, priv->buf_num);

	ipu_cpmem_set_buffer(channel, priv->buf_num, phys);
	ipu_idmac_select_buffer(channel, priv->buf_num);

	priv->buf_num ^= 1;

unlock:
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return IRQ_HANDLED;
}

static irqreturn_t prpenc_nfb4eof_interrupt(int irq, void *dev_id)
{
	struct prpenc_priv *priv = dev_id;

	v4l2_err(&priv->sd, "NFB4EOF\n");

	/*
	 * It has been discovered that with rotation, stream off
	 * creates a single NFB4EOF event which is 100% repeatable. So
	 * scheduling a restart here causes an endless NFB4EOF-->restart
	 * cycle. The error itself seems innocuous, capture is not adversely
	 * affected.
	 *
	 * So don't schedule a restart on NFB4EOF error. If the source
	 * of the NFB4EOF event on disable is ever found, it can
	 * be re-enabled, but is probably not necessary. Detecting the
	 * interrupt (and clearing the irq status in the IPU) seems to
	 * be enough.
	 */

	return IRQ_HANDLED;
}

/*
 * EOF timeout timer function.
 */
static void prpenc_eof_timeout(unsigned long data)
{
	struct prpenc_priv *priv = (struct prpenc_priv *)data;

	v4l2_err(&priv->sd, "EOF timeout\n");

	v4l2_subdev_notify(&priv->sd, IMXCAM_EOF_TIMEOUT_NOTIFY, NULL);
}

static void prpenc_free_dma_buf(struct prpenc_priv *priv,
				 struct imxcam_dma_buf *buf)
{
	struct imxcam_dev *dev = priv->dev;

	if (buf->virt)
		dma_free_coherent(dev->dev, buf->len, buf->virt, buf->phys);

	buf->virt = NULL;
	buf->phys = 0;
}

static int prpenc_alloc_dma_buf(struct prpenc_priv *priv,
				 struct imxcam_dma_buf *buf,
				 int size)
{
	struct imxcam_dev *dev = priv->dev;

	prpenc_free_dma_buf(priv, buf);

	buf->len = PAGE_ALIGN(size);
	buf->virt = dma_alloc_coherent(dev->dev, buf->len, &buf->phys,
				       GFP_DMA | GFP_KERNEL);
	if (!buf->virt) {
		v4l2_err(&priv->sd, "failed to alloc dma buffer\n");
		return -ENOMEM;
	}

	return 0;
}

static void prpenc_setup_channel(struct prpenc_priv *priv,
				  struct ipuv3_channel *channel,
				  enum ipu_rotate_mode rot_mode,
				  dma_addr_t addr0, dma_addr_t addr1,
				  bool rot_swap_width_height)
{
	struct imxcam_dev *dev = priv->dev;
	u32 width, height, stride;
	unsigned int burst_size;
	struct ipu_image image;

	if (rot_swap_width_height) {
		width = priv->outf.height;
		height = priv->outf.width;
	} else {
		width = priv->outf.width;
		height = priv->outf.height;
	}

	stride = dev->user_pixfmt->y_depth ?
		(width * dev->user_pixfmt->y_depth) >> 3 :
		(width * dev->user_pixfmt->bpp) >> 3;

	ipu_cpmem_zero(channel);

	memset(&image, 0, sizeof(image));
	image.pix.width = image.rect.width = width;
	image.pix.height = image.rect.height = height;
	image.pix.bytesperline = stride;
	image.pix.pixelformat = priv->outf.pixelformat;
	image.phys0 = addr0;
	image.phys1 = addr1;
	ipu_cpmem_set_image(channel, &image);

	if (channel == priv->enc_rot_in_ch ||
	    channel == priv->enc_rot_out_ch) {
		burst_size = 8;
		ipu_cpmem_set_block_mode(channel);
	} else {
		burst_size = (width & 0xf) ? 8 : 16;
	}

	ipu_cpmem_set_burstsize(channel, burst_size);

	if (rot_mode)
		ipu_cpmem_set_rotation(channel, rot_mode);

	if (V4L2_FIELD_HAS_BOTH(priv->inf.field) && channel == priv->enc_ch)
		ipu_cpmem_interlaced_scan(channel, stride);

	ipu_ic_task_idma_init(priv->ic_enc, channel, width, height,
			      burst_size, rot_mode);
	ipu_cpmem_set_axi_id(channel, 1);

	ipu_idmac_set_double_buffer(channel, true);
}

static int prpenc_setup_rotation(struct prpenc_priv *priv,
				  dma_addr_t phys0, dma_addr_t phys1)
{
	struct imxcam_dev *dev = priv->dev;
	int ret;

	ret = prpenc_alloc_dma_buf(priv, &priv->underrun_buf,
				    priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc underrun_buf, %d\n", ret);
		return ret;
	}

	ret = prpenc_alloc_dma_buf(priv, &priv->rot_buf[0],
				    priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc rot_buf[0], %d\n", ret);
		goto free_underrun;
	}
	ret = prpenc_alloc_dma_buf(priv, &priv->rot_buf[1],
				    priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc rot_buf[1], %d\n", ret);
		goto free_rot0;
	}

	ret = ipu_ic_task_init(priv->ic_enc,
			       priv->inf.width, priv->inf.height,
			       priv->outf.height, priv->outf.width,
			       priv->in_cs, priv->out_cs);
	if (ret) {
		v4l2_err(&priv->sd, "ipu_ic_task_init failed, %d\n", ret);
		goto free_rot1;
	}

	/* init the IC ENC-->MEM IDMAC channel */
	prpenc_setup_channel(priv, priv->enc_ch,
			      IPU_ROTATE_NONE,
			      priv->rot_buf[0].phys,
			      priv->rot_buf[1].phys,
			      true);

	/* init the MEM-->IC ENC ROT IDMAC channel */
	prpenc_setup_channel(priv, priv->enc_rot_in_ch,
			      dev->rot_mode,
			      priv->rot_buf[0].phys,
			      priv->rot_buf[1].phys,
			      true);

	/* init the destination IC ENC ROT-->MEM IDMAC channel */
	prpenc_setup_channel(priv, priv->enc_rot_out_ch,
			      IPU_ROTATE_NONE,
			      phys0, phys1,
			      false);

	/* now link IC ENC-->MEM to MEM-->IC ENC ROT */
	ipu_idmac_link(priv->enc_ch, priv->enc_rot_in_ch);

	/* enable the IC */
	ipu_ic_enable(priv->ic_enc);

	/* set buffers ready */
	ipu_idmac_select_buffer(priv->enc_ch, 0);
	ipu_idmac_select_buffer(priv->enc_ch, 1);
	ipu_idmac_select_buffer(priv->enc_rot_out_ch, 0);
	ipu_idmac_select_buffer(priv->enc_rot_out_ch, 1);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->enc_ch);
	ipu_idmac_enable_channel(priv->enc_rot_in_ch);
	ipu_idmac_enable_channel(priv->enc_rot_out_ch);

	/* and finally enable the IC PRPENC task */
	ipu_ic_task_enable(priv->ic_enc);

	return 0;

free_rot1:
	prpenc_free_dma_buf(priv, &priv->rot_buf[1]);
free_rot0:
	prpenc_free_dma_buf(priv, &priv->rot_buf[0]);
free_underrun:
	prpenc_free_dma_buf(priv, &priv->underrun_buf);
	return ret;
}

static int prpenc_setup_norotation(struct prpenc_priv *priv,
				    dma_addr_t phys0, dma_addr_t phys1)
{
	struct imxcam_dev *dev = priv->dev;
	int ret;

	ret = prpenc_alloc_dma_buf(priv, &priv->underrun_buf,
				    priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc underrun_buf, %d\n", ret);
		return ret;
	}

	ret = ipu_ic_task_init(priv->ic_enc,
			       priv->inf.width, priv->inf.height,
			       priv->outf.width, priv->outf.height,
			       priv->in_cs, priv->out_cs);
	if (ret) {
		v4l2_err(&priv->sd, "ipu_ic_task_init failed, %d\n", ret);
		goto free_underrun;
	}

	/* init the IC PRP-->MEM IDMAC channel */
	prpenc_setup_channel(priv, priv->enc_ch, dev->rot_mode,
			      phys0, phys1, false);

	ipu_cpmem_dump(priv->enc_ch);
	ipu_ic_dump(priv->ic_enc);
	ipu_dump(priv->ipu);

	ipu_ic_enable(priv->ic_enc);

	/* set buffers ready */
	ipu_idmac_select_buffer(priv->enc_ch, 0);
	ipu_idmac_select_buffer(priv->enc_ch, 1);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->enc_ch);

	/* enable the IC ENCODE task */
	ipu_ic_task_enable(priv->ic_enc);

	return 0;

free_underrun:
	prpenc_free_dma_buf(priv, &priv->underrun_buf);
	return ret;
}

static int prpenc_start(struct prpenc_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	int csi_id = dev->sensor->csi_ep.base.port;
	struct imxcam_buffer *frame, *tmp;
	dma_addr_t phys[2] = {0};
	int i = 0, ret;

	ret = prpenc_get_ipu_resources(priv);
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
	priv->in_cs = ipu_mbus_code_to_colorspace(priv->inf.code);

	priv->outf = dev->user_fmt.fmt.pix;
	priv->out_cs = ipu_pixelformat_to_colorspace(priv->outf.pixelformat);

	priv->buf_num = 0;

	/* init EOF completion waitq */
	init_completion(&priv->last_eof_comp);
	priv->last_eof = false;

	/* set IC to receive from CSI */
	ipu_ic_set_src(priv->ic_enc, csi_id, false);

	if (ipu_rot_mode_is_irt(dev->rot_mode))
		ret = prpenc_setup_rotation(priv, phys[0], phys[1]);
	else
		ret = prpenc_setup_norotation(priv, phys[0], phys[1]);
	if (ret)
		goto out_put_ipu;

	priv->nfb4eof_irq = ipu_idmac_channel_irq(priv->ipu,
						 priv->enc_ch,
						 IPU_IRQ_NFB4EOF);
	ret = devm_request_irq(dev->dev, priv->nfb4eof_irq,
			       prpenc_nfb4eof_interrupt, 0,
			       "imxcam-enc-nfb4eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering encode NFB4EOF irq: %d\n", ret);
		goto out_put_ipu;
	}

	if (ipu_rot_mode_is_irt(dev->rot_mode))
		priv->eof_irq = ipu_idmac_channel_irq(
			priv->ipu, priv->enc_rot_out_ch, IPU_IRQ_EOF);
	else
		priv->eof_irq = ipu_idmac_channel_irq(
			priv->ipu, priv->enc_ch, IPU_IRQ_EOF);

	ret = devm_request_irq(dev->dev, priv->eof_irq,
			       prpenc_eof_interrupt, 0,
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
	prpenc_put_ipu_resources(priv);
	for (i = 0; i < 2; i++) {
		frame = priv->active_frame[i];
		vb2_buffer_done(&frame->vb, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static int prpenc_stop(struct prpenc_priv *priv)
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

	/* disable IC tasks and the channels */
	ipu_ic_task_disable(priv->ic_enc);

	ipu_idmac_disable_channel(priv->enc_ch);
	if (ipu_rot_mode_is_irt(dev->rot_mode)) {
		ipu_idmac_disable_channel(priv->enc_rot_in_ch);
		ipu_idmac_disable_channel(priv->enc_rot_out_ch);
	}

	if (ipu_rot_mode_is_irt(dev->rot_mode))
		ipu_idmac_unlink(priv->enc_ch, priv->enc_rot_in_ch);

	ipu_ic_disable(priv->ic_enc);

	prpenc_free_dma_buf(priv, &priv->rot_buf[0]);
	prpenc_free_dma_buf(priv, &priv->rot_buf[1]);
	prpenc_free_dma_buf(priv, &priv->underrun_buf);

	prpenc_put_ipu_resources(priv);

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

static int prpenc_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct prpenc_priv *priv = v4l2_get_subdevdata(sd);

	return enable ? prpenc_start(priv) : prpenc_stop(priv);
}

static struct v4l2_subdev_video_ops prpenc_video_ops = {
	.s_stream = prpenc_s_stream,
};

static struct v4l2_subdev_ops prpenc_subdev_ops = {
	.video = &prpenc_video_ops,
};

struct v4l2_subdev *imxcam_ic_prpenc_init(struct imxcam_dev *dev)
{
	struct prpenc_priv *priv;

	priv = devm_kzalloc(dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	init_timer(&priv->eof_timeout_timer);
	priv->eof_timeout_timer.data = (unsigned long)priv;
	priv->eof_timeout_timer.function = prpenc_eof_timeout;

	v4l2_subdev_init(&priv->sd, &prpenc_subdev_ops);
	strlcpy(priv->sd.name, "imx-camera-prpenc", sizeof(priv->sd.name));
	v4l2_set_subdevdata(&priv->sd, priv);

	priv->dev = dev;
	return &priv->sd;
}
