// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek 8250 DMA driver.
 *
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Long Cheng <long.cheng@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/pm_runtime.h>
#include <linux/iopoll.h>

#include "../virt-dma.h"

#define MTK_APDMA_DEFAULT_REQUESTS	127
#define MTK_APDMA_CHANNELS		(CONFIG_SERIAL_8250_NR_UARTS * 2)

#define VFF_EN_B		BIT(0)
#define VFF_STOP_B		BIT(0)
#define VFF_FLUSH_B		BIT(0)
#define VFF_4G_SUPPORT_B	BIT(0)
#define VFF_RX_INT_EN0_B	BIT(0)	/*rx valid size >=  vff thre*/
#define VFF_RX_INT_EN1_B	BIT(1)
#define VFF_TX_INT_EN_B		BIT(0)	/*tx left size >= vff thre*/
#define VFF_WARM_RST_B		BIT(0)
#define VFF_RX_INT_FLAG_CLR_B	(BIT(0) | BIT(1))
#define VFF_TX_INT_FLAG_CLR_B	0
#define VFF_STOP_CLR_B		0
#define VFF_FLUSH_CLR_B		0
#define VFF_INT_EN_CLR_B	0
#define VFF_4G_SUPPORT_CLR_B	0

/* interrupt trigger level for tx */
#define VFF_TX_THRE(n)		((n) * 7 / 8)
/* interrupt trigger level for rx */
#define VFF_RX_THRE(n)		((n) * 3 / 4)

#define MTK_DMA_RING_SIZE	0xffffU
/* invert this bit when wrap ring head again*/
#define MTK_DMA_RING_WRAP	0x10000U

#define VFF_INT_FLAG		0x00
#define VFF_INT_EN		0x04
#define VFF_EN			0x08
#define VFF_RST			0x0c
#define VFF_STOP		0x10
#define VFF_FLUSH		0x14
#define VFF_ADDR		0x1c
#define VFF_LEN			0x24
#define VFF_THRE		0x28
#define VFF_WPT			0x2c
#define VFF_RPT			0x30
/*TX: the buffer size HW can read. RX: the buffer size SW can read.*/
#define VFF_VALID_SIZE		0x3c
/*TX: the buffer size SW can write. RX: the buffer size HW can write.*/
#define VFF_LEFT_SIZE		0x40
#define VFF_DEBUG_STATUS	0x50
#define VFF_4G_SUPPORT		0x54

struct mtk_dmadev {
	struct dma_device ddev;
	void __iomem *mem_base[MTK_APDMA_CHANNELS];
	spinlock_t lock; /* dma dev lock */
	struct tasklet_struct task;
	struct list_head pending;
	struct clk *clk;
	unsigned int dma_requests;
	bool support_33bits;
	unsigned int dma_irq[MTK_APDMA_CHANNELS];
	struct mtk_chan *ch[MTK_APDMA_CHANNELS];
};

struct mtk_chan {
	struct virt_dma_chan vc;
	struct list_head node;
	struct dma_slave_config	cfg;
	void __iomem *base;
	struct mtk_dma_desc *desc;

	bool stop;
	bool requested;

	unsigned int rx_status;
};

struct mtk_dma_sg {
	dma_addr_t addr;
	unsigned int en;		/* number of elements (24-bit) */
	unsigned int fn;		/* number of frames (16-bit) */
};

struct mtk_dma_desc {
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int sglen;
	struct mtk_dma_sg sg[0];

	unsigned int len;
};

static inline struct mtk_dmadev *to_mtk_dma_dev(struct dma_device *d)
{
	return container_of(d, struct mtk_dmadev, ddev);
}

static inline struct mtk_chan *to_mtk_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct mtk_chan, vc.chan);
}

static inline struct mtk_dma_desc *to_mtk_dma_desc
	(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct mtk_dma_desc, vd.tx);
}

static void mtk_dma_chan_write(struct mtk_chan *c,
			       unsigned int reg, unsigned int val)
{
	writel(val, c->base + reg);
}

static unsigned int mtk_dma_chan_read(struct mtk_chan *c, unsigned int reg)
{
	return readl(c->base + reg);
}

static void mtk_dma_desc_free(struct virt_dma_desc *vd)
{
	struct dma_chan *chan = vd->tx.chan;
	struct mtk_chan *c = to_mtk_dma_chan(chan);

	kfree(c->desc);
	c->desc = NULL;
}

