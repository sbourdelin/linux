/*
 * R-Car Gen3 Digital Radio Interface (DRIF) driver
 *
 * Copyright (C) 2016 Renesas Electronics Corporation
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

/*
 * The R-Car DRIF is a receive only MSIOF like controller with an
 * external master device driving the SCK. It receives data into a FIFO,
 * then this driver uses the SYS-DMAC engine to move the data from
 * the device to memory.
 *
 * Each DRIF channel supports two sub-channels with their own resources
 * like module clk, register set and dma.
 *
 * Depending on the master device, a DRIF channel can use
 *  (1) both sub-channels (D0 & D1 pins) to receive data in parallel (or)
 *  (2) one sub-channel (D0 or D1) to receive data
 *
 * The primary design goal of this controller is to act as Digitial Radio
 * Interface that receives digital samples from a tuner device. Hence the
 * driver exposes the device as a V4L2 SDR device. In order to qualify as
 * a V4L2 SDR device, it should possess tuner interface as mandated by the
 * framework. This driver expects a tuner driver (sub-device) to bind
 * asynchronously with this device and the combined drivers shall expose
 * a V4L2 compliant SDR device. The DRIF driver is independent of the
 * tuner vendor.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/ioctl.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

/* Internal flags */
#define RCAR_DRIF_BUF_DONE			BIT(0)
#define RCAR_DRIF_BUF_OVERFLOW			BIT(1)

#define RCAR_DRIF_BUF_PAIRS_DONE(buf1, buf2)	\
	((buf1->status & buf2->status & RCAR_DRIF_BUF_DONE) == \
	 RCAR_DRIF_BUF_DONE)

/* DRIF register offsets */
#define RCAR_DRIF_SITMDR1			(0x00)
#define RCAR_DRIF_SITMDR2			(0x04)
#define RCAR_DRIF_SITMDR3			(0x08)
#define RCAR_DRIF_SIRMDR1			(0x10)
#define RCAR_DRIF_SIRMDR2			(0x14)
#define RCAR_DRIF_SIRMDR3			(0x18)
#define RCAR_DRIF_SICTR				(0x28)
#define RCAR_DRIF_SIFCTR			(0x30)
#define RCAR_DRIF_SISTR				(0x40)
#define RCAR_DRIF_SIIER				(0x44)
#define RCAR_DRIF_SIRFDR			(0x60)

#define RCAR_DRIF_RFOVF			BIT(3)	/* Receive FIFO overflow */
#define RCAR_DRIF_RFUDF			BIT(4)	/* Receive FIFO underflow */
#define RCAR_DRIF_RFSERR		BIT(5)	/* Receive frame sync error */
#define RCAR_DRIF_REOF			BIT(7)	/* Frame reception end */
#define RCAR_DRIF_RDREQ			BIT(12) /* Receive data xfer req */
#define RCAR_DRIF_RFFUL			BIT(13)	/* Receive FIFO full */

/* Configuration */
#define RCAR_DRIF_SIRMDR1_SYNCMD_FRAME		(0 << 28)
#define RCAR_DRIF_SIRMDR1_SYNCMD_LR		(3 << 28)

#define RCAR_DRIF_SIRMDR1_SYNCAC_POL_HIGH	(0 << 25)
#define RCAR_DRIF_SIRMDR1_SYNCAC_POL_LOW	(1 << 25)

#define RCAR_DRIF_SIRMDR1_MSB_FIRST		(0 << 24)
#define RCAR_DRIF_SIRMDR1_LSB_FIRST		(1 << 24)

#define RCAR_DRIF_SIRMDR1_DTDL_0		(0 << 20)
#define RCAR_DRIF_SIRMDR1_DTDL_1		(1 << 20)
#define RCAR_DRIF_SIRMDR1_DTDL_2		(2 << 20)
#define RCAR_DRIF_SIRMDR1_DTDL_0PT5		(5 << 20)
#define RCAR_DRIF_SIRMDR1_DTDL_1PT5		(6 << 20)

#define RCAR_DRIF_SIRMDR1_SYNCDL_0		(0 << 20)
#define RCAR_DRIF_SIRMDR1_SYNCDL_1		(1 << 20)
#define RCAR_DRIF_SIRMDR1_SYNCDL_2		(2 << 20)
#define RCAR_DRIF_SIRMDR1_SYNCDL_3		(3 << 20)
#define RCAR_DRIF_SIRMDR1_SYNCDL_0PT5		(5 << 20)
#define RCAR_DRIF_SIRMDR1_SYNCDL_1PT5		(6 << 20)

#define RCAR_DRIF_MDR_GRPCNT(n)			(((n) - 1) << 30)
#define RCAR_DRIF_MDR_BITLEN(n)			(((n) - 1) << 24)
#define RCAR_DRIF_MDR_WDCNT(n)			(((n) - 1) << 16)

/* Hidden Transmit register that controls CLK & SYNC */
#define RCAR_DRIF_SITMDR1_PCON			BIT(30)

#define RCAR_DRIF_SICTR_RX_RISING_EDGE		BIT(26)
#define RCAR_DRIF_SICTR_RX_EN			BIT(8)
#define RCAR_DRIF_SICTR_RESET			BIT(0)

/* Constants */
#define RCAR_DRIF_MAX_NUM_HWBUFS		32
#define RCAR_DRIF_MAX_DEVS			4
#define RCAR_DRIF_DEFAULT_NUM_HWBUFS		16
#define RCAR_DRIF_DEFAULT_HWBUF_SIZE		(4 * PAGE_SIZE)
#define RCAR_DRIF_MAX_SUBCHANS			2
#define RCAR_SDR_BUFFER_SIZE			(64 * 1024)

/* A tuner for each sub channel */
#define RCAR_DRIF_MAX_SUBDEVS			RCAR_DRIF_MAX_SUBCHANS

/* Stream formats */
struct rcar_drif_format {
	char	*name;
	u32	pixelformat;
	u32	buffersize;
	u32	wdlen;
	u32	num_schans;
};

/* Format descriptions for capture and preview */
static const struct rcar_drif_format formats[] = {
	{
		.name		= "Sliced Complex U16BE",
		.pixelformat	= V4L2_SDR_FMT_SCU16BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 16,
		.num_schans	= 2,
	},
	{
		.name		= "Sliced Complex U18BE",
		.pixelformat	= V4L2_SDR_FMT_SCU18BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 18,
		.num_schans	= 2,
	},
	{
		.name		= "Sliced Complex U20BE",
		.pixelformat	= V4L2_SDR_FMT_SCU20BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 20,
		.num_schans	= 2,
	},
};

#define to_rcar_drif_buf_pair(p, ch, idx)		\
	(p->schan[!ch]->buf[idx])

static const unsigned int NUM_FORMATS = ARRAY_SIZE(formats);

