/*
 * V4L2 Capture Deinterlacer Subdev for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2014-2016 Mentor Graphics Inc.
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

/*
 * This subdev implements two different video pipelines:
 *
 * CSI -> VDIC -> IC -> CH21 -> MEM
 *
 * In this pipeline, the CSI sends a single interlaced field F(n-1)
 * directly to the VDIC (and optionally the following field F(n)
 * can be sent to memory via IDMAC channel 13). So only two fields
 * can be processed by the VDIC. This pipeline only works in VDIC's
 * high motion mode, which only requires a single field for processing.
 * The other motion modes (low and medium) require three fields, so this
 * pipeline does not work in those modes. Also, it is not clear how this
 * pipeline can deal with the various field orders (sequential BT/TB,
 * interlaced BT/TB) and there are reported image quality issues output
 * from the VDIC in this pipeline.
 *
 * CSI -> CH[0-3] -> MEM -> CH8,9,10 -> VDIC -> IC -> CH21 -> MEM
 *
 * In this pipeline, the CSI sends raw and full frames to memory buffers
 * via the IDMAC SMFC channels 0-3. Fields from these frames are then
 * transferred to the VDIC via IDMAC channels 8,9,10. The VDIC requires
 * three fields: previous field F(n-1), current field F(n), and next
 * field F(n+1), so we need three raw frames in memory: two completed frames
 * to send F(n-1), F(n), F(n+1) to the VDIC, and a third frame for active
 * CSI capture while the completed fields are sent through the VDIC->IC for
 * processing.
 *
 * While the "direct" CSI->VDIC pipeline requires less memory bus bandwidth
 * (just 1 channel vs. 5 channels for indirect pipeline), it can't be used
 * for all motion modes, it only processes a single field (so half the
 * original image resolution is lost), and it has the image quality issues
 * mentioned above. With the indirect pipeline we have full control over
 * field order. So by default the direct pipeline is disabled. Enable with
 * the module param below, if enabled it will be used by high motion mode.
 */

static int allow_direct;
module_param_named(direct, allow_direct, int, 0644);
MODULE_PARM_DESC(direct, "Allow CSI->VDIC direct pipeline (default: 0)");

struct vdic_priv;

struct vdic_pipeline_ops {
	int (*setup)(struct vdic_priv *priv);
	void (*start)(struct vdic_priv *priv);
	void (*stop)(struct vdic_priv *priv);
	void (*disable)(struct vdic_priv *priv);
};

struct vdic_field_addr {
	dma_addr_t prev; /* F(n-1) */
	dma_addr_t curr; /* F(n) */
	dma_addr_t next; /* F(n+1) */
};

struct vdic_priv {
	struct imxcam_dev    *dev;
	struct v4l2_subdev    sd;

	/* IPU and its units we require */
	struct ipu_soc *ipu;
	struct ipu_ic *ic_vf;
	struct ipu_smfc *smfc;
	struct ipu_vdi *vdi;

	struct ipuv3_channel *csi_ch;      /* raw CSI frames channel */
	struct ipuv3_channel *vdi_in_ch_p; /* F(n-1) transfer channel */
	struct ipuv3_channel *vdi_in_ch;   /* F(n) transfer channel */
	struct ipuv3_channel *vdi_in_ch_n; /* F(n+1) transfer channel */
	struct ipuv3_channel *prpvf_out_ch;/* final progressive frame channel */

	/* pipeline operations */
	struct vdic_pipeline_ops *ops;

	/* active (undergoing DMA) buffers */
	struct imxcam_buffer *active_frame[2];
	struct imxcam_dma_buf underrun_buf;
	int out_buf_num;

	/*
	 * Raw CSI frames for indirect pipeline, and the precalculated field
	 * addresses for each frame. The VDIC requires three fields: previous
	 * field F(n-1), current field F(n), and next field F(n+1), so we need
	 * three frames in memory: two completed frames to send F(n-1), F(n),
	 * F(n+1) to the VDIC, and a third frame for active CSI capture while
	 * the completed fields are sent through the VDIC->IC for processing.
	 */
	struct imxcam_dma_buf csi_frame[3];
	struct vdic_field_addr field[3];

