// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek 8250 DMA driver.
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
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "../virt-dma.h"

#define MTK_UART_APDMA_CHANNELS		(CONFIG_SERIAL_8250_NR_UARTS * 2)

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
#define VFF_INT_EN_CLR_B	0
#define VFF_4G_SUPPORT_CLR_B	0

/* interrupt trigger level for tx */
#define VFF_TX_THRE(n)		((n) * 7 / 8)
/* interrupt trigger level for rx */
#define VFF_RX_THRE(n)		((n) * 3 / 4)

#define VFF_RING_SIZE	0xffffU
/* invert this bit when wrap ring head again*/
#define VFF_RING_WRAP	0x10000U

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

struct mtk_uart_apdmadev {
	struct dma_device ddev;
	struct clk *clk;
	bool support_33bits;
	unsigned int dma_irq[MTK_UART_APDMA_CHANNELS];
};

struct mtk_uart_apdma_desc {
	struct virt_dma_desc vd;

	unsigned int avail_len;
};

struct mtk_chan {
	struct virt_dma_chan vc;
	struct dma_slave_config	cfg;
	void __iomem *base;
	struct mtk_uart_apdma_desc *desc;

	bool requested;

	unsigned int rx_status;
};

static inline struct mtk_uart_apdmadev *
to_mtk_uart_apdma_dev(struct dma_device *d)
{
	return container_of(d, struct mtk_uart_apdmadev, ddev);
}

static inline struct mtk_chan *to_mtk_uart_apdma_chan(struct dma_chan *c)
{
	return container_of(c, struct mtk_chan, vc.chan);
}

static inline struct mtk_uart_apdma_desc *to_mtk_uart_apdma_desc
	(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct mtk_uart_apdma_desc, vd.tx);
}

static void mtk_uart_apdma_write(struct mtk_chan *c,
			       unsigned int reg, unsigned int val)
{
	writel(val, c->base + reg);
}

static unsigned int mtk_uart_apdma_read(struct mtk_chan *c, unsigned int reg)
{
	return readl(c->base + reg);
}

static void mtk_uart_apdma_desc_free(struct virt_dma_desc *vd)
{
	struct dma_chan *chan = vd->tx.chan;
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);

	kfree(c->desc);
}

static void mtk_uart_apdma_start_tx(struct mtk_chan *c)
{
	unsigned int len, send, left, wpt, d_wpt, tmp;
	int ret;

	left = mtk_uart_apdma_read(c, VFF_LEFT_SIZE);
	if (!left) {
		mtk_uart_apdma_write(c, VFF_INT_EN, VFF_TX_INT_EN_B);
		return;
	}

	/* Wait 1sec for flush,  can't sleep*/
	ret = readx_poll_timeout(readl, c->base + VFF_FLUSH, tmp,
			tmp != VFF_FLUSH_B, 0, 1000000);
	if (ret)
		dev_warn(c->vc.chan.device->dev, "tx: fail, debug=0x%x\n",
			mtk_uart_apdma_read(c, VFF_DEBUG_STATUS));

	send = min_t(unsigned int, left, c->desc->avail_len);
	wpt = mtk_uart_apdma_read(c, VFF_WPT);
	len = mtk_uart_apdma_read(c, VFF_LEN);

	d_wpt = wpt + send;
	if ((d_wpt & VFF_RING_SIZE) >= len) {
		d_wpt = d_wpt - len;
		d_wpt = d_wpt ^ VFF_RING_WRAP;
	}
	mtk_uart_apdma_write(c, VFF_WPT, d_wpt);

	c->desc->avail_len -= send;

	mtk_uart_apdma_write(c, VFF_INT_EN, VFF_TX_INT_EN_B);
	if (mtk_uart_apdma_read(c, VFF_FLUSH) == 0U)
		mtk_uart_apdma_write(c, VFF_FLUSH, VFF_FLUSH_B);
}