/* Buffer for a received frame from one or both sub-channels */
struct rcar_drif_frame_buf {
	/* Common v4l buffer stuff -- must be first */
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct rcar_drif_async_subdev {
	struct v4l2_subdev *sd;
	struct v4l2_async_subdev asd;
};

/* DMA buffer */
struct rcar_drif_hwbuf {
	void *addr;			/* Cpu-side address */
	int status;			/* Buffer status flags */
};

/* Sub-channel resources */
struct rcar_drif_subchan {
	struct rcar_drif_chan *parent;	/* Parent ctx */
	void __iomem *base;		/* Base register address */
	resource_size_t start;		/* I/O resource offset */
	struct dma_chan *dmach;		/* Reserved DMA channel */
	struct clk *clkp;		/* Module clock */
	struct rcar_drif_hwbuf *buf[RCAR_DRIF_MAX_NUM_HWBUFS];
	dma_addr_t dma_handle;		/* Handle for all bufs */
	u8 num;				/* Sub-channel number */
};

/* Channel context */
struct rcar_drif_chan {
	struct device *dev;		/* Platform device */
	struct video_device vdev;	/* SDR device */
	struct v4l2_device v4l2_dev;	/* V4l2 device */

	/* Videobuf2 queue and queued buffers list */
	struct vb2_queue vb_queue;
	struct list_head queued_bufs;
	spinlock_t queued_bufs_lock;	/* Protects queued_bufs */

	struct mutex v4l2_mutex;	/* To serialize ioctls */
	struct mutex vb_queue_mutex;	/* To serialize streaming ioctls */
	struct v4l2_ctrl_handler ctrl_hdl;	/* SDR control handler */
	struct v4l2_async_notifier notifier;	/* For subdev (tuner) */
	u32 num_subdevs;		/* Num of sub devices used */

	/* Current V4L2 SDR format array index */
	u32 fmt_idx;

	/* Device tree SYNC properties */
	u32 mdr1;

	/* Internals */
	struct rcar_drif_subchan *schan[2]; /* DRIFx0 and DRIFx1 */
	unsigned long hw_schans_mask;	/* Enabled sub-channels per DT */
	unsigned long cur_schans_mask;	/* Used sub-channels for an SDR FMT */
	u32 num_hw_schans;		/* Num of DT enabled sub-channels */
	u32 num_cur_schans;		/* Num of used sub-channels */
	u32 num_hwbufs;			/* Num of DMA buffers */
	u32 hwbuf_size;			/* Each DMA buffer size */
	u32 produced;			/* Counter: Buf produced by dev */
};

/* Allocate buffer context */
static int rcar_drif_alloc_bufctxt(struct rcar_drif_chan *ch)
{
	struct rcar_drif_hwbuf *bufctx;
	u32 i, idx;

	/* Alloc buf contexts for given number of sub-channels */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		bufctx = kcalloc(ch->num_hwbufs, sizeof(*bufctx), GFP_KERNEL);
		if (!bufctx)
			return 1;

		for (idx = 0; idx < ch->num_hwbufs; idx++)
			ch->schan[i]->buf[idx] = bufctx + idx;
	}
	return 0;
}

/* Release buffer context */
static void rcar_drif_release_bufctxt(struct rcar_drif_chan *ch)
{
	u32 i;

	/* First entry of first sub channel is the bufctxt ptr always */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		kfree(ch->schan[i]->buf[0]);
		ch->schan[i]->buf[0] = NULL;
	}
}

/* Release DMA channel */
static void rcar_drif_release_dmachannel(struct rcar_drif_chan *ch)
{
	u32 i;

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS)
		if (ch->schan[i]->dmach) {
			dma_release_channel(ch->schan[i]->dmach);
			ch->schan[i]->dmach = NULL;
		}
}

/* Allocate DMA channel */
static int rcar_drif_alloc_dmachannel(struct rcar_drif_chan *ch)
{
	u32 i;
	struct dma_slave_config dma_cfg;
	int ret = -ENODEV;

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ch->schan[i]->dmach =
			dma_request_slave_channel(ch->dev, (i ? "rx1" : "rx0"));
		if (!ch->schan[i]->dmach) {
			dev_err(&ch->vdev.dev,
				"schan%u: dma channel req failed\n", i);
			goto dmach_error;
		}

		/* Configure slave */
		memset(&dma_cfg, 0, sizeof(dma_cfg));
		dma_cfg.src_addr = (phys_addr_t)(ch->schan[i]->start +
						 RCAR_DRIF_SIRFDR);
		dma_cfg.dst_addr = 0;
		dma_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		ret = dmaengine_slave_config(ch->schan[i]->dmach, &dma_cfg);
		if (ret) {
			dev_err(&ch->vdev.dev,
				"schan%u: dma slave config failed\n", i);
			goto dmach_error;
		}
		dev_dbg(&ch->vdev.dev, "schan%u: dma slave configed\n", i);
	}

	return 0;

dmach_error:
	rcar_drif_release_dmachannel(ch);
	return ret;
}

/* Release queued vb2 buffers */
static void rcar_drif_release_queued_bufs(struct rcar_drif_chan *ch,
					  enum vb2_buffer_state state)
{
	struct rcar_drif_frame_buf *fbuf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&ch->queued_bufs_lock, flags);
	list_for_each_entry_safe(fbuf, tmp, &ch->queued_bufs, list) {
		list_del(&fbuf->list);
		vb2_buffer_done(&fbuf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&ch->queued_bufs_lock, flags);
}

/* Set MDR defaults */
static inline void rcar_drif_set_mdr1(struct rcar_drif_chan *ch)
{
	u32 i;

	/* Set defaults for both sub-channels */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		/* Refer MSIOF section in manual for this register setting */
		writel(RCAR_DRIF_SITMDR1_PCON,
		       ch->schan[i]->base + RCAR_DRIF_SITMDR1);

		/* Setup MDR1 value */
		writel(ch->mdr1, ch->schan[i]->base + RCAR_DRIF_SIRMDR1);

		dev_dbg(&ch->vdev.dev,
			"schan%u: SIRMDR1 = 0x%08x",
			i, readl(ch->schan[i]->base + RCAR_DRIF_SIRMDR1));
	}
}

/* Extract bitlen and wdcnt from given word length */
static int rcar_drif_convert_wdlen(struct rcar_drif_chan *ch,
				   u32 wdlen, u32 *bitlen, u32 *wdcnt)
{
	u32 i, nr_wds;

	/* FIFO register size is 32 bits */
	for (i = 0; i < 32; i++) {
		nr_wds = wdlen % (32 - i);
		if (nr_wds == 0) {
			*bitlen = (32 - i);
			*wdcnt = (wdlen / *bitlen);
			break;
		}
	}

	/* Sanity check range */
	if ((i == 32) || !((*bitlen >= 8) && (*bitlen <= 32)) ||
	    !((*wdcnt >= 1) && (*wdcnt <= 64))) {
		dev_err(&ch->vdev.dev, "invalid wdlen %u configured\n",
			wdlen);
		return 1;
	}

	return 0;
}