	int csi_frame_num; /* csi_frame index, 0-2 */
	int csi_buf_num;   /* CSI channel double buffer index, 0-1 */

	struct v4l2_mbus_framefmt inf; /* input sensor format */
	struct v4l2_pix_format outf;   /* final output user format */
	enum ipu_color_space in_cs;    /* input colorspace */
	enum ipu_color_space out_cs;   /* output colorspace */
	u32 in_pixfmt;

	u32 in_stride;  /* input and output line strides */
	u32 out_stride;
	int field_size; /* 1/2 full image size */
	bool direct;    /* using direct CSI->VDIC->IC pipeline */

	struct timer_list eof_timeout_timer;

	int csi_eof_irq; /* CSI channel EOF IRQ */
	int nfb4eof_irq; /* CSI or PRPVF channel NFB4EOF IRQ */
	int out_eof_irq; /* PRPVF channel EOF IRQ */

	bool last_eof;  /* waiting for last EOF at vdic off */
	struct completion last_eof_comp;
};

static void vdic_put_ipu_resources(struct vdic_priv *priv)
{
	if (!IS_ERR_OR_NULL(priv->ic_vf))
		ipu_ic_put(priv->ic_vf);
	priv->ic_vf = NULL;

	if (!IS_ERR_OR_NULL(priv->csi_ch))
		ipu_idmac_put(priv->csi_ch);
	priv->csi_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->vdi_in_ch_p))
		ipu_idmac_put(priv->vdi_in_ch_p);
	priv->vdi_in_ch_p = NULL;

	if (!IS_ERR_OR_NULL(priv->vdi_in_ch))
		ipu_idmac_put(priv->vdi_in_ch);
	priv->vdi_in_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->vdi_in_ch_n))
		ipu_idmac_put(priv->vdi_in_ch_n);
	priv->vdi_in_ch_n = NULL;

	if (!IS_ERR_OR_NULL(priv->prpvf_out_ch))
		ipu_idmac_put(priv->prpvf_out_ch);
	priv->prpvf_out_ch = NULL;

	if (!IS_ERR_OR_NULL(priv->vdi))
		ipu_vdi_put(priv->vdi);
	priv->vdi = NULL;

	if (!IS_ERR_OR_NULL(priv->smfc))
		ipu_smfc_put(priv->smfc);
	priv->smfc = NULL;
}