static void mtk_uart_apdma_start_rx(struct mtk_chan *c)
{
	struct mtk_uart_apdma_desc *d = c->desc;
	unsigned int len, wg, rg, cnt;

	if ((mtk_uart_apdma_read(c, VFF_VALID_SIZE) == 0U) ||
		!d || !vchan_next_desc(&c->vc))
		return;

	len = mtk_uart_apdma_read(c, VFF_LEN);
	rg = mtk_uart_apdma_read(c, VFF_RPT);
	wg = mtk_uart_apdma_read(c, VFF_WPT);
	if ((rg ^ wg) & VFF_RING_WRAP)
		cnt = (wg & VFF_RING_SIZE) + len - (rg & VFF_RING_SIZE);
	else
		cnt = (wg & VFF_RING_SIZE) - (rg & VFF_RING_SIZE);

	c->rx_status = cnt;
	mtk_uart_apdma_write(c, VFF_RPT, wg);

	list_del(&d->vd.node);
	vchan_cookie_complete(&d->vd);
}

static irqreturn_t mtk_uart_apdma_irq_handler(int irq, void *dev_id)
{
	struct dma_chan *chan = (struct dma_chan *)dev_id;
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	struct mtk_uart_apdma_desc *d;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (c->cfg.direction == DMA_DEV_TO_MEM) {
		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);
		mtk_uart_apdma_start_rx(c);
	} else if (c->cfg.direction == DMA_MEM_TO_DEV) {
		d = c->desc;

		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);

		if (d->avail_len != 0U) {
			mtk_uart_apdma_start_tx(c);
		} else {
			list_del(&d->vd.node);
			vchan_cookie_complete(&d->vd);
		}
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static int mtk_uart_apdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct mtk_uart_apdmadev *mtkd = to_mtk_uart_apdma_dev(chan->device);
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	u32 tmp;
	int ret;

	pm_runtime_get_sync(mtkd->ddev.dev);

	mtk_uart_apdma_write(c, VFF_ADDR, 0);
	mtk_uart_apdma_write(c, VFF_THRE, 0);
	mtk_uart_apdma_write(c, VFF_LEN, 0);
	mtk_uart_apdma_write(c, VFF_RST, VFF_WARM_RST_B);

	ret = readx_poll_timeout(readl, c->base + VFF_EN, tmp,
			tmp == 0, 10, 100);
	if (ret) {
		dev_err(chan->device->dev, "dma reset: fail, timeout\n");
		return ret;
	}

	if (!c->requested) {
		c->requested = true;
		ret = request_irq(mtkd->dma_irq[chan->chan_id],
				  mtk_uart_apdma_irq_handler, IRQF_TRIGGER_NONE,
				  KBUILD_MODNAME, chan);
		if (ret < 0) {
			dev_err(chan->device->dev, "Can't request dma IRQ\n");
			return -EINVAL;
		}
	}

	if (mtkd->support_33bits)
		mtk_uart_apdma_write(c, VFF_4G_SUPPORT, VFF_4G_SUPPORT_CLR_B);

	return ret;
}

static void mtk_uart_apdma_free_chan_resources(struct dma_chan *chan)
{
	struct mtk_uart_apdmadev *mtkd = to_mtk_uart_apdma_dev(chan->device);
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);

	if (c->requested) {
		c->requested = false;
		free_irq(mtkd->dma_irq[chan->chan_id], chan);
	}

	tasklet_kill(&c->vc.task);

	vchan_free_chan_resources(&c->vc);

	pm_runtime_put_sync(mtkd->ddev.dev);
}

static enum dma_status mtk_uart_apdma_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *txstate)
{
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	enum dma_status ret;
	unsigned long flags;

	if (!txstate)
		return DMA_ERROR;