static int mtk_dma_clk_enable(struct mtk_dmadev *mtkd)
{
	int ret;

	ret = clk_prepare_enable(mtkd->clk);
	if (ret) {
		dev_err(mtkd->ddev.dev, "Couldn't enable the clock\n");
		return ret;
	}

	return 0;
}

static void mtk_dma_clk_disable(struct mtk_dmadev *mtkd)
{
	clk_disable_unprepare(mtkd->clk);
}

static void mtk_dma_tx_flush(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);

	if (mtk_dma_chan_read(c, VFF_FLUSH) == 0U)
		mtk_dma_chan_write(c, VFF_FLUSH, VFF_FLUSH_B);
}

static void mtk_dma_tx_write(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	unsigned int txcount = c->desc->len;
	unsigned int len, send, left, wpt, wrap;

	len = mtk_dma_chan_read(c, VFF_LEN);

	while ((left = mtk_dma_chan_read(c, VFF_LEFT_SIZE)) > 0U) {
		if (c->desc->len == 0U)
			break;
		send = min_t(unsigned int, left, c->desc->len);
		wpt = mtk_dma_chan_read(c, VFF_WPT);
		wrap = wpt & MTK_DMA_RING_WRAP ? 0U : MTK_DMA_RING_WRAP;

		if ((wpt & (len - 1U)) + send < len)
			mtk_dma_chan_write(c, VFF_WPT, wpt + send);
		else
			mtk_dma_chan_write(c, VFF_WPT,
					   ((wpt + send) & (len - 1U))
					   | wrap);

		c->desc->len -= send;
	}

	if (txcount != c->desc->len) {
		mtk_dma_chan_write(c, VFF_INT_EN, VFF_TX_INT_EN_B);
		mtk_dma_tx_flush(chan);
	}
}

static void mtk_dma_start_tx(struct mtk_chan *c)
{
	if (mtk_dma_chan_read(c, VFF_LEFT_SIZE) == 0U)
		mtk_dma_chan_write(c, VFF_INT_EN, VFF_TX_INT_EN_B);
	else
		mtk_dma_tx_write(&c->vc.chan);

	c->stop = false;
}

static void mtk_dma_get_rx_size(struct mtk_chan *c)
{
	unsigned int rx_size = mtk_dma_chan_read(c, VFF_LEN);
	unsigned int rdptr, wrptr, wrreg, rdreg, count;

	rdreg = mtk_dma_chan_read(c, VFF_RPT);
	wrreg = mtk_dma_chan_read(c, VFF_WPT);
	rdptr = rdreg & MTK_DMA_RING_SIZE;
	wrptr = wrreg & MTK_DMA_RING_SIZE;
	count = ((rdreg ^ wrreg) & MTK_DMA_RING_WRAP) ?
			(wrptr + rx_size - rdptr) : (wrptr - rdptr);

	c->rx_status = count;

	mtk_dma_chan_write(c, VFF_RPT, wrreg);
}

static void mtk_dma_start_rx(struct mtk_chan *c)
{
	struct dma_chan *chan = &c->vc.chan;
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(chan->device);
	struct mtk_dma_desc *d = c->desc;

	if (mtk_dma_chan_read(c, VFF_VALID_SIZE) == 0U)
		return;

	if (d && vchan_next_desc(&c->vc)) {
		mtk_dma_get_rx_size(c);
		list_del(&d->vd.node);
		vchan_cookie_complete(&d->vd);
	} else {
		spin_lock(&mtkd->lock);
		if (list_empty(&mtkd->pending))
			list_add_tail(&c->node, &mtkd->pending);
		spin_unlock(&mtkd->lock);
		tasklet_schedule(&mtkd->task);
	}
}

static void mtk_dma_reset(struct mtk_chan *c)
{
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(c->vc.chan.device);
	u32 status;
	int ret;

	mtk_dma_chan_write(c, VFF_ADDR, 0);
	mtk_dma_chan_write(c, VFF_THRE, 0);
	mtk_dma_chan_write(c, VFF_LEN, 0);
	mtk_dma_chan_write(c, VFF_RST, VFF_WARM_RST_B);

	ret = readx_poll_timeout(readl,
				 c->base + VFF_EN,
				 status, status == 0, 10, 100);
	if (ret) {
		dev_err(c->vc.chan.device->dev,
				"dma reset: fail, timeout\n");
		return;
	}

	if (c->cfg.direction == DMA_DEV_TO_MEM)
		mtk_dma_chan_write(c, VFF_RPT, 0);
	else if (c->cfg.direction == DMA_MEM_TO_DEV)
		mtk_dma_chan_write(c, VFF_WPT, 0);

	if (mtkd->support_33bits)
		mtk_dma_chan_write(c, VFF_4G_SUPPORT, VFF_4G_SUPPORT_CLR_B);
}

