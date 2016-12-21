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
 * Each DRIF channel DRIFx (as per datasheet) contains two internal
 * channels DRIFx0 & DRIFx1 within itself with each having its own resources
 * like module clk, register set, irq and dma. These internal channels share
 * common CLK & SYNC from master. The two data pins D0 & D1 shall be
 * considered to represent the two internal channels. This internal split
 * is not visible to the master device.
 *
 * Depending on the master device, a DRIF channel can use
 *  (1) both internal channels (D0 & D1) to receive data in parallel (or)
 *  (2) one internal channel (D0 or D1) to receive data
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

/* DRIF register offsets */
#define RCAR_DRIF_SITMDR1			0x00
#define RCAR_DRIF_SITMDR2			0x04
#define RCAR_DRIF_SITMDR3			0x08
#define RCAR_DRIF_SIRMDR1			0x10
#define RCAR_DRIF_SIRMDR2			0x14
#define RCAR_DRIF_SIRMDR3			0x18
#define RCAR_DRIF_SICTR				0x28
#define RCAR_DRIF_SIFCTR			0x30
#define RCAR_DRIF_SISTR				0x40
#define RCAR_DRIF_SIIER				0x44
#define RCAR_DRIF_SIRFDR			0x60

#define RCAR_DRIF_RFOVF			BIT(3)	/* Receive FIFO overflow */
#define RCAR_DRIF_RFUDF			BIT(4)	/* Receive FIFO underflow */
#define RCAR_DRIF_RFSERR		BIT(5)	/* Receive frame sync error */
#define RCAR_DRIF_REOF			BIT(7)	/* Frame reception end */
#define RCAR_DRIF_RDREQ			BIT(12) /* Receive data xfer req */
#define RCAR_DRIF_RFFUL			BIT(13)	/* Receive FIFO full */

/* SIRMDR1 */
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
#define RCAR_DRIF_MAX_CHANNEL			2
#define RCAR_SDR_BUFFER_SIZE			SZ_64K

/* Internal buffer status flags */
#define RCAR_DRIF_BUF_DONE			BIT(0)	/* DMA completed */
#define RCAR_DRIF_BUF_OVERFLOW			BIT(1)	/* Overflow detected */

#define to_rcar_drif_buf_pair(sdr, ch_num, idx)	(sdr->ch[!(ch_num)]->buf[idx])

static const unsigned int num_hwbufs = 32;
#define for_each_rcar_drif_channel(ch, ch_mask)			\
	for_each_set_bit(ch, ch_mask, RCAR_DRIF_MAX_CHANNEL)

/* Debug */
static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "activate debug info");

#define rdrif_dbg(level, sdr, fmt, arg...)				\
	v4l2_dbg(level, debug, &sdr->v4l2_dev, fmt, ## arg)

#define rdrif_err(sdr, fmt, arg...)					\
	dev_err(sdr->v4l2_dev.dev, fmt, ## arg)

/* Stream formats */
struct rcar_drif_format {
	u32	pixelformat;
	u32	buffersize;
	u32	wdlen;
	u32	num_ch;
};

/* Format descriptions for capture and preview */
static const struct rcar_drif_format formats[] = {
	{
		.pixelformat	= V4L2_SDR_FMT_PCU16BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 16,
		.num_ch	= 2,
	},
	{
		.pixelformat	= V4L2_SDR_FMT_PCU18BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 18,
		.num_ch	= 2,
	},
	{
		.pixelformat	= V4L2_SDR_FMT_PCU20BE,
		.buffersize	= RCAR_SDR_BUFFER_SIZE,
		.wdlen		= 20,
		.num_ch	= 2,
	},
};

static const unsigned int NUM_FORMATS = ARRAY_SIZE(formats);

/* Buffer for a received frame from one or both internal channels */
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
	void *addr;			/* CPU-side address */
	unsigned int status;		/* Buffer status flags */
};

/* Internal channel */
struct rcar_drif {
	struct rcar_drif_sdr *sdr;	/* Group device */
	struct platform_device *pdev;	/* Channel's pdev */
	void __iomem *base;		/* Base register address */
	resource_size_t start;		/* I/O resource offset */
	struct dma_chan *dmach;		/* Reserved DMA channel */
	struct clk *clkp;		/* Module clock */
	struct rcar_drif_hwbuf *buf[RCAR_DRIF_MAX_NUM_HWBUFS];
	dma_addr_t dma_handle;		/* Handle for all bufs */
	unsigned int num;		/* Channel number */
	bool acting_sdr;		/* Channel acting as SDR device */
};

/* DRIF V4L2 SDR */
struct rcar_drif_sdr {
	struct device *dev;		/* Platform device */
	struct video_device *vdev;	/* V4L2 SDR device */
	struct v4l2_device v4l2_dev;	/* V4L2 device */

	/* Videobuf2 queue and queued buffers list */
	struct vb2_queue vb_queue;
	struct list_head queued_bufs;
	spinlock_t queued_bufs_lock;	/* Protects queued_bufs */

	struct mutex v4l2_mutex;	/* To serialize ioctls */
	struct mutex vb_queue_mutex;	/* To serialize streaming ioctls */
	struct v4l2_ctrl_handler ctrl_hdl;	/* SDR control handler */
	struct v4l2_async_notifier notifier;	/* For subdev (tuner) */

	/* Current V4L2 SDR format array index */
	unsigned int fmt_idx;

	/* Device tree SYNC properties */
	u32 mdr1;

	/* Internals */
	struct rcar_drif *ch[RCAR_DRIF_MAX_CHANNEL]; /* DRIFx0,1 */
	unsigned long hw_ch_mask;	/* Enabled channels per DT */
	unsigned long cur_ch_mask;	/* Used channels for an SDR FMT */
	u32 num_hw_ch;			/* Num of DT enabled channels */
	u32 num_cur_ch;			/* Num of used channels */
	u32 hwbuf_size;			/* Each DMA buffer size */
	u32 produced;			/* Buffers produced by sdr dev */
};

/* Allocate buffer context */
static int rcar_drif_alloc_bufctxt(struct rcar_drif_sdr *sdr)
{
	struct rcar_drif_hwbuf *bufctx;
	unsigned int i, idx;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		bufctx = kcalloc(num_hwbufs, sizeof(*bufctx), GFP_KERNEL);
		if (!bufctx)
			return -ENOMEM;

		for (idx = 0; idx < num_hwbufs; idx++)
			sdr->ch[i]->buf[idx] = bufctx + idx;
	}
	return 0;
}