/* Set DRIF receive format */
static int rcar_drif_set_format(struct rcar_drif_chan *ch)
{
	u32 i, bitlen, wdcnt, wdlen;

	wdlen = formats[ch->fmt_idx].wdlen;
	dev_dbg(&ch->vdev.dev, "setfmt: idx %u, wdlen %u, num_schans %u\n",
		ch->fmt_idx, wdlen, formats[ch->fmt_idx].num_schans);

	/* Sanity check */
	if (formats[ch->fmt_idx].num_schans > ch->num_cur_schans) {
		dev_err(ch->dev, "fmt idx %u current schans %u mismatch\n",
			ch->fmt_idx, ch->num_cur_schans);
		return 1;
	}

	if (rcar_drif_convert_wdlen(ch, wdlen, &bitlen, &wdcnt))
		return 1;

	/* Setup group, bitlen & word count */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		u32 mdr;

		/* Two groups */
		mdr = RCAR_DRIF_MDR_GRPCNT(2) | RCAR_DRIF_MDR_BITLEN(bitlen) |
		       RCAR_DRIF_MDR_WDCNT(wdcnt);
		writel(mdr, ch->schan[i]->base + RCAR_DRIF_SIRMDR2);

		mdr = RCAR_DRIF_MDR_BITLEN(bitlen) | RCAR_DRIF_MDR_WDCNT(wdcnt);
		writel(mdr, ch->schan[i]->base + RCAR_DRIF_SIRMDR3);

		dev_dbg(&ch->vdev.dev,
			"schan%u: new SIRMDR[2,3] = 0x%08x 0x%08x\n",
			i, readl(ch->schan[i]->base + RCAR_DRIF_SIRMDR2),
			readl(ch->schan[i]->base + RCAR_DRIF_SIRMDR3));
	}
	return 0;
}

/* Release DMA buffers */
static void rcar_drif_release_buf(struct rcar_drif_chan *ch)
{
	u32 i;

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		struct rcar_drif_subchan *schan = ch->schan[i];

		/* First entry of the sub-channel contains the dma buf ptr */
		if (schan->buf[0] && schan->buf[0]->addr) {
			dma_free_coherent(ch->dev,
				ch->hwbuf_size * ch->num_hwbufs,
				schan->buf[0]->addr, schan->dma_handle);
			schan->buf[0]->addr = NULL;
		}
	}
}

/* Request DMA buffers */
static int rcar_drif_request_buf(struct rcar_drif_chan *ch)
{
	int ret = 0;
	u32 i, j;
	void *addr;

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		struct rcar_drif_subchan *schan = ch->schan[i];

		/* Allocate DMA buffers */
		addr = dma_alloc_coherent(ch->dev,
				ch->hwbuf_size * ch->num_hwbufs,
				&schan->dma_handle, GFP_KERNEL);
		if (!addr) {
			dev_err(&ch->vdev.dev,
				"dma alloc failed. num_hwbufs %u hwbuf_size %u\n",
				ch->num_hwbufs, ch->hwbuf_size);
			ret = 1;
			goto alloc_error;
		}

		for (j = 0; j < ch->num_hwbufs; j++) {
			schan->buf[j]->addr = addr + (j * ch->hwbuf_size);
			schan->buf[j]->status = 0;
		}
	}

	return 0;

alloc_error:
	return ret;
}

/* Setup vb_queue minimum buffer requirements */
static int rcar_drif_queue_setup(struct vb2_queue *vq,
			unsigned int *num_buffers, unsigned int *num_planes,
			unsigned int sizes[], struct device *alloc_devs[])
{
	struct rcar_drif_chan *ch = vb2_get_drv_priv(vq);

	dev_dbg(&ch->vdev.dev, "num_buffers %u vq->num_buffers %u\n",
		*num_buffers, vq->num_buffers);

	/* Need at least 16 buffers */
	if (vq->num_buffers + *num_buffers < 16)
		*num_buffers = 16 - vq->num_buffers;

	*num_planes = 1;
	sizes[0] = PAGE_ALIGN(formats[ch->fmt_idx].buffersize);

	dev_dbg(&ch->vdev.dev, "nbuf %d sizes[0] %d\n", *num_buffers, sizes[0]);
	return 0;
}

/* Enqueue buffer */
static void rcar_drif_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rcar_drif_chan *ch = vb2_get_drv_priv(vb->vb2_queue);
	struct rcar_drif_frame_buf *fbuf =
			container_of(vbuf, struct rcar_drif_frame_buf, vb);
	unsigned long flags;

	dev_dbg(&ch->vdev.dev, "buf_queue idx %u len %u, used %d\n", vb->index,
		vb->planes[0].length, vb->planes[0].bytesused);
	spin_lock_irqsave(&ch->queued_bufs_lock, flags);
	list_add_tail(&fbuf->list, &ch->queued_bufs);
	spin_unlock_irqrestore(&ch->queued_bufs_lock, flags);
}