	ret = dma_cookie_status(chan, cookie, txstate);
	spin_lock_irqsave(&c->vc.lock, flags);
	if (ret == DMA_IN_PROGRESS) {
		c->rx_status = mtk_uart_apdma_read(c, VFF_RPT) & VFF_RING_SIZE;
		dma_set_residue(txstate, c->rx_status);
	} else if (ret == DMA_COMPLETE && c->cfg.direction == DMA_DEV_TO_MEM) {
		dma_set_residue(txstate, c->rx_status);
	} else {
		dma_set_residue(txstate, 0);
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

/*
 * dmaengine_prep_slave_single will call the function. and sglen is 1.
 * 8250 uart using one ring buffer, and deal with one sg.
 */
static struct dma_async_tx_descriptor *mtk_uart_apdma_prep_slave_sg
	(struct dma_chan *chan, struct scatterlist *sgl,
	unsigned int sglen, enum dma_transfer_direction dir,
	unsigned long tx_flags, void *context)
{
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	struct mtk_uart_apdma_desc *d;

	if ((dir != DMA_DEV_TO_MEM) &&
		(dir != DMA_MEM_TO_DEV)) {
		dev_err(chan->device->dev, "bad direction\n");
		return NULL;
	}

	/* Now allocate and setup the descriptor */
	d = kzalloc(sizeof(*d), GFP_ATOMIC);
	if (!d)
		return NULL;

	/* sglen is 1 */
	d->avail_len = sg_dma_len(sgl);

	return vchan_tx_prep(&c->vc, &d->vd, tx_flags);
}

static void mtk_uart_apdma_issue_pending(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	struct virt_dma_desc *vd;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (c->cfg.direction == DMA_DEV_TO_MEM) {
		if (vchan_issue_pending(&c->vc)) {
			vd = vchan_next_desc(&c->vc);
			c->desc = to_mtk_uart_apdma_desc(&vd->tx);
			mtk_uart_apdma_start_rx(c);
		}
	} else if (c->cfg.direction == DMA_MEM_TO_DEV) {
		if (vchan_issue_pending(&c->vc)) {
			vd = vchan_next_desc(&c->vc);
			c->desc = to_mtk_uart_apdma_desc(&vd->tx);
			mtk_uart_apdma_start_tx(c);
		}
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static int mtk_uart_apdma_slave_config(struct dma_chan *chan,
				struct dma_slave_config *cfg)
{
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	struct mtk_uart_apdmadev *mtkd =
				to_mtk_uart_apdma_dev(c->vc.chan.device);

	c->cfg = *cfg;

	if (cfg->direction == DMA_DEV_TO_MEM) {
		unsigned int rx_len = cfg->src_addr_width * 1024;

		mtk_uart_apdma_write(c, VFF_ADDR, cfg->src_addr);
		mtk_uart_apdma_write(c, VFF_LEN, rx_len);
		mtk_uart_apdma_write(c, VFF_THRE, VFF_RX_THRE(rx_len));
		mtk_uart_apdma_write(c, VFF_INT_EN,
				VFF_RX_INT_EN0_B | VFF_RX_INT_EN1_B);
		mtk_uart_apdma_write(c, VFF_RPT, 0);
		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);
		mtk_uart_apdma_write(c, VFF_EN, VFF_EN_B);
	} else if (cfg->direction == DMA_MEM_TO_DEV)	{
		unsigned int tx_len = cfg->dst_addr_width * 1024;

		mtk_uart_apdma_write(c, VFF_ADDR, cfg->dst_addr);
		mtk_uart_apdma_write(c, VFF_LEN, tx_len);
		mtk_uart_apdma_write(c, VFF_THRE, VFF_TX_THRE(tx_len));
		mtk_uart_apdma_write(c, VFF_WPT, 0);
		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);
		mtk_uart_apdma_write(c, VFF_EN, VFF_EN_B);
	}

	if (mtkd->support_33bits)
		mtk_uart_apdma_write(c, VFF_4G_SUPPORT, VFF_4G_SUPPORT_B);

	if (mtk_uart_apdma_read(c, VFF_EN) != VFF_EN_B) {
		dev_err(chan->device->dev, "dir[%d] fail\n", cfg->direction);
		return -EINVAL;
	}

	return 0;
}

static int mtk_uart_apdma_terminate_all(struct dma_chan *chan)
{
	struct mtk_chan *c = to_mtk_uart_apdma_chan(chan);
	unsigned long flags;
	u32 tmp;
	int ret;

	spin_lock_irqsave(&c->vc.lock, flags);

	mtk_uart_apdma_write(c, VFF_FLUSH, VFF_FLUSH_B);
	/* Wait 1sec for flush,  can't sleep*/
	ret = readx_poll_timeout(readl, c->base + VFF_FLUSH, tmp,
			tmp != VFF_FLUSH_B, 0, 1000000);
	if (ret)
		dev_err(c->vc.chan.device->dev, "flush: fail, debug=0x%x\n",
			mtk_uart_apdma_read(c, VFF_DEBUG_STATUS));

	/*set stop as 1 -> wait until en is 0 -> set stop as 0*/
	mtk_uart_apdma_write(c, VFF_STOP, VFF_STOP_B);
	ret = readx_poll_timeout(readl, c->base + VFF_EN, tmp,
			tmp == 0, 10, 100);
	if (ret)
		dev_err(c->vc.chan.device->dev, "stop: fail, debug=0x%x\n",
			mtk_uart_apdma_read(c, VFF_DEBUG_STATUS));

	mtk_uart_apdma_write(c, VFF_STOP, VFF_STOP_CLR_B);
	mtk_uart_apdma_write(c, VFF_INT_EN, VFF_INT_EN_CLR_B);

	if (c->cfg.direction == DMA_DEV_TO_MEM)
		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_RX_INT_FLAG_CLR_B);
	else if (c->cfg.direction == DMA_MEM_TO_DEV)
		mtk_uart_apdma_write(c, VFF_INT_FLAG, VFF_TX_INT_FLAG_CLR_B);

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return 0;
}

static int mtk_uart_apdma_device_pause(struct dma_chan *chan)
{
	/* just for check caps pass */
	return 0;
}

static int mtk_uart_apdma_device_resume(struct dma_chan *chan)
{
	/* just for check caps pass */
	return 0;
}

static void mtk_uart_apdma_free(struct mtk_uart_apdmadev *mtkd)
{
	while (list_empty(&mtkd->ddev.channels) == 0) {
		struct mtk_chan *c = list_first_entry(&mtkd->ddev.channels,
			struct mtk_chan, vc.chan.device_node);

		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}
}

static const struct of_device_id mtk_uart_apdma_match[] = {
	{ .compatible = "mediatek,mt6577-uart-dma", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_uart_apdma_match);

static int mtk_uart_apdma_probe(struct platform_device *pdev)
{
	struct mtk_uart_apdmadev *mtkd;
	struct resource *res;
	struct mtk_chan *c;
	unsigned int i;
	int rc;

	mtkd = devm_kzalloc(&pdev->dev, sizeof(*mtkd), GFP_KERNEL);
	if (!mtkd)
		return -ENOMEM;

	mtkd->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(mtkd->clk)) {
		dev_err(&pdev->dev, "No clock specified\n");
		rc = PTR_ERR(mtkd->clk);
		return rc;
	}

	if (of_property_read_bool(pdev->dev.of_node, "dma-33bits"))
		mtkd->support_33bits = true;

	rc = dma_set_mask_and_coherent(&pdev->dev,
				DMA_BIT_MASK(32 | mtkd->support_33bits));
	if (rc)
		return rc;

	dma_cap_set(DMA_SLAVE, mtkd->ddev.cap_mask);
	mtkd->ddev.device_alloc_chan_resources =
				mtk_uart_apdma_alloc_chan_resources;
	mtkd->ddev.device_free_chan_resources =
				mtk_uart_apdma_free_chan_resources;
	mtkd->ddev.device_tx_status = mtk_uart_apdma_tx_status;
	mtkd->ddev.device_issue_pending = mtk_uart_apdma_issue_pending;
	mtkd->ddev.device_prep_slave_sg = mtk_uart_apdma_prep_slave_sg;
	mtkd->ddev.device_config = mtk_uart_apdma_slave_config;
	mtkd->ddev.device_pause = mtk_uart_apdma_device_pause;
	mtkd->ddev.device_resume = mtk_uart_apdma_device_resume;
	mtkd->ddev.device_terminate_all = mtk_uart_apdma_terminate_all;
	mtkd->ddev.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE);
	mtkd->ddev.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE);
	mtkd->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	mtkd->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	mtkd->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&mtkd->ddev.channels);

	for (i = 0; i < MTK_UART_APDMA_CHANNELS; i++) {
		c = devm_kzalloc(mtkd->ddev.dev, sizeof(*c), GFP_KERNEL);
		if (!c) {
			rc = -ENODEV;
			goto err_no_dma;
		}

		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			rc = -ENODEV;
			goto err_no_dma;
		}

		c->base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(c->base)) {
			rc = PTR_ERR(c->base);
			goto err_no_dma;
		}
		c->requested = false;
		c->vc.desc_free = mtk_uart_apdma_desc_free;
		vchan_init(&c->vc, &mtkd->ddev);

		mtkd->dma_irq[i] = platform_get_irq(pdev, i);
		if ((int)mtkd->dma_irq[i] < 0) {
			dev_err(&pdev->dev, "failed to get IRQ[%d]\n", i);
			rc = -EINVAL;
			goto err_no_dma;
		}
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
	mtk_uart_apdma_free(mtkd);
	return rc;
}