/* Release buffer context */
static void rcar_drif_release_bufctxt(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		kfree(sdr->ch[i]->buf[0]);
		sdr->ch[i]->buf[0] = NULL;
	}
}

/* Release DMA channel */
static void rcar_drif_release_dmachannel(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask)
		if (sdr->ch[i]->dmach) {
			dma_release_channel(sdr->ch[i]->dmach);
			sdr->ch[i]->dmach = NULL;
		}
}

/* Allocate DMA channel */
static int rcar_drif_alloc_dmachannel(struct rcar_drif_sdr *sdr)
{
	struct dma_slave_config dma_cfg;
	unsigned int i;
	int ret = -ENODEV;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		struct rcar_drif *ch = sdr->ch[i];

		ch->dmach = dma_request_slave_channel(&ch->pdev->dev, "rx");
		if (!ch->dmach) {
			rdrif_err(sdr, "ch%u: dma channel req failed\n", i);
			goto dmach_error;
		}

		/* Configure slave */
		memset(&dma_cfg, 0, sizeof(dma_cfg));
		dma_cfg.src_addr = (phys_addr_t)(ch->start + RCAR_DRIF_SIRFDR);
		dma_cfg.dst_addr = 0;
		dma_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		ret = dmaengine_slave_config(ch->dmach, &dma_cfg);
		if (ret) {
			rdrif_err(sdr, "ch%u: dma slave config failed\n", i);
			goto dmach_error;
		}
	}
	return 0;

dmach_error:
	rcar_drif_release_dmachannel(sdr);
	return ret;
}

/* Release queued vb2 buffers */
static void rcar_drif_release_queued_bufs(struct rcar_drif_sdr *sdr,
					  enum vb2_buffer_state state)
{
	struct rcar_drif_frame_buf *fbuf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sdr->queued_bufs_lock, flags);
	list_for_each_entry_safe(fbuf, tmp, &sdr->queued_bufs, list) {
		list_del(&fbuf->list);
		vb2_buffer_done(&fbuf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&sdr->queued_bufs_lock, flags);
}

/* Set MDR defaults */
static inline void rcar_drif_set_mdr1(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	/* Set defaults for both internal channels */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		/* Refer MSIOF section in manual for this register setting */
		writel(RCAR_DRIF_SITMDR1_PCON,
		       sdr->ch[i]->base + RCAR_DRIF_SITMDR1);

		/* Setup MDR1 value */
		writel(sdr->mdr1, sdr->ch[i]->base + RCAR_DRIF_SIRMDR1);

		rdrif_dbg(2, sdr, "ch%u: mdr1 = 0x%08x",
			  i, readl(sdr->ch[i]->base + RCAR_DRIF_SIRMDR1));
	}
}

/* Extract bitlen and wdcnt from given word length */
static int rcar_drif_convert_wdlen(struct rcar_drif_sdr *sdr,
				   u32 wdlen, u32 *bitlen, u32 *wdcnt)
{
	unsigned int i, nr_wds;

	/* FIFO register size is 32 bits */
	for (i = 0; i < 32; i++) {
		nr_wds = wdlen % (32 - i);
		if (nr_wds == 0) {
			*bitlen = 32 - i;
			*wdcnt = wdlen / *bitlen;
			break;
		}
	}

	/* Sanity check range */
	if (i == 32 || !(*bitlen >= 8 && *bitlen <= 32) ||
	    !(*wdcnt >= 1 && *wdcnt <= 64)) {
		rdrif_err(sdr, "invalid wdlen %u configured\n", wdlen);
		return -EINVAL;
	}

	return 0;
}

/* Set DRIF receive format */
static int rcar_drif_set_format(struct rcar_drif_sdr *sdr)
{
	u32 bitlen, wdcnt, wdlen;
	unsigned int i;
	int ret = -EINVAL;

	wdlen = formats[sdr->fmt_idx].wdlen;
	rdrif_dbg(2, sdr, "setfmt: idx %u, wdlen %u, num_ch %u\n",
		  sdr->fmt_idx, wdlen, formats[sdr->fmt_idx].num_ch);

	/* Sanity check */
	if (formats[sdr->fmt_idx].num_ch > sdr->num_cur_ch) {
		rdrif_err(sdr, "fmt idx %u current ch %u mismatch\n",
			  sdr->fmt_idx, sdr->num_cur_ch);
		return ret;
	}

	/* Get bitlen & wdcnt from wdlen */
	ret = rcar_drif_convert_wdlen(sdr, wdlen, &bitlen, &wdcnt);
	if (ret)
		return ret;

	/* Setup group, bitlen & wdcnt */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		u32 mdr;

		/* Two groups */
		mdr = RCAR_DRIF_MDR_GRPCNT(2) | RCAR_DRIF_MDR_BITLEN(bitlen) |
		       RCAR_DRIF_MDR_WDCNT(wdcnt);
		writel(mdr, sdr->ch[i]->base + RCAR_DRIF_SIRMDR2);

		mdr = RCAR_DRIF_MDR_BITLEN(bitlen) | RCAR_DRIF_MDR_WDCNT(wdcnt);
		writel(mdr, sdr->ch[i]->base + RCAR_DRIF_SIRMDR3);

		rdrif_dbg(2, sdr, "ch%u: new mdr[2,3] = 0x%08x, 0x%08x\n",
			  i, readl(sdr->ch[i]->base + RCAR_DRIF_SIRMDR2),
			  readl(sdr->ch[i]->base + RCAR_DRIF_SIRMDR3));
	}
	return ret;
}