/* Deliver buffer to user space */
static void rcar_drif_deliver_buf(struct rcar_drif_subchan *schan, u32 idx)
{
	struct rcar_drif_chan *ch = schan->parent;
	struct rcar_drif_frame_buf *fbuf;
	unsigned long flags;
	bool overflow = false;

	spin_lock_irqsave(&ch->queued_bufs_lock, flags);
	fbuf = list_first_entry_or_null(&ch->queued_bufs, struct
					rcar_drif_frame_buf, list);
	if (!fbuf) {
		/* App is late in enqueing buffers. There will be a gap in
		 * sequence number when app recovers
		 */
		dev_dbg(&ch->vdev.dev, "app late: schan%u: prod %u\n",
			schan->num, ch->produced);
		ch->produced++;	/* Increment the produced count anyway */
		spin_unlock_irqrestore(&ch->queued_bufs_lock, flags);
		return;
	}
	list_del(&fbuf->list);
	spin_unlock_irqrestore(&ch->queued_bufs_lock, flags);

	if (ch->num_cur_schans == RCAR_DRIF_MAX_SUBCHANS) {
		struct rcar_drif_hwbuf *buf_i, *buf_q;

		if (schan->num) {
			buf_i = to_rcar_drif_buf_pair(ch, schan->num, idx);
			buf_q = schan->buf[idx];
		} else {
			buf_q = to_rcar_drif_buf_pair(ch, schan->num, idx);
			buf_i = schan->buf[idx];
		}
		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0),
		       buf_i->addr, ch->hwbuf_size);
		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0) + ch->hwbuf_size,
		       buf_q->addr, ch->hwbuf_size);

		if ((buf_i->status | buf_q->status) & RCAR_DRIF_BUF_OVERFLOW) {
			/* Clear the flag in status */
			buf_i->status &= ~RCAR_DRIF_BUF_OVERFLOW;
			buf_q->status &= ~RCAR_DRIF_BUF_OVERFLOW;
			overflow = true;
		}
	} else {
		struct rcar_drif_hwbuf *buf_iq;

		buf_iq = schan->buf[idx];
		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0),
		       buf_iq->addr, ch->hwbuf_size);

		if (buf_iq->status & RCAR_DRIF_BUF_OVERFLOW) {
			/* Clear the flag in status */
			buf_iq->status &= ~RCAR_DRIF_BUF_OVERFLOW;
			overflow = true;
		}
	}

	dev_dbg(&ch->vdev.dev, "schan%u: produced %u\n",
		schan->num, ch->produced);

	fbuf->vb.field = V4L2_FIELD_NONE;
	fbuf->vb.sequence = ch->produced++;
	fbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&fbuf->vb.vb2_buf, 0,
			      formats[ch->fmt_idx].buffersize);
	if (overflow)
		vb2_buffer_done(&fbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	else
		vb2_buffer_done(&fbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* Check if DMA of the paired sub-channel is also complete */
static void rcar_drif_dmapair_complete(struct rcar_drif_subchan *schan, u32 idx)
{
	struct rcar_drif_chan *ch = schan->parent;
	struct rcar_drif_hwbuf *buf, *pair;

	buf = schan->buf[idx];
	pair = to_rcar_drif_buf_pair(ch, schan->num, idx);
	buf->status |= RCAR_DRIF_BUF_DONE;
	dev_dbg(&ch->vdev.dev, "schan%u: status: buf 0x%08x pair 0x%08x\n",
		schan->num, buf->status, pair->status);

	/* Check if both DMA buffers are done */
	if (RCAR_DRIF_BUF_PAIRS_DONE(buf, pair)) {
		/* Clear status flag */
		buf->status &= ~RCAR_DRIF_BUF_DONE;
		pair->status &= ~RCAR_DRIF_BUF_DONE;

		/* Deliver buffer to user */
		rcar_drif_deliver_buf(schan, idx);
	}
}

/* DMA callback for each stage */
static void rcar_drif_dma_complete(void *dma_async_param)
{
	struct rcar_drif_subchan *schan =
		(struct rcar_drif_subchan *)dma_async_param;
	struct rcar_drif_chan *ch = schan->parent;
	struct rcar_drif_hwbuf *buf;
	u32 str, idx;

	mutex_lock(&ch->vb_queue_mutex);

	/* DMA can be terminated while the callback was waiting on lock */
	if (!vb2_is_streaming(&ch->vb_queue)) {
		mutex_unlock(&ch->vb_queue_mutex);
		return;
	}

	idx = ch->produced % ch->num_hwbufs;
	buf = schan->buf[idx];

	/* Check for DRIF errors */
	str = readl(schan->base + RCAR_DRIF_SISTR);
	if (str & RCAR_DRIF_RFOVF) {
		/* Writing the same clears it */
		writel(str, schan->base + RCAR_DRIF_SISTR);

		/* Overflow: some samples are lost */
		buf->status |= RCAR_DRIF_BUF_OVERFLOW;
		dev_dbg(&ch->vdev.dev, "fifo overflow: schan%u: prod %d\n",
			schan->num, ch->produced);
	}

	/* Check if the rx'ed data can be delivered to user */
	if (ch->num_cur_schans == RCAR_DRIF_MAX_SUBCHANS)
		rcar_drif_dmapair_complete(schan, idx);
	else
		rcar_drif_deliver_buf(schan, idx);

	mutex_unlock(&ch->vb_queue_mutex);
}

static int rcar_drif_qbuf(struct rcar_drif_subchan *schan)
{
	struct rcar_drif_chan *ch = schan->parent;
	dma_addr_t addr = schan->dma_handle;
	struct dma_async_tx_descriptor *rxd;
	dma_cookie_t cookie;

	/* Setup cyclic DMA with given buffers */
	rxd = dmaengine_prep_dma_cyclic(schan->dmach, addr,
					ch->hwbuf_size * ch->num_hwbufs,
					ch->hwbuf_size, DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rxd) {
		dev_err(&ch->vdev.dev, "schan%u: prep dma cyclic failed\n",
			schan->num);
		return 1;
	}

	/* Submit descriptor */
	rxd->callback = rcar_drif_dma_complete;
	rxd->callback_param = schan;
	dev_dbg(&ch->vdev.dev, "schan%u: param %p\n", schan->num, schan);
	cookie = dmaengine_submit(rxd);
	if (dma_submit_error(cookie)) {
		dev_err(&ch->vdev.dev, "schan%u: dma submit failed\n",
			schan->num);
		return 1;
	}

	dma_async_issue_pending(schan->dmach);
	return 0;
}

/* Enable reception */
static int rcar_drif_enable_rx(struct rcar_drif_chan *ch)
{
	u32 ctr, i;
	int ret;

	/*
	 * When both sub-channels are enabled, they can be syncronized only by
	 * the master
	 */

	/* Enable receive */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ctr = readl(ch->schan[i]->base + RCAR_DRIF_SICTR);
		ctr |= (RCAR_DRIF_SICTR_RX_RISING_EDGE |
			 RCAR_DRIF_SICTR_RX_EN);
		writel(ctr, ch->schan[i]->base + RCAR_DRIF_SICTR);
	}

	/* Check receive enabled */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ret = readl_poll_timeout(ch->schan[i]->base + RCAR_DRIF_SICTR,
					 ctr, ctr & RCAR_DRIF_SICTR_RX_EN,
					 2, 500000);
		if (ret) {
			dev_err(&ch->vdev.dev,
			"schan%u: failed to enable rx. CTR 0x%08x\n",
			i, readl(ch->schan[i]->base + RCAR_DRIF_SICTR));
			break;
		}
	}
	return ret;
}

/* Disable reception */
static void rcar_drif_disable_rx(struct rcar_drif_chan *ch)
{
	u32 ctr, i;
	int ret;

	/* Disable receive */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ctr = readl(ch->schan[i]->base + RCAR_DRIF_SICTR);
		ctr &= ~RCAR_DRIF_SICTR_RX_EN;
		writel(ctr, ch->schan[i]->base + RCAR_DRIF_SICTR);
	}

	/* Check receive disabled */
	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ret = readl_poll_timeout(ch->schan[i]->base + RCAR_DRIF_SICTR,
					 ctr, !(ctr & RCAR_DRIF_SICTR_RX_EN),
					 2, 500000);
		if (ret)
			dev_warn(&ch->vdev.dev,
			"schan%u: failed to disable rx. CTR 0x%08x\n",
			i, readl(ch->schan[i]->base + RCAR_DRIF_SICTR));
	}
}