static void mtk_dma_stop(struct mtk_chan *c)
{
	u32 status;
	int ret;

	mtk_dma_chan_write(c, VFF_FLUSH, VFF_FLUSH_CLR_B);
	/* Wait for flush */
	ret = readx_poll_timeout(readl,
				 c->base + VFF_FLUSH,
				 status,
				 (status & VFF_FLUSH_B) != VFF_FLUSH_B,
				 10, 100);
	if (ret)
		dev_err(c->vc.chan.device->dev,
			"dma stop: polling FLUSH fail, DEBUG=0x%x\n",
			mtk_dma_chan_read(c, VFF_DEBUG_STATUS));

	/*set stop as 1 -> wait until en is 0 -> set stop as 0*/
	mtk_dma_chan_write(c, VFF_STOP, VFF_STOP_B);
	ret = readx_poll_timeout(readl,
				 c->base + VFF_EN,
				 status, status == 0, 10, 100);
	if (ret)
		dev_err(c->vc.chan.device->dev,
			"dma stop: polling VFF_EN fail, DEBUG=0x%x\n",
			mtk_dma_chan_read(c, VFF_DEBUG_STATUS));

	mtk_dma_chan_write(c, VFF_STOP, VFF_STOP_CLR_B);
	mtk_dma_chan_write(c, VFF_INT_EN, VFF_INT_EN_CLR_B);

	if (c->cfg.direction == DMA_DEV_TO_MEM)
		mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);
	else
		mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);

	c->stop = true;
}

/*
 * This callback schedules all pending channels. We could be more
 * clever here by postponing allocation of the real DMA channels to
 * this point, and freeing them when our virtual channel becomes idle.
 *
 * We would then need to deal with 'all channels in-use'
 */
static void mtk_dma_sched(unsigned long data)
{
	struct mtk_dmadev *mtkd = (struct mtk_dmadev *)data;
	struct virt_dma_desc *vd;
	struct mtk_chan *c;
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irq(&mtkd->lock);
	list_splice_tail_init(&mtkd->pending, &head);
	spin_unlock_irq(&mtkd->lock);

	if (!list_empty(&head)) {
		c = list_first_entry(&head, struct mtk_chan, node);

		spin_lock_irqsave(&c->vc.lock, flags);
		if (c->cfg.direction == DMA_DEV_TO_MEM) {
			list_del_init(&c->node);
			mtk_dma_start_rx(c);
		} else if (c->cfg.direction == DMA_MEM_TO_DEV) {
			vd = vchan_next_desc(&c->vc);
			c->desc = to_mtk_dma_desc(&vd->tx);
			list_del_init(&c->node);
			mtk_dma_start_tx(c);
		}
		spin_unlock_irqrestore(&c->vc.lock, flags);
	}
}

static int mtk_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(chan->device);
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	int ret = -EBUSY;

	pm_runtime_get_sync(mtkd->ddev.dev);

	if (!mtkd->ch[chan->chan_id]) {
		c->base = mtkd->mem_base[chan->chan_id];
		mtkd->ch[chan->chan_id] = c;
		ret = 1;
	}
	c->requested = false;
	mtk_dma_reset(c);

	return ret;
}

static void mtk_dma_free_chan_resources(struct dma_chan *chan)
{
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(chan->device);
	struct mtk_chan *c = to_mtk_dma_chan(chan);

	if (c->requested) {
		c->requested = false;
		free_irq(mtkd->dma_irq[chan->chan_id], chan);
	}

	tasklet_kill(&mtkd->task);
	tasklet_kill(&c->vc.task);

	c->base = NULL;
	mtkd->ch[chan->chan_id] = NULL;
	vchan_free_chan_resources(&c->vc);

	pm_runtime_put_sync(mtkd->ddev.dev);
}