/* Release DMA buffers */
static void rcar_drif_release_buf(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		struct rcar_drif *ch = sdr->ch[i];

		/* First entry contains the dma buf ptr */
		if (ch->buf[0] && ch->buf[0]->addr) {
			dma_free_coherent(&ch->pdev->dev,
					  sdr->hwbuf_size * num_hwbufs,
					  ch->buf[0]->addr, ch->dma_handle);
			ch->buf[0]->addr = NULL;
		}
	}
}

/* Request DMA buffers */
static int rcar_drif_request_buf(struct rcar_drif_sdr *sdr)
{
	int ret = -ENOMEM;
	unsigned int i, j;
	void *addr;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		struct rcar_drif *ch = sdr->ch[i];

		/* Allocate DMA buffers */
		addr = dma_alloc_coherent(&ch->pdev->dev,
					  sdr->hwbuf_size * num_hwbufs,
					  &ch->dma_handle, GFP_KERNEL);
		if (!addr) {
			rdrif_err(sdr,
			"ch%u: dma alloc failed. num_hwbufs %u size %u\n",
			i, num_hwbufs, sdr->hwbuf_size);
			goto alloc_error;
		}

		/* Split the chunk and populate bufctxt */
		for (j = 0; j < num_hwbufs; j++) {
			ch->buf[j]->addr = addr + (j * sdr->hwbuf_size);
			ch->buf[j]->status = 0;
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
	struct rcar_drif_sdr *sdr = vb2_get_drv_priv(vq);

	/* Need at least 16 buffers */
	if (vq->num_buffers + *num_buffers < 16)
		*num_buffers = 16 - vq->num_buffers;

	*num_planes = 1;
	sizes[0] = PAGE_ALIGN(formats[sdr->fmt_idx].buffersize);

	rdrif_dbg(2, sdr, "num_bufs %d sizes[0] %d\n", *num_buffers, sizes[0]);
	return 0;
}

/* Enqueue buffer */
static void rcar_drif_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rcar_drif_sdr *sdr = vb2_get_drv_priv(vb->vb2_queue);
	struct rcar_drif_frame_buf *fbuf =
			container_of(vbuf, struct rcar_drif_frame_buf, vb);
	unsigned long flags;

	rdrif_dbg(2, sdr, "buf_queue idx %u\n", vb->index);
	spin_lock_irqsave(&sdr->queued_bufs_lock, flags);
	list_add_tail(&fbuf->list, &sdr->queued_bufs);
	spin_unlock_irqrestore(&sdr->queued_bufs_lock, flags);
}

/* Get a frame buf from list */
static struct rcar_drif_frame_buf *
rcar_drif_get_fbuf(struct rcar_drif_sdr *sdr)
{
	struct rcar_drif_frame_buf *fbuf;
	unsigned long flags;

	spin_lock_irqsave(&sdr->queued_bufs_lock, flags);
	fbuf = list_first_entry_or_null(&sdr->queued_bufs, struct
					rcar_drif_frame_buf, list);
	if (!fbuf) {
		/*
		 * App is late in enqueing buffers. Samples lost & there will
		 * be a gap in sequence number when app recovers
		 */
		rdrif_dbg(1, sdr, "\napp late: prod %u\n", sdr->produced);
		sdr->produced++; /* Increment the produced count anyway */
		spin_unlock_irqrestore(&sdr->queued_bufs_lock, flags);
		return NULL;
	}
	list_del(&fbuf->list);
	spin_unlock_irqrestore(&sdr->queued_bufs_lock, flags);

	return fbuf;
}

static inline bool rcar_drif_buf_pairs_done(struct rcar_drif_hwbuf *buf1,
					    struct rcar_drif_hwbuf *buf2)
{
	return (buf1->status & buf2->status & RCAR_DRIF_BUF_DONE);
}

/* Channel DMA complete */
static void rcar_drif_channel_complete(struct rcar_drif *ch, u32 idx)
{
	u32 str;

	ch->buf[idx]->status |= RCAR_DRIF_BUF_DONE;

	/* Check for DRIF errors */
	str = readl(ch->base + RCAR_DRIF_SISTR);
	if (unlikely(str & RCAR_DRIF_RFOVF)) {
		/* Writing the same clears it */
		writel(str, ch->base + RCAR_DRIF_SISTR);

		/* Overflow: some samples are lost */
		ch->buf[idx]->status |= RCAR_DRIF_BUF_OVERFLOW;
	}
}