static int vdic_get_ipu_resources(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	int csi_id = dev->sensor->csi_ep.base.port;
	struct v4l2_subdev *csi_sd = dev->sensor->csi_sd;
	int ret, err_chan;

	priv->ipu = dev_get_drvdata(csi_sd->dev->parent);

	priv->ic_vf = ipu_ic_get(priv->ipu, IC_TASK_VIEWFINDER);
	if (IS_ERR(priv->ic_vf)) {
		v4l2_err(&priv->sd, "failed to get IC VF\n");
		ret = PTR_ERR(priv->ic_vf);
		goto out;
	}

	priv->vdi = ipu_vdi_get(priv->ipu);
	if (IS_ERR(priv->vdi)) {
		v4l2_err(&priv->sd, "failed to get VDIC\n");
		ret = PTR_ERR(priv->vdi);
		goto out;
	}

	priv->prpvf_out_ch = ipu_idmac_get(priv->ipu,
					   IPUV3_CHANNEL_IC_PRP_VF_MEM);
	if (IS_ERR(priv->prpvf_out_ch)) {
		err_chan = IPUV3_CHANNEL_IC_PRP_VF_MEM;
		ret = PTR_ERR(priv->prpvf_out_ch);
		goto out_err_chan;
	}

	if (!priv->direct) {
		/*
		 * Choose the CSI-->SMFC-->MEM channel corresponding
		 * to the IPU and CSI IDs.
		 */
		int csi_ch_num = IPUV3_CHANNEL_CSI0 +
			(ipu_get_num(priv->ipu) << 1) + csi_id;

		priv->csi_ch = ipu_idmac_get(priv->ipu, csi_ch_num);
		if (IS_ERR(priv->csi_ch)) {
			err_chan = csi_ch_num;
			ret = PTR_ERR(priv->csi_ch);
			goto out_err_chan;
		}

		priv->smfc = ipu_smfc_get(priv->ipu, csi_ch_num);
		if (IS_ERR(priv->smfc)) {
			v4l2_err(&priv->sd, "failed to get SMFC\n");
			ret = PTR_ERR(priv->smfc);
			goto out;
		}

		priv->vdi_in_ch_p = ipu_idmac_get(priv->ipu,
						  IPUV3_CHANNEL_MEM_VDI_P);
		if (IS_ERR(priv->vdi_in_ch_p)) {
			err_chan = IPUV3_CHANNEL_MEM_VDI_P;
			ret = PTR_ERR(priv->vdi_in_ch_p);
			goto out_err_chan;
		}

		priv->vdi_in_ch = ipu_idmac_get(priv->ipu,
						IPUV3_CHANNEL_MEM_VDI);
		if (IS_ERR(priv->vdi_in_ch)) {
			err_chan = IPUV3_CHANNEL_MEM_VDI;
			ret = PTR_ERR(priv->vdi_in_ch);
			goto out_err_chan;
		}

		priv->vdi_in_ch_n = ipu_idmac_get(priv->ipu,
						  IPUV3_CHANNEL_MEM_VDI_N);
		if (IS_ERR(priv->vdi_in_ch_n)) {
			err_chan = IPUV3_CHANNEL_MEM_VDI_N;
			ret = PTR_ERR(priv->vdi_in_ch_n);
			goto out_err_chan;
		}
	}

	return 0;

out_err_chan:
	v4l2_err(&priv->sd, "could not get IDMAC channel %u\n", err_chan);
out:
	vdic_put_ipu_resources(priv);
	return ret;
}

static void prepare_csi_buffer(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	int next_frame, curr_frame;

	curr_frame = priv->csi_frame_num;
	next_frame = (curr_frame + 2) % 3;

	dev_dbg(dev->dev, "%d - %d %d\n",
		priv->csi_buf_num, curr_frame, next_frame);

	ipu_cpmem_set_buffer(priv->csi_ch, priv->csi_buf_num,
			     priv->csi_frame[next_frame].phys);
	ipu_idmac_select_buffer(priv->csi_ch, priv->csi_buf_num);
}

static void prepare_vdi_in_buffers(struct vdic_priv *priv)
{
	int last_frame, curr_frame;

	curr_frame = priv->csi_frame_num;
	last_frame = curr_frame - 1;
	if (last_frame < 0)
		last_frame = 2;

	ipu_cpmem_set_buffer(priv->vdi_in_ch_p, 0,
			     priv->field[last_frame].prev);
	ipu_cpmem_set_buffer(priv->vdi_in_ch,   0,
			     priv->field[curr_frame].curr);
	ipu_cpmem_set_buffer(priv->vdi_in_ch_n, 0,
			     priv->field[curr_frame].next);

	ipu_idmac_select_buffer(priv->vdi_in_ch_p, 0);
	ipu_idmac_select_buffer(priv->vdi_in_ch, 0);
	ipu_idmac_select_buffer(priv->vdi_in_ch_n, 0);
}

static void prepare_prpvf_out_buffer(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	struct imxcam_buffer *frame;
	dma_addr_t phys;

	if (!list_empty(&ctx->ready_q)) {
		frame = list_entry(ctx->ready_q.next,
				   struct imxcam_buffer, list);
		phys = vb2_dma_contig_plane_dma_addr(&frame->vb, 0);
		list_del(&frame->list);
		priv->active_frame[priv->out_buf_num] = frame;
	} else {
		phys = priv->underrun_buf.phys;
		priv->active_frame[priv->out_buf_num] = NULL;
	}

	ipu_cpmem_set_buffer(priv->prpvf_out_ch, priv->out_buf_num, phys);
	ipu_idmac_select_buffer(priv->prpvf_out_ch, priv->out_buf_num);
}