/* Start sub-channel */
static int rcar_drif_start_subchan(struct rcar_drif_subchan *schan)
{
	struct rcar_drif_chan *ch = schan->parent;
	u32 ctr, str;
	int ret;

	/* Reset receive */
	writel(RCAR_DRIF_SICTR_RESET, schan->base + RCAR_DRIF_SICTR);
	ret = readl_poll_timeout(schan->base + RCAR_DRIF_SICTR,
					 ctr, !(ctr & RCAR_DRIF_SICTR_RESET),
					 2, 500000);
	if (ret) {
		dev_err(&ch->vdev.dev,
			"schan%u: failed to reset rx. CTR 0x%08x\n",
			schan->num, readl(schan->base + RCAR_DRIF_SICTR));
		return ret;
	}

	/* Queue buffers for DMA */
	if (rcar_drif_qbuf(schan)) {
		dev_err(&ch->vdev.dev,
			"failed to queue schan %u DMA transfer.\n", schan->num);
		return 1;
	}

	/* Clear status register flags */
	str = RCAR_DRIF_RFFUL | RCAR_DRIF_REOF | RCAR_DRIF_RFSERR |
		RCAR_DRIF_RFUDF | RCAR_DRIF_RFOVF;
	writel(str, schan->base + RCAR_DRIF_SISTR);

	/* Enable DMA receive interrupt */
	writel(0x00009000, schan->base + RCAR_DRIF_SIIER);

	return 0;
}

/* Start receive operation */
static int rcar_drif_start(struct rcar_drif_chan *ch)
{
	u32 i;
	int ret;

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ret = rcar_drif_start_subchan(ch->schan[i]);
		if (ret)
			goto start_error;
	}

	/* Reset counters */
	ch->produced = 0;

	/* Enable Rx */
	ret = rcar_drif_enable_rx(ch);
	if (ret)
		goto start_error;

	dev_dbg(&ch->vdev.dev, "started\n");

start_error:
	return ret;
}

/* Stop sub-channel */
static void rcar_drif_stop_subchan(struct rcar_drif_subchan *schan)
{
	struct rcar_drif_chan *ch = schan->parent;
	int ret, retries = 3;

	/* Disable DMA receive interrupt */
	writel(0x00000000, schan->base + RCAR_DRIF_SIIER);

	do {
		/* Terminate all DMA transfers */
		ret = dmaengine_terminate_sync(schan->dmach);
		if (!ret)
			break;
		dev_dbg(&ch->vdev.dev, "stop retry\n");
	} while (--retries);

	WARN_ON(!retries);
}

/* Stop receive operation */
static void rcar_drif_stop(struct rcar_drif_chan *ch)
{
	u32 i;

	/* Disable Rx */
	rcar_drif_disable_rx(ch);

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS)
		rcar_drif_stop_subchan(ch->schan[i]);

	dev_dbg(&ch->vdev.dev, "stopped: prod %u\n", ch->produced);
}

/* Start streaming */
static int rcar_drif_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rcar_drif_chan *ch = vb2_get_drv_priv(vq);
	int ret, i, j;

	dev_dbg(&ch->vdev.dev, "start streaming\n");
	mutex_lock(&ch->v4l2_mutex);

	for_each_set_bit(i, &ch->cur_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		ret = clk_prepare_enable(ch->schan[i]->clkp);
		if (ret)
			goto start_error;
	}

	/* Set default MDRx settings */
	rcar_drif_set_mdr1(ch);

	/* Set new format */
	ret = rcar_drif_set_format(ch);
	if (ret)
		goto start_error;

	if (ch->num_cur_schans == RCAR_DRIF_MAX_SUBCHANS)
		ch->hwbuf_size = formats[ch->fmt_idx].buffersize/2;
	else
		ch->hwbuf_size = formats[ch->fmt_idx].buffersize;

	dev_dbg(&ch->vdev.dev, "num_hwbufs %u, hwbuf_size %u\n",
		ch->num_hwbufs, ch->hwbuf_size);

	if (rcar_drif_alloc_dmachannel(ch)) {
		ret = -ENODEV;
		goto start_error;
	}

	/* Alloc buf context */
	if (rcar_drif_alloc_bufctxt(ch)) {
		ret = -ENOMEM;
		goto start_error;
	}

	if (rcar_drif_request_buf(ch)) {
		ret = -ENOMEM;
		goto start_error;
	}

	dev_dbg(&ch->vdev.dev, "dma setup successful\n");

	ret = rcar_drif_start(ch);
	if (ret)
		goto start_error;

	mutex_unlock(&ch->v4l2_mutex);
	return ret;

start_error:
	/* Return all queued buffers to vb2 */
	rcar_drif_release_queued_bufs(ch, VB2_BUF_STATE_QUEUED);
	rcar_drif_release_buf(ch);
	rcar_drif_release_bufctxt(ch);
	rcar_drif_release_dmachannel(ch);
	for (j = 0; j < i; j++)
		clk_disable_unprepare(ch->schan[j]->clkp);

	mutex_unlock(&ch->v4l2_mutex);
	return ret;
}

/* Stop streaming */
static void rcar_drif_stop_streaming(struct vb2_queue *vq)
{
	struct rcar_drif_chan *ch = vb2_get_drv_priv(vq);
	u32 i;

	mutex_lock(&ch->v4l2_mutex);

	/* Stop hardware streaming */
	rcar_drif_stop(ch);

	/* Return all queued buffers to vb2 */
	rcar_drif_release_queued_bufs(ch, VB2_BUF_STATE_ERROR);

	/* Release buf & buf context */
	rcar_drif_release_buf(ch);
	rcar_drif_release_bufctxt(ch);

	/* Release DMA channel resources */
	rcar_drif_release_dmachannel(ch);

	for (i = 0; i < RCAR_DRIF_MAX_SUBCHANS; i++)
		clk_disable_unprepare(ch->schan[i]->clkp);

	mutex_unlock(&ch->v4l2_mutex);
	dev_dbg(&ch->vdev.dev, "stopped streaming\n");
}

/* Vb2 ops */
static struct vb2_ops rcar_drif_vb2_ops = {
	.queue_setup            = rcar_drif_queue_setup,
	.buf_queue              = rcar_drif_buf_queue,
	.start_streaming        = rcar_drif_start_streaming,
	.stop_streaming         = rcar_drif_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int rcar_drif_querycap(struct file *file, void *fh,
		struct v4l2_capability *cap)
{
	struct rcar_drif_chan *ch = video_drvdata(file);

	strlcpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strlcpy(cap->card, ch->vdev.name, sizeof(cap->card));
	cap->device_caps = V4L2_CAP_SDR_CAPTURE | V4L2_CAP_TUNER |
				   V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 ch->vdev.name);
	return 0;
}

static int rcar_drif_set_default_format(struct rcar_drif_chan *ch)
{
	u32 i;

	for (i = 0; i < NUM_FORMATS; i++) {
		/* Find any matching fmt and set it as default */
		if (ch->num_hw_schans == formats[i].num_schans) {
			ch->fmt_idx = i;
			ch->cur_schans_mask = ch->hw_schans_mask;
			ch->num_cur_schans = ch->num_hw_schans;
			dev_dbg(ch->dev, "default fmt[%u]: mask %lu num %u\n",
				i, ch->cur_schans_mask, ch->num_cur_schans);
			return 0;
		}
	}
	return -EINVAL;
}

static int rcar_drif_enum_fmt_sdr_cap(struct file *file, void *priv,
				      struct v4l2_fmtdesc *f)
{
	if (f->index >= NUM_FORMATS)
		return -EINVAL;

	strlcpy(f->description, formats[f->index].name, sizeof(f->description));
	f->pixelformat = formats[f->index].pixelformat;
	return 0;
}

static int rcar_drif_g_fmt_sdr_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct rcar_drif_chan *ch = video_drvdata(file);

	f->fmt.sdr.pixelformat = formats[ch->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[ch->fmt_idx].buffersize;
	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	return 0;
}

static int rcar_drif_s_fmt_sdr_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct vb2_queue *q = &ch->vb_queue;
	u32 i;

	if (vb2_is_busy(q))
		return -EBUSY;

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].pixelformat == f->fmt.sdr.pixelformat) {
			ch->fmt_idx  = i;
			f->fmt.sdr.buffersize = formats[i].buffersize;

			/* If a format demands one subch out of two enabled
			 * sub-channels then pick the 0th sub-channel
			 */
			if (formats[i].num_schans < ch->num_hw_schans) {
				ch->cur_schans_mask = BIT(0);	/* Enable D0 */
				ch->num_cur_schans = formats[i].num_schans;
			} else {
				ch->cur_schans_mask = ch->hw_schans_mask;
				ch->num_cur_schans = ch->num_hw_schans;
			}

			dev_dbg(&ch->vdev.dev, "cur: idx %u mask %lu num %u\n",
				i, ch->cur_schans_mask, ch->num_cur_schans);
			return 0;
		}
	}

	if (rcar_drif_set_default_format(ch))
		return -EINVAL;

	f->fmt.sdr.pixelformat = formats[ch->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[ch->fmt_idx].buffersize;
	return 0;
}

static int rcar_drif_try_fmt_sdr_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	u32 i;

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].pixelformat == f->fmt.sdr.pixelformat) {
			f->fmt.sdr.buffersize = formats[i].buffersize;
			return 0;
		}
	}

	f->fmt.sdr.pixelformat = formats[ch->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[ch->fmt_idx].buffersize;
	return 0;
}

static int rcar_drif_enum_freq_bands(struct file *file, void *priv,
		struct v4l2_frequency_band *band)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct v4l2_subdev *sd, *tmp;
	int ret = 0;

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list)
		ret = v4l2_subdev_call(sd, tuner, enum_freq_bands, band);

	return ret;
}

static int rcar_drif_g_frequency(struct file *file, void *priv,
		struct v4l2_frequency *f)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct v4l2_subdev *sd, *tmp;
	int ret = 0;

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list)
		ret = v4l2_subdev_call(sd, tuner, g_frequency, f);

	return ret;
}

static int rcar_drif_s_frequency(struct file *file, void *priv,
				 const struct v4l2_frequency *f)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct v4l2_subdev *sd, *tmp;
	int ret = 0;

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list)
		ret = v4l2_subdev_call(sd, tuner, s_frequency, f);

	return ret;
}

static int rcar_drif_g_tuner(struct file *file, void *priv,
			     struct v4l2_tuner *vt)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct v4l2_subdev *sd, *tmp;
	int ret = 0;

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list)
		ret = v4l2_subdev_call(sd, tuner, g_tuner, vt);

	return ret;
}

static int rcar_drif_s_tuner(struct file *file, void *priv,
			     const struct v4l2_tuner *vt)
{
	struct rcar_drif_chan *ch = video_drvdata(file);
	struct v4l2_subdev *sd, *tmp;
	int ret = 0;

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list)
		ret = v4l2_subdev_call(sd, tuner, s_tuner, vt);

	return ret;
}

static const struct v4l2_ioctl_ops rcar_drif_ioctl_ops = {
	.vidioc_querycap          = rcar_drif_querycap,

	.vidioc_enum_fmt_sdr_cap  = rcar_drif_enum_fmt_sdr_cap,
	.vidioc_g_fmt_sdr_cap     = rcar_drif_g_fmt_sdr_cap,
	.vidioc_s_fmt_sdr_cap     = rcar_drif_s_fmt_sdr_cap,
	.vidioc_try_fmt_sdr_cap   = rcar_drif_try_fmt_sdr_cap,

	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_create_bufs       = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf       = vb2_ioctl_prepare_buf,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,

	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,

	.vidioc_s_frequency       = rcar_drif_s_frequency,
	.vidioc_g_frequency       = rcar_drif_g_frequency,
	.vidioc_s_tuner		  = rcar_drif_s_tuner,
	.vidioc_g_tuner		  = rcar_drif_g_tuner,
	.vidioc_enum_freq_bands   = rcar_drif_enum_freq_bands,
	.vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_log_status        = v4l2_ctrl_log_status,
};

static const struct v4l2_file_operations rcar_drif_fops = {
	.owner                    = THIS_MODULE,
	.open                     = v4l2_fh_open,
	.release                  = vb2_fop_release,
	.read                     = vb2_fop_read,
	.poll                     = vb2_fop_poll,
	.mmap                     = vb2_fop_mmap,
	.unlocked_ioctl           = video_ioctl2,
};

static struct video_device rcar_drif_template = {
	.name                     = "R-Car DRIF",
	.release                  = video_device_release_empty,
	.fops                     = &rcar_drif_fops,
	.ioctl_ops                = &rcar_drif_ioctl_ops,
};

static void rcar_drif_video_release(struct v4l2_device *v)
{
	struct rcar_drif_chan *ch = container_of(v, struct rcar_drif_chan,
						v4l2_dev);
	u32 i;

	dev_dbg(ch->dev, "video release\n");
	v4l2_device_unregister(&ch->v4l2_dev);
	for_each_set_bit(i, &ch->hw_schans_mask, RCAR_DRIF_MAX_SUBCHANS)
		kfree(ch->schan[i]);
	kfree(ch);
}