/* Deliver buffer to user */
static void rcar_drif_deliver_buf(struct rcar_drif *ch)
{
	struct rcar_drif_sdr *sdr = ch->sdr;
	u32 idx = sdr->produced % num_hwbufs;
	struct rcar_drif_frame_buf *fbuf;
	bool overflow = false;

	rcar_drif_channel_complete(ch, idx);

	if (sdr->num_cur_ch == RCAR_DRIF_MAX_CHANNEL) {
		struct rcar_drif_hwbuf *bufi, *bufq;

		if (ch->num) {
			bufi = to_rcar_drif_buf_pair(sdr, ch->num, idx);
			bufq = ch->buf[idx];
		} else {
			bufi = ch->buf[idx];
			bufq = to_rcar_drif_buf_pair(sdr, ch->num, idx);
		}

		/* Check if both DMA buffers are done */
		if (!rcar_drif_buf_pairs_done(bufi, bufq))
			return;

		/* Clear buf done status */
		bufi->status &= ~RCAR_DRIF_BUF_DONE;
		bufq->status &= ~RCAR_DRIF_BUF_DONE;

		/* Get fbuf */
		fbuf = rcar_drif_get_fbuf(sdr);
		if (!fbuf)
			return;

		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0),
		       bufi->addr, sdr->hwbuf_size);
		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0) + sdr->hwbuf_size,
		       bufq->addr, sdr->hwbuf_size);

		if ((bufi->status | bufq->status) & RCAR_DRIF_BUF_OVERFLOW) {
			overflow = true;
			/* Clear the flag in status */
			bufi->status &= ~RCAR_DRIF_BUF_OVERFLOW;
			bufq->status &= ~RCAR_DRIF_BUF_OVERFLOW;
		}
	} else {
		struct rcar_drif_hwbuf *bufiq;

		/* Get fbuf */
		fbuf = rcar_drif_get_fbuf(sdr);
		if (!fbuf)
			return;

		bufiq = ch->buf[idx];

		memcpy(vb2_plane_vaddr(&fbuf->vb.vb2_buf, 0),
		       bufiq->addr, sdr->hwbuf_size);

		if (bufiq->status & RCAR_DRIF_BUF_OVERFLOW) {
			overflow = true;
			/* Clear the flag in status */
			bufiq->status &= ~RCAR_DRIF_BUF_OVERFLOW;
		}
	}

	rdrif_dbg(2, sdr, "ch%u: prod %u\n", ch->num, sdr->produced);

	fbuf->vb.field = V4L2_FIELD_NONE;
	fbuf->vb.sequence = sdr->produced++;
	fbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&fbuf->vb.vb2_buf, 0,
			      formats[sdr->fmt_idx].buffersize);

	/* Set error state on overflow */
	if (overflow)
		vb2_buffer_done(&fbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	else
		vb2_buffer_done(&fbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* DMA callback for each stage */
static void rcar_drif_dma_complete(void *dma_async_param)
{
	struct rcar_drif *ch = dma_async_param;
	struct rcar_drif_sdr *sdr = ch->sdr;

	mutex_lock(&sdr->vb_queue_mutex);

	/* DMA can be terminated while the callback was waiting on lock */
	if (!vb2_is_streaming(&sdr->vb_queue))
		goto stopped;

	rcar_drif_deliver_buf(ch);
stopped:
	mutex_unlock(&sdr->vb_queue_mutex);
}

static int rcar_drif_qbuf(struct rcar_drif *ch)
{
	struct rcar_drif_sdr *sdr = ch->sdr;
	dma_addr_t addr = ch->dma_handle;
	struct dma_async_tx_descriptor *rxd;
	dma_cookie_t cookie;
	int ret = -EIO;

	/* Setup cyclic DMA with given buffers */
	rxd = dmaengine_prep_dma_cyclic(ch->dmach, addr,
					sdr->hwbuf_size * num_hwbufs,
					sdr->hwbuf_size, DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rxd) {
		rdrif_err(sdr, "ch%u: prep dma cyclic failed\n", ch->num);
		return ret;
	}

	/* Submit descriptor */
	rxd->callback = rcar_drif_dma_complete;
	rxd->callback_param = ch;
	cookie = dmaengine_submit(rxd);
	if (dma_submit_error(cookie)) {
		rdrif_err(sdr, "ch%u: dma submit failed\n", ch->num);
		return ret;
	}

	dma_async_issue_pending(ch->dmach);
	return 0;
}

/* Enable reception */
static int rcar_drif_enable_rx(struct rcar_drif_sdr *sdr)
{
	unsigned int i;
	u32 ctr;
	int ret;

	/*
	 * When both internal channels are enabled, they can be synchronized
	 * only by the master
	 */

	/* Enable receive */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ctr = readl(sdr->ch[i]->base + RCAR_DRIF_SICTR);
		ctr |= (RCAR_DRIF_SICTR_RX_RISING_EDGE |
			 RCAR_DRIF_SICTR_RX_EN);
		writel(ctr, sdr->ch[i]->base + RCAR_DRIF_SICTR);
	}

	/* Check receive enabled */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ret = readl_poll_timeout(sdr->ch[i]->base + RCAR_DRIF_SICTR,
					 ctr, ctr & RCAR_DRIF_SICTR_RX_EN,
					 2, 500000);
		if (ret) {
			rdrif_err(sdr, "ch%u: rx en failed. ctr 0x%08x\n",
				  i, readl(sdr->ch[i]->base + RCAR_DRIF_SICTR));
			break;
		}
	}
	return ret;
}

/* Disable reception */
static void rcar_drif_disable_rx(struct rcar_drif_sdr *sdr)
{
	unsigned int i;
	u32 ctr;
	int ret;

	/* Disable receive */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ctr = readl(sdr->ch[i]->base + RCAR_DRIF_SICTR);
		ctr &= ~RCAR_DRIF_SICTR_RX_EN;
		writel(ctr, sdr->ch[i]->base + RCAR_DRIF_SICTR);
	}

	/* Check receive disabled */
	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ret = readl_poll_timeout(sdr->ch[i]->base + RCAR_DRIF_SICTR,
					 ctr, !(ctr & RCAR_DRIF_SICTR_RX_EN),
					 2, 500000);
		if (ret)
			dev_warn(&sdr->vdev->dev,
			"ch%u: failed to disable rx. ctr 0x%08x\n",
			i, readl(sdr->ch[i]->base + RCAR_DRIF_SICTR));
	}
}