/* prpvf_out_ch EOF interrupt (progressive frame ready) */
static irqreturn_t prpvf_out_eof_interrupt(int irq, void *dev_id)
{
	struct vdic_priv *priv = dev_id;
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_buffer *frame;
	enum vb2_buffer_state state;
	struct timeval cur_timeval;
	u64 cur_time_ns;
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);

	cur_time_ns = ktime_get_ns();
	cur_timeval = ns_to_timeval(cur_time_ns);

	/* timestamp and return the completed frame */
	frame = priv->active_frame[priv->out_buf_num];
	if (frame) {
		frame->vb.timestamp = cur_time_ns;
		state = (dev->signal_locked &&
			 !atomic_read(&dev->pending_restart)) ?
			VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
		vb2_buffer_done(&frame->vb, state);
	}

	if (!priv->direct)
		goto flip;

	if (priv->last_eof) {
		complete(&priv->last_eof_comp);
		priv->active_frame[priv->out_buf_num] = NULL;
		priv->last_eof = false;
		goto unlock;
	}

	/* bump the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));

	prepare_prpvf_out_buffer(priv);

flip:
	priv->out_buf_num ^= 1;

	if (dev->fim.eof && dev->fim.eof(dev, &cur_timeval))
		v4l2_subdev_notify(&priv->sd, IMXCAM_FRAME_INTERVAL_NOTIFY,
				   NULL);
unlock:
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return IRQ_HANDLED;
}

/* csi_ch EOF interrupt */
static irqreturn_t csi_eof_interrupt(int irq, void *dev_id)
{
	struct vdic_priv *priv = dev_id;
	struct imxcam_dev *dev = priv->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);

	if (priv->last_eof) {
		complete(&priv->last_eof_comp);
		priv->active_frame[priv->out_buf_num] = NULL;
		priv->last_eof = false;
		goto unlock;
	}

	/* bump the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));

	/* prepare next buffers */
	prepare_csi_buffer(priv);
	prepare_prpvf_out_buffer(priv);
	prepare_vdi_in_buffers(priv);

	/* increment double-buffer index and frame index */
	priv->csi_buf_num ^= 1;
	priv->csi_frame_num = (priv->csi_frame_num + 1) % 3;

unlock:
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return IRQ_HANDLED;
}

static irqreturn_t nfb4eof_interrupt(int irq, void *dev_id)
{
	struct vdic_priv *priv = dev_id;

	v4l2_err(&priv->sd, "NFB4EOF\n");

	v4l2_subdev_notify(&priv->sd, IMXCAM_NFB4EOF_NOTIFY, NULL);

	return IRQ_HANDLED;
}

/*
 * EOF timeout timer function.
 */
static void vdic_eof_timeout(unsigned long data)
{
	struct vdic_priv *priv = (struct vdic_priv *)data;

	v4l2_err(&priv->sd, "EOF timeout\n");

	v4l2_subdev_notify(&priv->sd, IMXCAM_EOF_TIMEOUT_NOTIFY, NULL);
}

static void vdic_free_dma_buf(struct vdic_priv *priv,
			      struct imxcam_dma_buf *buf)
{
	struct imxcam_dev *dev = priv->dev;

	if (buf->virt)
		dma_free_coherent(dev->dev, buf->len, buf->virt, buf->phys);

	buf->virt = NULL;
	buf->phys = 0;
}

static int vdic_alloc_dma_buf(struct vdic_priv *priv,
			      struct imxcam_dma_buf *buf,
			      int size)
{
	struct imxcam_dev *dev = priv->dev;

	vdic_free_dma_buf(priv, buf);

	buf->len = PAGE_ALIGN(size);
	buf->virt = dma_alloc_coherent(dev->dev, buf->len, &buf->phys,
				       GFP_DMA | GFP_KERNEL);
	if (!buf->virt) {
		v4l2_err(&priv->sd, "failed to alloc dma buffer\n");
		return -ENOMEM;
	}

	return 0;
}