static enum dma_status mtk_dma_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *txstate)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	enum dma_status ret;
	unsigned long flags;

	if (!txstate)
		return DMA_ERROR;

	ret = dma_cookie_status(chan, cookie, txstate);
	spin_lock_irqsave(&c->vc.lock, flags);
	if (ret == DMA_IN_PROGRESS) {
		c->rx_status = mtk_dma_chan_read(c, VFF_RPT)
			     & MTK_DMA_RING_SIZE;
		dma_set_residue(txstate, c->rx_status);
	} else if (ret == DMA_COMPLETE && c->cfg.direction == DMA_DEV_TO_MEM) {
		dma_set_residue(txstate, c->rx_status);
	} else {
		dma_set_residue(txstate, 0);
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

static struct dma_async_tx_descriptor *mtk_dma_prep_slave_sg
	(struct dma_chan *chan, struct scatterlist *sgl,
	unsigned int sglen,	enum dma_transfer_direction dir,
	unsigned long tx_flags, void *context)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	struct scatterlist *sgent;
	struct mtk_dma_desc *d;
	struct mtk_dma_sg *sg;
	unsigned int size, i, j, en;

	en = 1;

	if ((dir != DMA_DEV_TO_MEM) &&
		(dir != DMA_MEM_TO_DEV)) {
		dev_err(chan->device->dev, "bad direction\n");
		return NULL;
	}

	/* Now allocate and setup the descriptor. */
	d = kzalloc(sizeof(*d) + sglen * sizeof(d->sg[0]), GFP_ATOMIC);
	if (!d)
		return NULL;

	d->dir = dir;

	j = 0;
	for_each_sg(sgl, sgent, sglen, i) {
		d->sg[j].addr = sg_dma_address(sgent);
		d->sg[j].en = en;
		d->sg[j].fn = sg_dma_len(sgent) / en;
		j++;
	}

	d->sglen = j;

	if (dir == DMA_MEM_TO_DEV) {
		for (size = i = 0; i < d->sglen; i++) {
			sg = &d->sg[i];
			size += sg->en * sg->fn;
		}
		d->len = size;
	}

	return vchan_tx_prep(&c->vc, &d->vd, tx_flags);
}

static void mtk_dma_issue_pending(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	struct virt_dma_desc *vd;
	struct mtk_dmadev *mtkd;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (c->cfg.direction == DMA_DEV_TO_MEM) {
		mtkd = to_mtk_dma_dev(chan->device);
		if (vchan_issue_pending(&c->vc) && !c->desc) {
			vd = vchan_next_desc(&c->vc);
			c->desc = to_mtk_dma_desc(&vd->tx);
		}
	} else if (c->cfg.direction == DMA_MEM_TO_DEV) {
		if (vchan_issue_pending(&c->vc) && !c->desc) {
			vd = vchan_next_desc(&c->vc);
			c->desc = to_mtk_dma_desc(&vd->tx);
			mtk_dma_start_tx(c);
		}
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static irqreturn_t mtk_dma_rx_interrupt(int irq, void *dev_id)
{
	struct dma_chan *chan = (struct dma_chan *)dev_id;
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);

	mtk_dma_start_rx(c);

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t mtk_dma_tx_interrupt(int irq, void *dev_id)
{
	struct dma_chan *chan = (struct dma_chan *)dev_id;
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(chan->device);
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	struct mtk_dma_desc *d = c->desc;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (d->len != 0U) {
		list_add_tail(&c->node, &mtkd->pending);
		tasklet_schedule(&mtkd->task);
	} else {
		list_del(&d->vd.node);
		vchan_cookie_complete(&d->vd);
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);

	mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);

	return IRQ_HANDLED;
}

static int mtk_dma_slave_config(struct dma_chan *chan,
				struct dma_slave_config *cfg)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	struct mtk_dmadev *mtkd = to_mtk_dma_dev(c->vc.chan.device);
	int ret;

	c->cfg = *cfg;

	if (cfg->direction == DMA_DEV_TO_MEM) {
		unsigned int rx_len = cfg->src_addr_width * 1024;

		mtk_dma_chan_write(c, VFF_ADDR, cfg->src_addr);
		mtk_dma_chan_write(c, VFF_LEN, rx_len);
		mtk_dma_chan_write(c, VFF_THRE, VFF_RX_THRE(rx_len));
		mtk_dma_chan_write(c,
				   VFF_INT_EN, VFF_RX_INT_EN0_B
				   | VFF_RX_INT_EN1_B);
		mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);
		mtk_dma_chan_write(c, VFF_EN, VFF_EN_B);

		if (!c->requested) {
			c->requested = true;
			ret = request_irq(mtkd->dma_irq[chan->chan_id],
					  mtk_dma_rx_interrupt,
					  IRQF_TRIGGER_NONE,
					  KBUILD_MODNAME, chan);
			if (ret < 0) {
				dev_err(chan->device->dev, "Can't request rx dma IRQ\n");
				return -EINVAL;
			}
		}
	} else if (cfg->direction == DMA_MEM_TO_DEV)	{
		unsigned int tx_len = cfg->dst_addr_width * 1024;

		mtk_dma_chan_write(c, VFF_ADDR, cfg->dst_addr);
		mtk_dma_chan_write(c, VFF_LEN, tx_len);
		mtk_dma_chan_write(c, VFF_THRE, VFF_TX_THRE(tx_len));
		mtk_dma_chan_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);
		mtk_dma_chan_write(c, VFF_EN, VFF_EN_B);

		if (!c->requested) {
			c->requested = true;
			ret = request_irq(mtkd->dma_irq[chan->chan_id],
					  mtk_dma_tx_interrupt,
					  IRQF_TRIGGER_NONE,
					  KBUILD_MODNAME, chan);
			if (ret < 0) {
				dev_err(chan->device->dev, "Can't request tx dma IRQ\n");
				return -EINVAL;
			}
		}
	}

	if (mtkd->support_33bits)
		mtk_dma_chan_write(c, VFF_4G_SUPPORT, VFF_4G_SUPPORT_B);

	if (mtk_dma_chan_read(c, VFF_EN) != VFF_EN_B) {
		dev_err(chan->device->dev,
			"config dma dir[%d] fail\n", cfg->direction);
		return -EINVAL;
	}

	return 0;
}