static int mtk_uart_apdma_remove(struct platform_device *pdev)
{
	struct mtk_uart_apdmadev *mtkd = platform_get_drvdata(pdev);

	if (pdev->dev.of_node)
		of_dma_controller_free(pdev->dev.of_node);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	dma_async_device_unregister(&mtkd->ddev);
	mtk_uart_apdma_free(mtkd);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mtk_uart_apdma_suspend(struct device *dev)
{
	struct mtk_uart_apdmadev *mtkd = dev_get_drvdata(dev);

	if (!pm_runtime_suspended(dev))
		clk_disable_unprepare(mtkd->clk);

	return 0;
}

static int mtk_uart_apdma_resume(struct device *dev)
{
	int ret;
	struct mtk_uart_apdmadev *mtkd = dev_get_drvdata(dev);

	if (!pm_runtime_suspended(dev)) {
		ret = clk_prepare_enable(mtkd->clk);
		if (ret)
			return ret;
	}

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM
static int mtk_uart_apdma_runtime_suspend(struct device *dev)
{
	struct mtk_uart_apdmadev *mtkd = dev_get_drvdata(dev);

	clk_disable_unprepare(mtkd->clk);

	return 0;
}

static int mtk_uart_apdma_runtime_resume(struct device *dev)
{
	int ret;
	struct mtk_uart_apdmadev *mtkd = dev_get_drvdata(dev);

	ret = clk_prepare_enable(mtkd->clk);
	if (ret)
		return ret;

	return 0;
}
#endif /* CONFIG_PM */

static const struct dev_pm_ops mtk_uart_apdma_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_uart_apdma_suspend, mtk_uart_apdma_resume)
	SET_RUNTIME_PM_OPS(mtk_uart_apdma_runtime_suspend,
			   mtk_uart_apdma_runtime_resume, NULL)
};

static struct platform_driver mtk_uart_apdma_driver = {
	.probe	= mtk_uart_apdma_probe,
	.remove	= mtk_uart_apdma_remove,
	.driver = {
		.name		= KBUILD_MODNAME,
		.pm		= &mtk_uart_apdma_pm_ops,
		.of_match_table = of_match_ptr(mtk_uart_apdma_match),
	},
};

module_platform_driver(mtk_uart_apdma_driver);

MODULE_DESCRIPTION("MediaTek UART APDMA Controller Driver");
MODULE_AUTHOR("Long Cheng <long.cheng@mediatek.com>");
MODULE_LICENSE("GPL v2");