/* Start channel */
static int rcar_drif_start_channel(struct rcar_drif *ch)
{
	struct rcar_drif_sdr *sdr = ch->sdr;
	u32 ctr, str;
	int ret;

	/* Reset receive */
	writel(RCAR_DRIF_SICTR_RESET, ch->base + RCAR_DRIF_SICTR);
	ret = readl_poll_timeout(ch->base + RCAR_DRIF_SICTR,
					 ctr, !(ctr & RCAR_DRIF_SICTR_RESET),
					 2, 500000);
	if (ret) {
		rdrif_err(sdr, "ch%u: failed to reset rx. ctr 0x%08x\n",
			  ch->num, readl(ch->base + RCAR_DRIF_SICTR));
		return ret;
	}

	/* Queue buffers for DMA */
	ret = rcar_drif_qbuf(ch);
	if (ret)
		return ret;

	/* Clear status register flags */
	str = RCAR_DRIF_RFFUL | RCAR_DRIF_REOF | RCAR_DRIF_RFSERR |
		RCAR_DRIF_RFUDF | RCAR_DRIF_RFOVF;
	writel(str, ch->base + RCAR_DRIF_SISTR);

	/* Enable DMA receive interrupt */
	writel(0x00009000, ch->base + RCAR_DRIF_SIIER);

	return ret;
}

/* Start receive operation */
static int rcar_drif_start(struct rcar_drif_sdr *sdr)
{
	unsigned int i;
	int ret;

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ret = rcar_drif_start_channel(sdr->ch[i]);
		if (ret)
			goto start_error;
	}

	sdr->produced = 0;
	ret = rcar_drif_enable_rx(sdr);
start_error:
	return ret;
}

/* Stop channel */
static void rcar_drif_stop_channel(struct rcar_drif *ch)
{
	struct rcar_drif_sdr *sdr = ch->sdr;
	int ret, retries = 3;

	/* Disable DMA receive interrupt */
	writel(0x00000000, ch->base + RCAR_DRIF_SIIER);

	do {
		/* Terminate all DMA transfers */
		ret = dmaengine_terminate_sync(ch->dmach);
		if (!ret)
			break;
		rdrif_dbg(2, sdr, "stop retry\n");
	} while (--retries);

	WARN_ON(!retries);
}

/* Stop receive operation */
static void rcar_drif_stop(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	/* Disable Rx */
	rcar_drif_disable_rx(sdr);

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask)
		rcar_drif_stop_channel(sdr->ch[i]);
}

/* Start streaming */
static int rcar_drif_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rcar_drif_sdr *sdr = vb2_get_drv_priv(vq);
	unsigned int i, j;
	int ret;

	mutex_lock(&sdr->v4l2_mutex);

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask) {
		ret = clk_prepare_enable(sdr->ch[i]->clkp);
		if (ret)
			goto start_error;
	}

	/* Set default MDRx settings */
	rcar_drif_set_mdr1(sdr);

	/* Set new format */
	ret = rcar_drif_set_format(sdr);
	if (ret)
		goto start_error;

	if (sdr->num_cur_ch == RCAR_DRIF_MAX_CHANNEL)
		sdr->hwbuf_size =
		formats[sdr->fmt_idx].buffersize / RCAR_DRIF_MAX_CHANNEL;
	else
		sdr->hwbuf_size = formats[sdr->fmt_idx].buffersize;

	rdrif_dbg(1, sdr, "num_hwbufs %u, hwbuf_size %u\n",
		num_hwbufs, sdr->hwbuf_size);

	/* Alloc DMA channel */
	ret = rcar_drif_alloc_dmachannel(sdr);
	if (ret)
		goto start_error;

	/* Alloc buf context */
	ret = rcar_drif_alloc_bufctxt(sdr);
	if (ret)
		goto start_error;

	/* Request buffers */
	ret = rcar_drif_request_buf(sdr);
	if (ret)
		goto start_error;

	/* Start Rx */
	ret = rcar_drif_start(sdr);
	if (ret)
		goto start_error;

	mutex_unlock(&sdr->v4l2_mutex);
	rdrif_dbg(1, sdr, "started\n");
	return ret;

start_error:
	rcar_drif_release_queued_bufs(sdr, VB2_BUF_STATE_QUEUED);
	rcar_drif_release_buf(sdr);
	rcar_drif_release_bufctxt(sdr);
	rcar_drif_release_dmachannel(sdr);
	for (j = 0; j < i; j++)
		clk_disable_unprepare(sdr->ch[j]->clkp);

	mutex_unlock(&sdr->v4l2_mutex);
	return ret;
}

/* Stop streaming */
static void rcar_drif_stop_streaming(struct vb2_queue *vq)
{
	struct rcar_drif_sdr *sdr = vb2_get_drv_priv(vq);
	unsigned int i;

	mutex_lock(&sdr->v4l2_mutex);

	/* Stop hardware streaming */
	rcar_drif_stop(sdr);

	/* Return all queued buffers to vb2 */
	rcar_drif_release_queued_bufs(sdr, VB2_BUF_STATE_ERROR);

	/* Release buf & buf context */
	rcar_drif_release_buf(sdr);
	rcar_drif_release_bufctxt(sdr);

	/* Release DMA channel resources */
	rcar_drif_release_dmachannel(sdr);

	for_each_rcar_drif_channel(i, &sdr->cur_ch_mask)
		clk_disable_unprepare(sdr->ch[i]->clkp);

	mutex_unlock(&sdr->v4l2_mutex);
	rdrif_dbg(1, sdr, "stopped: prod %u\n", sdr->produced);
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
	struct rcar_drif_sdr *sdr = video_drvdata(file);

	strlcpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strlcpy(cap->card, sdr->vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 sdr->vdev->name);
	return 0;
}