static int mtk_dma_terminate_all(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	list_del_init(&c->node);
	mtk_dma_stop(c);
	spin_unlock_irqrestore(&c->vc.lock, flags);

	return 0;
}

static int mtk_dma_device_pause(struct dma_chan *chan)
{
	/* just for check caps pass */
	return -EINVAL;
}

static int mtk_dma_device_resume(struct dma_chan *chan)
{
	/* just for check caps pass */
	return -EINVAL;
}

static void mtk_dma_free(struct mtk_dmadev *mtkd)
{
	tasklet_kill(&mtkd->task);
	while (list_empty(&mtkd->ddev.channels) == 0) {
		struct mtk_chan *c = list_first_entry(&mtkd->ddev.channels,
			struct mtk_chan, vc.chan.device_node);

		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
		devm_kfree(mtkd->ddev.dev, c);
	}
}

static const struct of_device_id mtk_uart_dma_match[] = {
	{ .compatible = "mediatek,mt6577-uart-dma", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_uart_dma_match);

static int mtk_apdma_probe(struct platform_device *pdev)
{
	struct mtk_dmadev *mtkd;
	struct resource *res;
	struct mtk_chan *c;
	unsigned int i;
	int rc;

	mtkd = devm_kzalloc(&pdev->dev, sizeof(*mtkd), GFP_KERNEL);
	if (!mtkd)
		return -ENOMEM;

	for (i = 0; i < MTK_APDMA_CHANNELS; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			return -ENODEV;
		mtkd->mem_base[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(mtkd->mem_base[i]))
			return PTR_ERR(mtkd->mem_base[i]);
	}

	for (i = 0; i < MTK_APDMA_CHANNELS; i++) {
		mtkd->dma_irq[i] = platform_get_irq(pdev, i);
		if ((int)mtkd->dma_irq[i] < 0) {
			dev_err(&pdev->dev, "failed to get IRQ[%d]\n", i);
			return -EINVAL;
		}
	}

	mtkd->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(mtkd->clk)) {
		dev_err(&pdev->dev, "No clock specified\n");
		return PTR_ERR(mtkd->clk);
	}

	if (of_property_read_bool(pdev->dev.of_node, "dma-33bits")) {
		dev_info(&pdev->dev, "Support dma 33bits\n");
		mtkd->support_33bits = true;
	}

	if (mtkd->support_33bits)
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(33));
	else
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;

	dma_cap_set(DMA_SLAVE, mtkd->ddev.cap_mask);
	mtkd->ddev.device_alloc_chan_resources = mtk_dma_alloc_chan_resources;
	mtkd->ddev.device_free_chan_resources = mtk_dma_free_chan_resources;
	mtkd->ddev.device_tx_status = mtk_dma_tx_status;
	mtkd->ddev.device_issue_pending = mtk_dma_issue_pending;
	mtkd->ddev.device_prep_slave_sg = mtk_dma_prep_slave_sg;
	mtkd->ddev.device_config = mtk_dma_slave_config;
	mtkd->ddev.device_pause = mtk_dma_device_pause;
	mtkd->ddev.device_resume = mtk_dma_device_resume;
	mtkd->ddev.device_terminate_all = mtk_dma_terminate_all;
	mtkd->ddev.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE);
	mtkd->ddev.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE);
	mtkd->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	mtkd->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	mtkd->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&mtkd->ddev.channels);
	INIT_LIST_HEAD(&mtkd->pending);

	spin_lock_init(&mtkd->lock);
	tasklet_init(&mtkd->task, mtk_dma_sched, (unsigned long)mtkd);

	mtkd->dma_requests = MTK_APDMA_DEFAULT_REQUESTS;
	if (of_property_read_u32(pdev->dev.of_node,
				 "dma-requests", &mtkd->dma_requests)) {
		dev_info(&pdev->dev,
			 "Missing dma-requests property, using %u.\n",
			 MTK_APDMA_DEFAULT_REQUESTS);
	}

	for (i = 0; i < MTK_APDMA_CHANNELS; i++) {
		c = devm_kzalloc(mtkd->ddev.dev, sizeof(*c), GFP_KERNEL);
		if (!c)
			goto err_no_dma;

		c->vc.desc_free = mtk_dma_desc_free;
		vchan_init(&c->vc, &mtkd->ddev);
		INIT_LIST_HEAD(&c->node);
	}

	pm_runtime_enable(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);

	rc = dma_async_device_register(&mtkd->ddev);
	if (rc)
		goto rpm_disable;

	platform_set_drvdata(pdev, mtkd);

	if (pdev->dev.of_node) {
		/* Device-tree DMA controller registration */
		rc = of_dma_controller_register(pdev->dev.of_node,
						of_dma_xlate_by_chan_id,
						mtkd);
		if (rc)
			goto dma_remove;
	}

	return rc;