static int rcar_drif_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct rcar_drif_chan *ch =
		container_of(notifier, struct rcar_drif_chan, notifier);
	dev_dbg(ch->dev, "bound: asd %s\n", asd->match.of.node->name);
	return 0;
}

/* Sub-device registered notification callback */
static int rcar_drif_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct rcar_drif_chan *ch =
		container_of(notifier, struct rcar_drif_chan, notifier);
	struct v4l2_subdev *sd, *tmp;
	int ret;

	v4l2_ctrl_handler_init(&ch->ctrl_hdl, 10);
	ch->v4l2_dev.ctrl_handler = &ch->ctrl_hdl;
	dev_dbg(ch->dev, "notify complete\n");
	ret = v4l2_device_register_subdev_nodes(&ch->v4l2_dev);
	if (ret) {
		dev_err(ch->dev, "failed register subdev nodes ret %d\n", ret);
		return ret;
	}

	list_for_each_entry_safe(sd, tmp, &ch->v4l2_dev.subdevs, list) {
		ret = v4l2_ctrl_add_handler(ch->v4l2_dev.ctrl_handler,
					    sd->ctrl_handler, NULL);
		if (ret) {
			dev_err(ch->dev, "failed ctrl add hdlr ret %d\n", ret);
			return ret;
		}
	}
	return 0;
}

/* Parse sub-devs (tuner) to find a matching device */
static int rcar_drif_parse_subdevs(struct device *dev,
				   struct v4l2_async_notifier *notifier)
{
	struct device_node *node = NULL;

	notifier->subdevs = devm_kcalloc(dev, RCAR_DRIF_MAX_SUBDEVS,
				sizeof(*notifier->subdevs), GFP_KERNEL);
	if (!notifier->subdevs)
		return -ENOMEM;

	while (notifier->num_subdevs < RCAR_DRIF_MAX_SUBDEVS &&
	       (node = of_graph_get_next_endpoint(dev->of_node, node))) {
		struct rcar_drif_async_subdev *rsd;

		dev_dbg(dev, "node name %s\n", node->name);
		rsd = devm_kzalloc(dev, sizeof(*rsd), GFP_KERNEL);
		if (!rsd) {
			of_node_put(node);
			return -ENOMEM;
		}

		notifier->subdevs[notifier->num_subdevs] = &rsd->asd;
		rsd->asd.match.of.node = of_graph_get_remote_port_parent(node);
		of_node_put(node);
		if (!rsd->asd.match.of.node) {
			dev_warn(dev, "bad remote port parent\n");
			return -EINVAL;
		}

		rsd->asd.match_type = V4L2_ASYNC_MATCH_OF;
		notifier->num_subdevs++;
	}
	return 0;
}

/* SIRMDR1 configuration */
static void rcar_drif_validate_syncmd(struct rcar_drif_chan *ch, u32 val)
{
	if (val > 1) {
		dev_warn(ch->dev, "invalid syncmd %u using L/R mode\n", val);
		return;
	}

	ch->mdr1 &= ~(3 << 28);	/* Clear current settings */
	if (val == 0)
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCMD_FRAME;
	else
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCMD_LR;
}

/* Get the dtdl or syncdl bits as in MSIOF */
static u32 rcar_drif_get_dtdl_or_syncdl_bits(u32 dtdl_or_syncdl)
{
	/*
	 * DTDL/SYNCDL bit	: dtdl/syncdl
	 * b'000		: 0
	 * b'001		: 100
	 * b'010		: 200
	 * b'011 (SYNCDL only)	: 300
	 * b'101		: 50
	 * b'110		: 150
	 */
	if (dtdl_or_syncdl % 100)
		return dtdl_or_syncdl / 100 + 5;
	else
		return dtdl_or_syncdl / 100;
}

static void rcar_drif_validate_dtdl_syncdl(struct rcar_drif_chan *ch, u32 dtdl,
					   u32 syncdl)
{
	/* Sanity checks */
	if (dtdl > 200 || syncdl > 300) {
		dev_warn(ch->dev, "invalid dtdl %u or syncdl %u\n",
			 dtdl, syncdl);
		return;
	}
	if ((dtdl + syncdl) % 100) {
		dev_warn(ch->dev, "sum of dtdl %u or syncdl %u not OK\n",
			 dtdl, syncdl);
		return;
	}
	ch->mdr1 &= (~(7 << 20) & ~(7 << 16));	/* Clear current settings */
	ch->mdr1 |= (rcar_drif_get_dtdl_or_syncdl_bits(dtdl) << 20);
	ch->mdr1 |= (rcar_drif_get_dtdl_or_syncdl_bits(syncdl) << 16);
}

static void rcar_drif_parse_properties(struct rcar_drif_chan *ch)
{
	u32 val, dtdl = 100, syncdl = 0;
	struct device_node *np = ch->dev->of_node;

	/* Set the defaults and check for overrides */
	ch->mdr1 = RCAR_DRIF_SIRMDR1_SYNCMD_LR;	/* Default */
	if (!of_property_read_u32(np, "renesas,syncmd", &val))
		rcar_drif_validate_syncmd(ch, val);

	if (of_find_property(np, "renesas,lsb-first", NULL))
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_LSB_FIRST;
	else
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_MSB_FIRST;

	if (of_find_property(np, "renesas,syncac-pol-high", NULL))
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCAC_POL_HIGH;
	else
		ch->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCAC_POL_LOW;

	ch->mdr1 |= RCAR_DRIF_SIRMDR1_DTDL_1 | RCAR_DRIF_SIRMDR1_SYNCDL_0;
	of_property_read_u32(np, "renesas,dtdl", &dtdl);
	of_property_read_u32(np, "renesas,syncdl", &syncdl);
	rcar_drif_validate_dtdl_syncdl(ch, dtdl, syncdl);

	dev_info(ch->dev, "Parsed SIRMDR1 0x%08x\n", ch->mdr1);
}