static int rcar_drif_set_default_format(struct rcar_drif_sdr *sdr)
{
	unsigned int i;

	for (i = 0; i < NUM_FORMATS; i++) {
		/* Matching fmt based on required channels is set as default */
		if (sdr->num_hw_ch == formats[i].num_ch) {
			sdr->fmt_idx = i;
			sdr->cur_ch_mask = sdr->hw_ch_mask;
			sdr->num_cur_ch = sdr->num_hw_ch;
			dev_dbg(sdr->dev, "default fmt[%u]: mask %lu num %u\n",
				i, sdr->cur_ch_mask, sdr->num_cur_ch);
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

	f->pixelformat = formats[f->index].pixelformat;
	return 0;
}

static int rcar_drif_g_fmt_sdr_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);

	f->fmt.sdr.pixelformat = formats[sdr->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[sdr->fmt_idx].buffersize;
	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	return 0;
}

static int rcar_drif_s_fmt_sdr_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct vb2_queue *q = &sdr->vb_queue;
	unsigned int i;

	if (vb2_is_busy(q))
		return -EBUSY;

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].pixelformat == f->fmt.sdr.pixelformat) {
			sdr->fmt_idx  = i;
			f->fmt.sdr.buffersize = formats[i].buffersize;

			/*
			 * If a format demands one channel only out of two
			 * enabled channels, pick the 0th channel.
			 */
			if (formats[i].num_ch < sdr->num_hw_ch) {
				sdr->cur_ch_mask = BIT(0);
				sdr->num_cur_ch = formats[i].num_ch;
			} else {
				sdr->cur_ch_mask = sdr->hw_ch_mask;
				sdr->num_cur_ch = sdr->num_hw_ch;
			}

			rdrif_dbg(1, sdr, "cur: idx %u mask %lu num %u\n",
				  i, sdr->cur_ch_mask, sdr->num_cur_ch);
			return 0;
		}
	}

	if (rcar_drif_set_default_format(sdr)) {
		rdrif_err(sdr, "cannot set default format\n");
		return -EINVAL;
	}

	f->fmt.sdr.pixelformat = formats[sdr->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[sdr->fmt_idx].buffersize;
	return 0;
}

static int rcar_drif_try_fmt_sdr_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	unsigned int i;

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	for (i = 0; i < NUM_FORMATS; i++) {
		if (formats[i].pixelformat == f->fmt.sdr.pixelformat) {
			f->fmt.sdr.buffersize = formats[i].buffersize;
			return 0;
		}
	}

	f->fmt.sdr.pixelformat = formats[sdr->fmt_idx].pixelformat;
	f->fmt.sdr.buffersize = formats[sdr->fmt_idx].buffersize;
	return 0;
}

/* Tuner subdev ioctls */
static int rcar_drif_enum_freq_bands(struct file *file, void *priv,
				     struct v4l2_frequency_band *band)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct v4l2_subdev *sd;
	int ret = 0;

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_subdev_call(sd, tuner, enum_freq_bands, band);
		if (ret)
			break;
	}
	return ret;
}

static int rcar_drif_g_frequency(struct file *file, void *priv,
				 struct v4l2_frequency *f)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct v4l2_subdev *sd;
	int ret = 0;

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_subdev_call(sd, tuner, g_frequency, f);
		if (ret)
			break;
	}
	return ret;
}

static int rcar_drif_s_frequency(struct file *file, void *priv,
				 const struct v4l2_frequency *f)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct v4l2_subdev *sd;
	int ret = 0;

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_subdev_call(sd, tuner, s_frequency, f);
		if (ret)
			break;
	}
	return ret;
}

static int rcar_drif_g_tuner(struct file *file, void *priv,
			     struct v4l2_tuner *vt)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct v4l2_subdev *sd;
	int ret = 0;

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_subdev_call(sd, tuner, g_tuner, vt);
		if (ret)
			break;
	}
	return ret;
}

static int rcar_drif_s_tuner(struct file *file, void *priv,
			     const struct v4l2_tuner *vt)
{
	struct rcar_drif_sdr *sdr = video_drvdata(file);
	struct v4l2_subdev *sd;
	int ret = 0;

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_subdev_call(sd, tuner, s_tuner, vt);
		if (ret)
			break;
	}
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

static int rcar_drif_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct rcar_drif_sdr *sdr =
		container_of(notifier, struct rcar_drif_sdr, notifier);

	/* Nothing to do at this point */
	rdrif_dbg(2, sdr, "bound asd: %s\n", asd->match.of.node->name);
	return 0;
}

/* Sub-device registered notification callback */
static int rcar_drif_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct rcar_drif_sdr *sdr =
		container_of(notifier, struct rcar_drif_sdr, notifier);
	struct v4l2_subdev *sd;
	int ret;

	sdr->v4l2_dev.ctrl_handler = &sdr->ctrl_hdl;

	ret = v4l2_device_register_subdev_nodes(&sdr->v4l2_dev);
	if (ret) {
		rdrif_err(sdr, "failed register subdev nodes ret %d\n", ret);
		return ret;
	}

	v4l2_device_for_each_subdev(sd, &sdr->v4l2_dev) {
		ret = v4l2_ctrl_add_handler(sdr->v4l2_dev.ctrl_handler,
					    sd->ctrl_handler, NULL);
		if (ret) {
			rdrif_err(sdr, "failed ctrl add hdlr ret %d\n", ret);
			return ret;
		}
	}
	rdrif_dbg(2, sdr, "notify complete\n");
	return 0;
}

/* Parse sub-devs (tuner) to find a matching device */
static int rcar_drif_parse_subdevs(struct device *dev,
				   struct v4l2_async_notifier *notifier)
{
	struct device_node *node;
	struct rcar_drif_async_subdev *rsd;

	notifier->subdevs = devm_kzalloc(dev, sizeof(*notifier->subdevs),
					 GFP_KERNEL);
	if (!notifier->subdevs)
		return -ENOMEM;

	node = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!node)
		return 0;


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
	return 0;
}

/* SIRMDR1 configuration */
static int rcar_drif_validate_syncmd(struct rcar_drif_sdr *sdr, u32 val)
{
	if (val > 1) {
		dev_err(sdr->dev, "invalid syncmd %u\n", val);
		return -EINVAL;
	}

	sdr->mdr1 &= ~(3 << 28);	/* Clear current settings */
	if (val == 0)
		sdr->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCMD_FRAME;
	else
		sdr->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCMD_LR;
	return 0;
}