dma_remove:
	dma_async_device_unregister(&mtkd->ddev);
rpm_disable:
	pm_runtime_disable(&pdev->dev);
err_no_dma:
	mtk_dma_free(mtkd);
	return rc;
}

static int mtk_apdma_remove(struct platform_device *pdev)
{
	struct mtk_dmadev *mtkd = platform_get_drvdata(pdev);

	if (pdev->dev.of_node)
		of_dma_controller_free(pdev->dev.of_node);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	dma_async_device_unregister(&mtkd->ddev);

	mtk_dma_free(mtkd);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mtk_dma_suspend(struct device *dev)
{
	struct mtk_dmadev *mtkd = dev_get_drvdata(dev);

	if (!pm_runtime_suspended(dev))
		mtk_dma_clk_disable(mtkd);

	return 0;
}

static int mtk_dma_resume(struct device *dev)
{
	int ret;
	struct mtk_dmadev *mtkd = dev_get_drvdata(dev);

	if (!pm_runtime_suspended(dev)) {
		ret = mtk_dma_clk_enable(mtkd);
		if (ret)
			return ret;
	}

	return 0;
}

static int mtk_dma_runtime_suspend(struct device *dev)
{
	struct mtk_dmadev *mtkd = dev_get_drvdata(dev);

	mtk_dma_clk_disable(mtkd);

	return 0;
}

static int mtk_dma_runtime_resume(struct device *dev)
{
	int ret;
	struct mtk_dmadev *mtkd = dev_get_drvdata(dev);

	ret = mtk_dma_clk_enable(mtkd);
	if (ret)
		return ret;

	return 0;
}

#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops mtk_dma_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_dma_suspend, mtk_dma_resume)
	SET_RUNTIME_PM_OPS(mtk_dma_runtime_suspend,
			   mtk_dma_runtime_resume, NULL)
};

static struct platform_driver mtk_dma_driver = {
	.probe	= mtk_apdma_probe,
	.remove	= mtk_apdma_remove,
	.driver = {
		.name		= KBUILD_MODNAME,
		.pm		= &mtk_dma_pm_ops,
		.of_match_table = of_match_ptr(mtk_uart_dma_match),
	},
};

module_platform_driver(mtk_dma_driver);

MODULE_DESCRIPTION("MediaTek UART APDMA Controller Driver");
MODULE_AUTHOR("Long Cheng <long.cheng@mediatek.com>");
MODULE_LICENSE("GPL v2");