static void setup_csi_channel(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_sensor *sensor = dev->sensor;
	struct ipuv3_channel *channel = priv->csi_ch;
	struct v4l2_mbus_framefmt *inf = &priv->inf;
	int csi_id = sensor->csi_ep.base.port;
	int vc_num = sensor->csi_ep.base.id;
	unsigned int burst_size;
	struct ipu_image image;
	bool passthrough;

	ipu_cpmem_zero(channel);

	memset(&image, 0, sizeof(image));
	image.pix.width = image.rect.width = inf->width;
	image.pix.height = image.rect.height = inf->height;
	image.pix.bytesperline = priv->in_stride;
	image.pix.pixelformat = priv->in_pixfmt;
	image.phys0 = priv->csi_frame[0].phys;
	image.phys1 = priv->csi_frame[1].phys;
	ipu_cpmem_set_image(channel, &image);

	burst_size = (inf->width & 0xf) ? 8 : 16;

	ipu_cpmem_set_burstsize(channel, burst_size);

	/*
	 * If the sensor uses 16-bit parallel CSI bus, we must handle
	 * the data internally in the IPU as 16-bit generic, aka
	 * passthrough mode.
	 */
	passthrough = (sensor->ep.bus_type != V4L2_MBUS_CSI2 &&
		       sensor->ep.bus.parallel.bus_width >= 16);

	if (passthrough)
		ipu_cpmem_set_format_passthrough(channel, 16);

	if (sensor->ep.bus_type == V4L2_MBUS_CSI2)
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
	ipu_cpmem_set_high_priority(channel);
	ipu_idmac_enable_watermark(channel, true);
	ipu_cpmem_set_axi_id(channel, 0);
	ipu_idmac_lock_enable(channel, 8);

	burst_size = ipu_cpmem_get_burstsize(channel);
	burst_size = passthrough ?
		(burst_size >> 3) - 1 : (burst_size >> 2) - 1;

	ipu_smfc_set_burstsize(priv->smfc, burst_size);

	ipu_idmac_set_double_buffer(channel, true);
}

static void setup_vdi_channel(struct vdic_priv *priv,
			      struct ipuv3_channel *channel,
			      dma_addr_t phys0, dma_addr_t phys1,
			      bool out_chan)
{
	u32 stride, width, height, pixfmt;
	unsigned int burst_size;
	struct ipu_image image;

	if (out_chan) {
		width = priv->outf.width;
		height = priv->outf.height;
		pixfmt = priv->outf.pixelformat;
		stride = priv->out_stride;
	} else {
		width = priv->inf.width;
		height = priv->inf.height / 2;
		pixfmt = priv->in_pixfmt;
		stride = priv->in_stride;
	}

	ipu_cpmem_zero(channel);

	memset(&image, 0, sizeof(image));
	image.pix.width = image.rect.width = width;
	image.pix.height = image.rect.height = height;
	image.pix.bytesperline = stride;
	image.pix.pixelformat = pixfmt;
	image.phys0 = phys0;
	image.phys1 = phys1;
	ipu_cpmem_set_image(channel, &image);

	burst_size = (width & 0xf) ? 8 : 16;

	ipu_cpmem_set_burstsize(channel, burst_size);

	if (out_chan)
		ipu_ic_task_idma_init(priv->ic_vf, channel, width, height,
				      burst_size, IPU_ROTATE_NONE);

	ipu_cpmem_set_axi_id(channel, 1);

	ipu_idmac_set_double_buffer(channel, out_chan);
}

static int vdic_setup_direct(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_ctx *ctx = dev->io_ctx;
	struct imxcam_buffer *frame, *tmp;
	dma_addr_t phys[2] = {0};
	int i = 0;

	priv->out_buf_num = 0;

	list_for_each_entry_safe(frame, tmp, &ctx->ready_q, list) {
		phys[i] = vb2_dma_contig_plane_dma_addr(&frame->vb, 0);
		list_del(&frame->list);
		priv->active_frame[i++] = frame;
		if (i >= 2)
			break;
	}

	/* init the prpvf out channel */
	setup_vdi_channel(priv, priv->prpvf_out_ch, phys[0], phys[1], true);

	return 0;
}

static void vdic_start_direct(struct vdic_priv *priv)
{
	/* set buffers ready */
	ipu_idmac_select_buffer(priv->prpvf_out_ch, 0);
	ipu_idmac_select_buffer(priv->prpvf_out_ch, 1);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->prpvf_out_ch);
}