/* Get the dtdl or syncdl bits */
static u32 rcar_drif_get_dtdl_or_syncdl_bits(u32 dtdl_or_syncdl)
{
	/*
	 * DTDL/SYNCDL bit	: dtdl/syncdl
	 * b'000		: 0
	 * b'001		: 2
	 * b'010		: 4
	 * b'011 (SYNCDL only)	: 6
	 * b'101		: 1
	 * b'110		: 3
	 */
	if (dtdl_or_syncdl % 2)
		return dtdl_or_syncdl / 2 + 5;

	return dtdl_or_syncdl / 2;
}

/* Validate DT properties dtdl and syncdl */
static int rcar_drif_validate_dtdl_syncdl(struct rcar_drif_sdr *sdr)
{
	struct device_node *np = sdr->dev->of_node;
	u32 dtdl = 2, syncdl = 0;	/* delay in 0.5 clock cycle units */

	sdr->mdr1 |= RCAR_DRIF_SIRMDR1_DTDL_1 | RCAR_DRIF_SIRMDR1_SYNCDL_0;
	of_property_read_u32(np, "renesas,dtdl", &dtdl);
	of_property_read_u32(np, "renesas,syncdl", &syncdl);

	/* Sanity checks */
	if (dtdl > 4 || syncdl > 6) {
		dev_err(sdr->dev, "invalid dtdl %u/syncdl %u\n", dtdl, syncdl);
		return -EINVAL;
	}
	if ((dtdl + syncdl) % 2) {
		dev_err(sdr->dev, "sum of dtdl %u & syncdl %u not OK\n",
			dtdl, syncdl);
		return -EINVAL;
	}
	sdr->mdr1 &= ~(7 << 20) & ~(7 << 16);	/* Clear current settings */
	sdr->mdr1 |= rcar_drif_get_dtdl_or_syncdl_bits(dtdl) << 20;
	sdr->mdr1 |= rcar_drif_get_dtdl_or_syncdl_bits(syncdl) << 16;
	return 0;
}

static int rcar_drif_parse_properties(struct rcar_drif_sdr *sdr)
{
	struct device_node *np = sdr->dev->of_node;
	u32 val;
	int ret;

	/* Set the defaults and check for overrides */
	sdr->mdr1 = RCAR_DRIF_SIRMDR1_SYNCMD_LR;
	if (!of_property_read_u32(np, "renesas,syncmd", &val)) {
		ret = rcar_drif_validate_syncmd(sdr, val);
		if (ret)
			return ret;
	}

	if (of_find_property(np, "renesas,lsb-first", NULL))
		sdr->mdr1 |= RCAR_DRIF_SIRMDR1_LSB_FIRST;
	else
		sdr->mdr1 |= RCAR_DRIF_SIRMDR1_MSB_FIRST;

	sdr->mdr1 |= RCAR_DRIF_SIRMDR1_SYNCAC_POL_HIGH;
	if (!of_property_read_u32(np, "renesas,syncac-active", &val))
		sdr->mdr1 |= val ? RCAR_DRIF_SIRMDR1_SYNCAC_POL_HIGH :
			RCAR_DRIF_SIRMDR1_SYNCAC_POL_LOW;

	return rcar_drif_validate_dtdl_syncdl(sdr);
}

static bool rcar_drif_primary_bond(struct platform_device *pdev)
{
	if (of_find_property(pdev->dev.of_node, "renesas,primary-bond", NULL))
		return true;

	return false;
}

static struct platform_device *rcar_drif_enabled_bond(struct platform_device *p)
{
	struct device_node *np;

	np = of_parse_phandle(p->dev.of_node, "renesas,bonding", 0);
	if (np && of_device_is_available(np))
		return of_find_device_by_node(np);

	return NULL;
}

static int rcar_drif_channel_probe(struct platform_device *pdev)
{
	struct rcar_drif *ch;
	struct resource	*res;
	void __iomem *base;
	struct clk *clkp;
	int ret;

	/* Peripheral clock */
	clkp = devm_clk_get(&pdev->dev, "fck");
	if (IS_ERR(clkp)) {
		ret = PTR_ERR(clkp);
		dev_err(&pdev->dev, "clk get failed (%d)\n", ret);
		return ret;
	}

	/* Register map */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		dev_err(&pdev->dev, "ioremap failed (%d)\n", ret);
		return ret;
	}

	/* Reserve memory for enabled channel */
	ch = devm_kzalloc(&pdev->dev, sizeof(*ch), GFP_KERNEL);
	if (!ch) {
		ret = PTR_ERR(ch);
		dev_err(&pdev->dev, "failed alloc channel\n");
		return ret;
	}
	ch->pdev = pdev;
	ch->clkp = clkp;
	ch->base = base;
	ch->start = res->start;
	platform_set_drvdata(pdev, ch);
	return 0;
}

