/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - DMA controller
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/flexcard.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/flexcard.h>

#include "flexcard-dma.h"

/*
 * Allocate twice the size of FLEXCARD_DMA_BUF_SIZE for the receiving
 * ring buffer to easily handle wrap-arounds.
 */
#define DMA_TOTAL_BUF_SIZE		(2*FLEXCARD_DMA_BUF_SIZE)

static int flexcard_dma_stop(struct flexcard_dma *dma)
{
	u32 __iomem *dma_ctrl = &dma->reg->dma_ctrl;
	u32 __iomem *dma_stat = &dma->reg->dma_stat;
	int retry;

	writel(FLEXCARD_DMA_CTRL_STOP_REQ, dma_ctrl);

	/*
	 * DMA_IDLE bit reads 1 when the DMA state machine is in idle state
	 * after a stop request, otherwise 0. DMA stop should complete in at
	 * least 200us.
	 */
	retry = 200;
	while (!(readl(dma_ctrl) & FLEXCARD_DMA_CTRL_DMA_IDLE) && retry--)
		udelay(1);
	if (!retry)
		return -EBUSY;

	/*
	 * Check for max. 200us, if there are DMA jobs in progress.
	 */
	retry = 200;
	while ((readl(dma_stat) & FLEXCARD_DMA_STAT_BUSY) && retry--)
		udelay(1);

	return retry ? 0 : -EBUSY;
}

static int flexcard_dma_reset(struct flexcard_dma *dma)
{
	u32 __iomem *dma_ctrl = &dma->reg->dma_ctrl;
	int retry = 500;

	writel(FLEXCARD_DMA_CTRL_RST_DMA, dma_ctrl);

	/*
	 * DMA_IDLE bit reads 1 when the DMA state machine is in idle state
	 * after a reset request, otherwise 0. DMA reset should complete in
	 * at least 5ms.
	 */
	while (!(readl(dma_ctrl) & FLEXCARD_DMA_CTRL_DMA_IDLE) && retry--)
		udelay(10);

	return retry ? 0 : -EIO;
}

static int flexcard_dma_setup(struct flexcard_dma *dma)
{
	int ret;

	ret = flexcard_dma_reset(dma);
	if (ret)
		return ret;

	writel(0x0, &dma->reg->dma_rptr);
	writel(0x0, &dma->reg->dma_wptr);
	writel(0x0, &dma->reg->dma_ctrl);

	writeq(dma->phys, &dma->reg->dma_cba);
	writel(FLEXCARD_DMA_BUF_SIZE, &dma->reg->dma_cbs);

	return ret;
}