static void vdic_stop_direct(struct vdic_priv *priv)
{
	ipu_idmac_disable_channel(priv->prpvf_out_ch);
}

static void vdic_disable_direct(struct vdic_priv *priv)
{
	/* nothing to do */
}

static int vdic_setup_indirect(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct vdic_field_addr *field;
	struct imxcam_dma_buf *frame;
	int ret, in_size, i;

	/*
	 * FIXME: following in_size calc would not be correct for planar pixel
	 * formats, but all mbus pixel codes are packed formats, so so far this
	 * is OK.
	 */
	in_size = priv->in_stride * priv->inf.height;

	priv->csi_buf_num = priv->csi_frame_num = priv->out_buf_num = 0;
	priv->field_size = in_size / 2;

	/* request EOF irq for vdi out channel */
	priv->csi_eof_irq = ipu_idmac_channel_irq(priv->ipu,
						  priv->csi_ch,
						  IPU_IRQ_EOF);
	ret = devm_request_irq(dev->dev, priv->csi_eof_irq,
			       csi_eof_interrupt, 0,
			       "imxcam-csi-eof", priv);
	if (ret) {
		v4l2_err(&priv->sd, "Error registering CSI eof irq: %d\n",
			 ret);
		return ret;
	}

	for (i = 0; i < 3; i++) {
		frame = &priv->csi_frame[i];

		ret = vdic_alloc_dma_buf(priv, frame, in_size);
		if (ret) {
			v4l2_err(&priv->sd,
				 "failed to alloc csi_frame[%d], %d\n", i, ret);
			while (--i >= 0)
				vdic_free_dma_buf(priv, &priv->csi_frame[i]);
			goto out_free_irq;
		}

		/* precalculate the field addresses for this frame */
		field = &priv->field[i];
		switch (priv->inf.field) {
		case V4L2_FIELD_SEQ_TB:
			field->prev = frame->phys + priv->field_size;
			field->curr = frame->phys;
			field->next = frame->phys + priv->field_size;
			break;
		case V4L2_FIELD_SEQ_BT:
			field->prev = frame->phys;
			field->curr = frame->phys + priv->field_size;
			field->next = frame->phys;
			break;
		case V4L2_FIELD_INTERLACED_BT:
			field->prev = frame->phys;
			field->curr = frame->phys + priv->in_stride;
			field->next = frame->phys;
			break;
		default:
			/* assume V4L2_FIELD_INTERLACED_TB */
			field->prev = frame->phys + priv->in_stride;
			field->curr = frame->phys;
			field->next = frame->phys + priv->in_stride;
			break;
		}
	}

	priv->active_frame[0] = priv->active_frame[1] = NULL;

	/* init the CSI channel */
	setup_csi_channel(priv);

	/* init the vdi-in channels */
	setup_vdi_channel(priv, priv->vdi_in_ch_p, 0, 0, false);
	setup_vdi_channel(priv, priv->vdi_in_ch, 0, 0, false);
	setup_vdi_channel(priv, priv->vdi_in_ch_n, 0, 0, false);

	/* init the prpvf out channel */
	setup_vdi_channel(priv, priv->prpvf_out_ch, 0, 0, true);

	return 0;

out_free_irq:
	devm_free_irq(dev->dev, priv->csi_eof_irq, priv);
	return ret;
}

static void vdic_start_indirect(struct vdic_priv *priv)
{
	int i;

	/* set buffers ready */
	for (i = 0; i < 2; i++)
		ipu_idmac_select_buffer(priv->csi_ch, i);

	/* enable SMFC */
	ipu_smfc_enable(priv->smfc);

	/* enable the channels */
	ipu_idmac_enable_channel(priv->csi_ch);
	ipu_idmac_enable_channel(priv->prpvf_out_ch);
	ipu_idmac_enable_channel(priv->vdi_in_ch_p);
	ipu_idmac_enable_channel(priv->vdi_in_ch);
	ipu_idmac_enable_channel(priv->vdi_in_ch_n);
}