static int rcar_drif_probe(struct platform_device *pdev)
{
	struct rcar_drif_chan *ch;
	struct device_node *of_child;
	unsigned long hw_schans_mask = 0;
	int ret;
	u32 i;

	of_child = of_get_child_by_name(pdev->dev.of_node, "sub-channel0");
	if (of_child && of_device_is_available(of_child))
		hw_schans_mask |= BIT(0);	/* Sub-channel 0 */

	of_child = of_get_child_by_name(pdev->dev.of_node, "sub-channel1");
	if (of_child && of_device_is_available(of_child))
		hw_schans_mask |= BIT(1);	/* Sub-channel 1 */

	/* Reserve memory for driver structure */
	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch) {
		ret = PTR_ERR(ch);
		dev_err(&pdev->dev, "failed alloc drif context\n");
		return ret;
	}
	ch->dev = &pdev->dev;

	/* Parse device tree optional properties */
	rcar_drif_parse_properties(ch);

	/* Setup enabled sub-channels */
	for_each_set_bit(i, &hw_schans_mask, RCAR_DRIF_MAX_SUBCHANS) {
		struct clk *clkp;
		struct resource	*res;
		void __iomem *base;

		/* Peripheral clock */
		clkp = devm_clk_get(&pdev->dev, (i == 0 ? "fck0" : "fck1"));
		if (IS_ERR(clkp)) {
			ret = PTR_ERR(clkp);
			dev_err(&pdev->dev, "schan%u: clk get failed\n", i);
			goto err_free_mem;
		}

		/* Register map */
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base)) {
			ret = PTR_ERR(base);
			dev_err(&pdev->dev, "schan%u: io res get failed\n", i);
			break;
		}

		/* Reserve memory for enabled sub-channels */
		ch->schan[i] = kzalloc(sizeof(*ch->schan[i]), GFP_KERNEL);
		if (!ch->schan[i]) {
			ret = PTR_ERR(ch);
			dev_err(&pdev->dev, "failed alloc sub-channel\n");
			goto err_free_mem;
		}
		ch->schan[i]->clkp = clkp;
		ch->schan[i]->base = base;
		ch->schan[i]->num = i;
		ch->schan[i]->start = res->start;
		ch->schan[i]->parent = ch;
		ch->num_hw_schans++;
	}
	ch->hw_schans_mask = hw_schans_mask;

	/* Validate any supported format for enabled sub-channels */
	ret = rcar_drif_set_default_format(ch);
	if (ret)
		goto err_free_mem;

	/* Set defaults */
	ch->num_hwbufs = RCAR_DRIF_DEFAULT_NUM_HWBUFS;
	ch->hwbuf_size = RCAR_DRIF_DEFAULT_HWBUF_SIZE;

	mutex_init(&ch->v4l2_mutex);
	mutex_init(&ch->vb_queue_mutex);
	spin_lock_init(&ch->queued_bufs_lock);
	INIT_LIST_HEAD(&ch->queued_bufs);

	/* Init videobuf2 queue structure */
	ch->vb_queue.type = V4L2_BUF_TYPE_SDR_CAPTURE;
	ch->vb_queue.io_modes = VB2_READ | VB2_MMAP | VB2_DMABUF;
	ch->vb_queue.drv_priv = ch;
	ch->vb_queue.buf_struct_size = sizeof(struct rcar_drif_frame_buf);
	ch->vb_queue.ops = &rcar_drif_vb2_ops;
	ch->vb_queue.mem_ops = &vb2_vmalloc_memops;
	ch->vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	/* Init videobuf2 queue */
	ret = vb2_queue_init(&ch->vb_queue);
	if (ret) {
		dev_err(ch->dev, "Could not initialize vb2 queue\n");
		goto err_free_mem;
	}

	/* Init video_device structure */
	ch->vdev = rcar_drif_template;
	ch->vdev.lock = &ch->v4l2_mutex;
	ch->vdev.queue = &ch->vb_queue;
	ch->vdev.queue->lock = &ch->vb_queue_mutex;
	ch->vdev.ctrl_handler = &ch->ctrl_hdl;
	video_set_drvdata(&ch->vdev, ch);

	/* Register the v4l2_device structure */
	ch->v4l2_dev.release = rcar_drif_video_release;
	ret = v4l2_device_register(&pdev->dev, &ch->v4l2_dev);
	if (ret) {
		dev_err(ch->dev, "Failed to register v4l2-device (%d)\n", ret);
		goto err_free_mem;
	}

	ch->vdev.v4l2_dev = &ch->v4l2_dev;

	/* Parse subdevs after v4l2_device_register because if the subdev
	 * is already probed, bound and complete will get called immediately
	 */
	ret = rcar_drif_parse_subdevs(&pdev->dev, &ch->notifier);
	if (ret)
		goto err_free_mem;

	dev_info(ch->dev, "Num of subdevs %d\n", ch->notifier.num_subdevs);
	ch->notifier.bound = rcar_drif_notify_bound;
	ch->notifier.complete = rcar_drif_notify_complete;

	ret = v4l2_async_notifier_register(&ch->v4l2_dev, &ch->notifier);
	if (ret < 0) {
		dev_err(ch->dev, "notifier registration failed\n");
		goto err_unregister_v4l2_dev;
	}

	/* Register SDR device */
	ret = video_register_device(&ch->vdev, VFL_TYPE_SDR, -1);
	if (ret) {
		dev_err(ch->dev, "Failed to register as video device (%d)\n",
				ret);
		goto err_unregister;
	}

	platform_set_drvdata(pdev, ch);
	dev_notice(ch->dev, "probed\n");
	return 0;

err_unregister:
	v4l2_async_notifier_unregister(&ch->notifier);
err_unregister_v4l2_dev:
	v4l2_device_unregister(&ch->v4l2_dev);
err_free_mem:
	for_each_set_bit(i, &hw_schans_mask, RCAR_DRIF_MAX_SUBCHANS)
		kfree(ch->schan[i]);
	kfree(ch);
	return ret;
}

static int rcar_drif_remove(struct platform_device *pdev)
{
	struct rcar_drif_chan *ch = platform_get_drvdata(pdev);

	v4l2_ctrl_handler_free(ch->v4l2_dev.ctrl_handler);
	v4l2_async_notifier_unregister(&ch->notifier);
	video_unregister_device(&ch->vdev);
	dev_notice(ch->dev, "removed\n");
	v4l2_device_put(&ch->v4l2_dev);
	return 0;
}

static int __maybe_unused rcar_drif_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused rcar_drif_resume(struct device *dev)
{
	return 0;
}

static SIMPLE_DEV_PM_OPS(rcar_drif_pm_ops, rcar_drif_suspend,
			 rcar_drif_resume);

static const struct of_device_id rcar_drif_of_table[] = {
	{ .compatible = "renesas,rcar-gen3-drif" },
	{ }
};

MODULE_DEVICE_TABLE(of, rcar_drif_of_table);

#define RCAR_DRIF_DRV_NAME "rcar_drif"
static struct platform_driver rcar_drif_driver = {
	.driver = {
		.name = RCAR_DRIF_DRV_NAME,
		.of_match_table = of_match_ptr(rcar_drif_of_table),
		.pm = &rcar_drif_pm_ops,
		},
	.probe = rcar_drif_probe,
	.remove = rcar_drif_remove,
};

module_platform_driver(rcar_drif_driver);

MODULE_DESCRIPTION("Renesas R-Car Gen3 DRIF driver");
MODULE_ALIAS("platform:" RCAR_DRIF_DRV_NAME);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ramesh Shanmugasundaram <ramesh.shanmugasundaram@bp.renesas.com>");