static irqreturn_t flexcard_dma_isr(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct flexcard_dma *dma = platform_get_drvdata(pdev);
	u32 avail, parsed, rptr = dma->rptr;

	avail = readl(&dma->reg->dma_cblr);
	if (!avail)
		return IRQ_NONE;

	do {
		u32 tocp = rptr + FLEXCARD_MAX_PAKET_SIZE;
		/*
		 * For simplicity the parser always looks at contiguous
		 * buffer space.
		 *
		 * We ensure that by copying the eventually wrapped
		 * bytes of the next message from the bottom of the
		 * dma buffer to the space right after the dma buffer
		 * which has been allocated just for that reason.
		 */
		if (tocp > FLEXCARD_DMA_BUF_SIZE) {
			tocp &= FLEXCARD_DMA_BUF_MASK;
			memcpy(dma->buf + FLEXCARD_DMA_BUF_SIZE,
			       dma->buf, tocp);
		}

		parsed = flexcard_parse_packet(dma->buf + rptr, avail, dma);
		if (parsed > avail) {
			dev_err(&pdev->dev, "Parser overrun\n");
			rptr = (rptr + parsed) & FLEXCARD_DMA_BUF_MASK;
			break;
		}
		avail -= parsed;
		rptr = (rptr + parsed) & FLEXCARD_DMA_BUF_MASK;
	} while (parsed && avail);

	/* Update the read pointer in the device if we processed data */
	if (dma->rptr != rptr) {
		dma->rptr = rptr;
		writel(rptr, &dma->reg->dma_rptr);
	} else {
		/* This may happen if no packets has been parsed */
		dev_err_ratelimited(&pdev->dev, "rptr unchanged\n");
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static irqreturn_t flexcard_dma_ovr(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct flexcard_dma *dma = platform_get_drvdata(pdev);
	u32 stat;

	/* check overflow flag */
	stat = readl(&dma->reg->dma_stat);
	if (!(stat & FLEXCARD_DMA_STAT_OFL))
		return IRQ_NONE;

	dev_err(&pdev->dev, "DMA buffer overflow\n");

	writel(0x0, &dma->reg->dma_rptr);

	/* reset overflow flag */
	writel(FLEXCARD_DMA_STAT_OFL, &dma->reg->dma_stat);

	return IRQ_HANDLED;
}

static int flexcard_dma_resource(struct platform_device *pdev)
{
	struct flexcard_dma *dma = platform_get_drvdata(pdev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	dma->reg = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!dma->reg) {
		dev_err(&pdev->dev, "failed to map DMA register\n");
		return -ENOMEM;
	}

	dma->irq = platform_get_irq(pdev, 0);
	if (dma->irq < 0) {
		dev_err(&pdev->dev, "failed to get CBL IRQ\n");
		return -ENXIO;
	}

	dma->irq_ovr = platform_get_irq(pdev, 1);
	if (dma->irq_ovr < 0) {
		dev_err(&pdev->dev, "failed to get CO IRQ\n");
		return -ENXIO;
	}
	return 0;
}

static int flexcard_dma_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell;
	struct flexcard_dma *dma;
	int ret;

	cell = mfd_get_cell(pdev);
	if (!cell)
		return -ENODEV;

	dma = devm_kzalloc(&pdev->dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	platform_set_drvdata(pdev, dma);

	dma->buf = dma_alloc_coherent(&pdev->dev, DMA_TOTAL_BUF_SIZE,
				      &dma->phys, GFP_KERNEL);
	if (!dma->buf) {
		dev_err(&pdev->dev, "could not allocate DMA memory\n");
		return -ENOMEM;
	}

	ret = flexcard_dma_resource(pdev);
	if (ret)
		goto out_free_buf;

	ret = flexcard_dma_setup(dma);
	if (ret) {
		dev_err(&pdev->dev, "could not setup Flexcard DMA: %d\n", ret);
		goto out_free_buf;
	}

	ret = devm_request_threaded_irq(&pdev->dev, dma->irq, NULL,
					flexcard_dma_isr, IRQF_ONESHOT,
					"flexcard-CBL", pdev);
	if (ret) {
		dev_err(&pdev->dev, "could not request Flexcard DMA CBL IRQ\n");
		goto out_free_buf;
	}

	ret = devm_request_irq(&pdev->dev, dma->irq_ovr, flexcard_dma_ovr, 0,
			  "flexcard-CO", pdev);
	if (ret) {
		dev_err(&pdev->dev, "could not request Flexcard DMA CO IRQ\n");
		goto out_free_irq;
	}

	writel(FLEXCARD_DMA_CTRL_DMA_ENA, &dma->reg->dma_ctrl);
	writel(0x300, &dma->reg->dma_cbcr);

	dev_info(&pdev->dev, "Flexcard DMA registered");

	return 0;

out_free_irq:
	writel(0x0, &dma->reg->dma_ctrl);
out_free_buf:
	dma_free_coherent(&pdev->dev, DMA_TOTAL_BUF_SIZE,
			  dma->buf, dma->phys);
	return ret;
}

static int flexcard_dma_remove(struct platform_device *pdev)
{
	struct flexcard_dma *dma = platform_get_drvdata(pdev);
	int ret;

	ret = flexcard_dma_stop(dma);
	if (ret) {
		dev_err(&pdev->dev, "could not stop DMA state machine\n");
		return ret;
	}

	dma_free_coherent(&pdev->dev, DMA_TOTAL_BUF_SIZE,
			  dma->buf, dma->phys);

	return ret;
}

static struct platform_driver flexcard_dma_driver = {
	.probe		= flexcard_dma_probe,
	.remove		= flexcard_dma_remove,
	.driver		= {
		.name   = "flexcard-dma",
	}
};

module_platform_driver(flexcard_dma_driver);

MODULE_AUTHOR("Holger Dengler <dengler@linutronix.de>");
MODULE_AUTHOR("Benedikt Spranger <b.spranger@linutronix.de>");
MODULE_DESCRIPTION("Eberspaecher Flexcard PMC II DMA Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:flexcard-dma");