static void vdic_stop_indirect(struct vdic_priv *priv)
{
	/* disable channels */
	ipu_idmac_disable_channel(priv->prpvf_out_ch);
	ipu_idmac_disable_channel(priv->vdi_in_ch_p);
	ipu_idmac_disable_channel(priv->vdi_in_ch);
	ipu_idmac_disable_channel(priv->vdi_in_ch_n);
	ipu_idmac_disable_channel(priv->csi_ch);

	/* disable SMFC */
	ipu_smfc_disable(priv->smfc);
}

static void vdic_disable_indirect(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	int i;

	devm_free_irq(dev->dev, priv->csi_eof_irq, priv);

	for (i = 0; i < 3; i++)
		vdic_free_dma_buf(priv, &priv->csi_frame[i]);
}

static struct vdic_pipeline_ops direct_ops = {
	.setup = vdic_setup_direct,
	.start = vdic_start_direct,
	.stop = vdic_stop_direct,
	.disable = vdic_disable_direct,
};

static struct vdic_pipeline_ops indirect_ops = {
	.setup = vdic_setup_indirect,
	.start = vdic_start_indirect,
	.stop = vdic_stop_indirect,
	.disable = vdic_disable_indirect,
};

static int vdic_start(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	int csi_id = dev->sensor->csi_ep.base.port;
	struct imxcam_buffer *frame;
	int i, ret;

	priv->direct = (allow_direct && dev->motion == HIGH_MOTION);
	/* this info is needed by CSI subdev for destination routing */
	dev->vdic_direct = priv->direct;

	priv->ops = priv->direct ? &direct_ops : &indirect_ops;

	ret = vdic_get_ipu_resources(priv);
	if (ret)
		return ret;

	priv->inf = dev->sensor_fmt;
	priv->in_pixfmt = dev->sensor_pixfmt->fourcc;
	priv->inf.width = dev->crop.width;
	priv->inf.height = dev->crop.height;
	priv->in_stride = dev->sensor_pixfmt->y_depth ?
		(priv->inf.width * dev->sensor_pixfmt->y_depth) >> 3 :
		(priv->inf.width * dev->sensor_pixfmt->bpp) >> 3;
	priv->in_cs = ipu_mbus_code_to_colorspace(priv->inf.code);

	priv->outf = dev->user_fmt.fmt.pix;
	priv->out_cs = ipu_pixelformat_to_colorspace(priv->outf.pixelformat);
	priv->out_stride = dev->user_pixfmt->y_depth ?
		(priv->outf.width * dev->user_pixfmt->y_depth) >> 3 :
		(priv->outf.width * dev->user_pixfmt->bpp) >> 3;

	/* set IC to receive from VDIC */
	ipu_ic_set_src(priv->ic_vf, csi_id, true);

	/*
	 * set VDIC to receive from CSI for direct path, and memory
	 * for indirect.
	 */
	ipu_vdi_set_src(priv->vdi, priv->direct);

	ret = vdic_alloc_dma_buf(priv, &priv->underrun_buf,
				 priv->outf.sizeimage);
	if (ret) {
		v4l2_err(&priv->sd, "failed to alloc underrun_buf, %d\n", ret);
		goto out_put_ipu;
	}

	/* init EOF completion waitq */
	init_completion(&priv->last_eof_comp);
	priv->last_eof = false;

	/* request EOF irq for prpvf out channel */
	priv->out_eof_irq = ipu_idmac_channel_irq(priv->ipu,
						  priv->prpvf_out_ch,
						  IPU_IRQ_EOF);
	ret = devm_request_irq(dev->dev, priv->out_eof_irq,
			       prpvf_out_eof_interrupt, 0,
			       "imxcam-prpvf-out-eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering prpvf out eof irq: %d\n", ret);
		goto out_free_underrun;
	}

	/* request NFB4EOF irq */
	priv->nfb4eof_irq = ipu_idmac_channel_irq(priv->ipu, priv->direct ?
						  priv->prpvf_out_ch :
						  priv->csi_ch,
						  IPU_IRQ_NFB4EOF);
	ret = devm_request_irq(dev->dev, priv->nfb4eof_irq,
			       nfb4eof_interrupt, 0,
			       "imxcam-vdic-nfb4eof", priv);
	if (ret) {
		v4l2_err(&priv->sd,
			 "Error registering NFB4EOF irq: %d\n", ret);
		goto out_free_eof_irq;
	}

	/* init the VDIC */
	ipu_vdi_setup(priv->vdi, priv->inf.code,
		      priv->inf.width, priv->inf.height, priv->inf.field,
		      dev->motion);

	ret = ipu_ic_task_init(priv->ic_vf,
			       priv->inf.width, priv->inf.height,
			       priv->outf.width, priv->outf.height,
			       priv->in_cs, priv->out_cs);
	if (ret) {
		v4l2_err(&priv->sd, "ipu_ic_task_init failed, %d\n", ret);
		goto out_free_nfb4eof_irq;
	}

	ret = priv->ops->setup(priv);
	if (ret)
		goto out_free_nfb4eof_irq;

	ipu_vdi_enable(priv->vdi);
	ipu_ic_enable(priv->ic_vf);

	priv->ops->start(priv);

	/* enable the IC VF task */
	ipu_ic_task_enable(priv->ic_vf);

	/* sensor stream on */
	ret = dev->sensor_set_stream(dev, 1);
	if (ret) {
		v4l2_err(&priv->sd, "sensor stream on failed\n");
		goto out_stop;
	}

	/* start the EOF timeout timer */
	mod_timer(&priv->eof_timeout_timer,
		  jiffies + msecs_to_jiffies(IMXCAM_EOF_TIMEOUT));

	return 0;