static int rcar_drif_probe(struct platform_device *pdev)
{
	struct rcar_drif *ch, *b_ch = NULL;
	struct platform_device *b_pdev;
	struct rcar_drif_sdr *sdr;
	int ret;

	/* Probe internal channel */
	ret = rcar_drif_channel_probe(pdev);
	if (ret)
		return ret;

	/* Check if both channels of the bond are enabled */
	b_pdev = rcar_drif_enabled_bond(pdev);
	if (b_pdev) {
		/* Check if current channel acting as primary-bond */
		if (!rcar_drif_primary_bond(pdev)) {
			dev_notice(&pdev->dev, "probed\n");
			return 0;
		}

		/* Check if the other device is probed */
		b_ch = platform_get_drvdata(b_pdev);
		if (!b_ch) {
			dev_info(&pdev->dev, "defer probe\n");
			return -EPROBE_DEFER;
		}
		/* Set the other channel number */
		b_ch->num = 1;
	}

	/* Channel acting as SDR instance */
	ch = platform_get_drvdata(pdev);
	ch->acting_sdr = true;

	/* Reserve memory for SDR structure */
	sdr = devm_kzalloc(&pdev->dev, sizeof(*sdr), GFP_KERNEL);
	if (!sdr) {
		ret = PTR_ERR(sdr);
		dev_err(&pdev->dev, "failed alloc drif context\n");
		return ret;
	}
	sdr->dev = &pdev->dev;
	sdr->hw_ch_mask = BIT(ch->num);

	/* Establish links between SDR and channel(s) */
	ch->sdr = sdr;
	sdr->ch[ch->num] = ch;
	if (b_ch) {
		sdr->ch[b_ch->num] = b_ch;
		b_ch->sdr = sdr;
		sdr->hw_ch_mask |= BIT(b_ch->num);
	}
	sdr->num_hw_ch = hweight_long(sdr->hw_ch_mask);

	/* Parse device tree properties */
	ret = rcar_drif_parse_properties(sdr);
	if (ret)
		return ret;

	dev_dbg(sdr->dev, "parsed mdr1 0x%08x\n", sdr->mdr1);

	/* Validate any supported format for enabled channels */
	ret = rcar_drif_set_default_format(sdr);
	if (ret) {
		dev_err(sdr->dev, "failed to set default format\n");
		return ret;
	}

	/* Set defaults */
	sdr->hwbuf_size = RCAR_DRIF_DEFAULT_HWBUF_SIZE;

	mutex_init(&sdr->v4l2_mutex);
	mutex_init(&sdr->vb_queue_mutex);
	spin_lock_init(&sdr->queued_bufs_lock);
	INIT_LIST_HEAD(&sdr->queued_bufs);

	/* Init videobuf2 queue structure */
	sdr->vb_queue.type = V4L2_BUF_TYPE_SDR_CAPTURE;
	sdr->vb_queue.io_modes = VB2_READ | VB2_MMAP | VB2_DMABUF;
	sdr->vb_queue.drv_priv = sdr;
	sdr->vb_queue.buf_struct_size = sizeof(struct rcar_drif_frame_buf);
	sdr->vb_queue.ops = &rcar_drif_vb2_ops;
	sdr->vb_queue.mem_ops = &vb2_vmalloc_memops;
	sdr->vb_queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	/* Init videobuf2 queue */
	ret = vb2_queue_init(&sdr->vb_queue);
	if (ret) {
		dev_err(sdr->dev, "could not initialize vb2 queue\n");
		return ret;
	}

	/* Register the v4l2_device */
	ret = v4l2_device_register(&pdev->dev, &sdr->v4l2_dev);
	if (ret) {
		dev_err(sdr->dev, "failed v4l2_device_register (%d)\n", ret);
		return ret;
	}

	/*
	 * Parse subdevs after v4l2_device_register because if the subdev
	 * is already probed, bound and complete will be called immediately
	 */
	ret = rcar_drif_parse_subdevs(&pdev->dev, &sdr->notifier);
	if (ret)
		goto err_unreg_v4l2;

	sdr->notifier.bound = rcar_drif_notify_bound;
	sdr->notifier.complete = rcar_drif_notify_complete;

	v4l2_ctrl_handler_init(&sdr->ctrl_hdl, 10);

	/* Register notifier */
	ret = v4l2_async_notifier_register(&sdr->v4l2_dev, &sdr->notifier);
	if (ret < 0) {
		dev_err(sdr->dev, "notifier registration failed (%d)\n", ret);
		goto err_unreg_v4l2;
	}

	/* Init video_device structure */
	sdr->vdev = video_device_alloc();
	if (!sdr->vdev) {
		ret = -ENOMEM;
		goto err_unreg_notif;
	}
	snprintf(sdr->vdev->name, sizeof(sdr->vdev->name), "R-Car DRIF");
	sdr->vdev->fops = &rcar_drif_fops;
	sdr->vdev->ioctl_ops = &rcar_drif_ioctl_ops;
	sdr->vdev->release = video_device_release;
	sdr->vdev->lock = &sdr->v4l2_mutex;
	sdr->vdev->queue = &sdr->vb_queue;
	sdr->vdev->queue->lock = &sdr->vb_queue_mutex;
	sdr->vdev->ctrl_handler = &sdr->ctrl_hdl;
	sdr->vdev->v4l2_dev = &sdr->v4l2_dev;
	sdr->vdev->device_caps = V4L2_CAP_SDR_CAPTURE | V4L2_CAP_TUNER |
		V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	video_set_drvdata(sdr->vdev, sdr);

	/* Register V4L2 SDR device */
	ret = video_register_device(sdr->vdev, VFL_TYPE_SDR, -1);
	if (ret) {
		dev_err(sdr->dev, "failed video_register_device (%d)\n", ret);
		goto err_unreg_notif;
	}

	dev_notice(sdr->dev, "probed\n");
	return 0;

err_unreg_notif:
	video_device_release(sdr->vdev);
	v4l2_async_notifier_unregister(&sdr->notifier);
err_unreg_v4l2:
	v4l2_device_unregister(&sdr->v4l2_dev);
	return ret;
}

static int rcar_drif_remove(struct platform_device *pdev)
{
	struct rcar_drif *ch = platform_get_drvdata(pdev);
	struct rcar_drif_sdr *sdr = ch->sdr;

	if (!ch->acting_sdr) {
		/* Nothing to do */
		dev_notice(&pdev->dev, "removed\n");
		return 0;
	}

	/* SDR instance */
	v4l2_ctrl_handler_free(sdr->v4l2_dev.ctrl_handler);
	v4l2_async_notifier_unregister(&sdr->notifier);
	v4l2_device_unregister(&sdr->v4l2_dev);
	video_unregister_device(sdr->vdev);
	dev_notice(&pdev->dev, "removed\n");
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