out_stop:
	ipu_ic_task_disable(priv->ic_vf);
	priv->ops->stop(priv);
	ipu_ic_disable(priv->ic_vf);
	ipu_vdi_disable(priv->vdi);
	priv->ops->disable(priv);
out_free_nfb4eof_irq:
	devm_free_irq(dev->dev, priv->nfb4eof_irq, priv);
out_free_eof_irq:
	devm_free_irq(dev->dev, priv->out_eof_irq, priv);
out_free_underrun:
	vdic_free_dma_buf(priv, &priv->underrun_buf);
out_put_ipu:
	vdic_put_ipu_resources(priv);
	for (i = 0; i < 2; i++) {
		frame = priv->active_frame[i];
		if (frame)
			vb2_buffer_done(&frame->vb, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static int vdic_stop(struct vdic_priv *priv)
{
	struct imxcam_dev *dev = priv->dev;
	struct imxcam_buffer *frame;
	unsigned long flags;
	int i, ret;

	/* mark next EOF interrupt as the last before vdic off */
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

	ipu_ic_task_disable(priv->ic_vf);
	priv->ops->stop(priv);
	ipu_ic_disable(priv->ic_vf);
	ipu_vdi_disable(priv->vdi);
	priv->ops->disable(priv);
	devm_free_irq(dev->dev, priv->nfb4eof_irq, priv);
	devm_free_irq(dev->dev, priv->out_eof_irq, priv);
	vdic_free_dma_buf(priv, &priv->underrun_buf);
	vdic_put_ipu_resources(priv);

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

static int vdic_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vdic_priv *priv = v4l2_get_subdevdata(sd);

	return enable ? vdic_start(priv) : vdic_stop(priv);
}

static struct v4l2_subdev_video_ops vdic_video_ops = {
	.s_stream = vdic_s_stream,
};

static struct v4l2_subdev_ops vdic_subdev_ops = {
	.video = &vdic_video_ops,
};

struct v4l2_subdev *imxcam_vdic_init(struct imxcam_dev *dev)
{
	struct vdic_priv *priv;

	priv = devm_kzalloc(dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	init_timer(&priv->eof_timeout_timer);
	priv->eof_timeout_timer.data = (unsigned long)priv;
	priv->eof_timeout_timer.function = vdic_eof_timeout;

	v4l2_subdev_init(&priv->sd, &vdic_subdev_ops);
	strlcpy(priv->sd.name, "imx-camera-vdic", sizeof(priv->sd.name));
	v4l2_set_subdevdata(&priv->sd, priv);

	priv->dev = dev;
	return &priv->sd;
}
