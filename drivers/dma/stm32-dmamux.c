/*
 * DMA Router driver for STM32 DMA MUX
 *
 * Copyright (C) 2017 M'Boumba Cedric Madianga <cedric.madianga@gmail.com>
 *
 * Based on TI DMA Crossbar driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/dma/stm32-dmamux.h>

#define STM32_DMAMUX_CCR(x)		(0x4 * (x))
#define STM32_DMAMUX_MAX_CHANNELS	32
#define STM32_DMAMUX_MAX_REQUESTS	255

struct stm32_dmamux {
	u32 request;
	u32 chan_id;
	bool busy;
};

struct stm32_dmamux_data {
	struct dma_router dmarouter;
	struct clk *clk;
	struct reset_control *rst;
	void __iomem *iomem;
	u32 dmamux_requests; /* number of DMA requests connected to DMAMUX */
	u32 dmamux_channels; /* Number of DMA channels supported */
	spinlock_t lock; /* Protects register access */
};

static inline u32 stm32_dmamux_read(void __iomem *iomem, u32 reg)
{
	return readl_relaxed(iomem + reg);
}

static inline void stm32_dmamux_write(void __iomem *iomem, u32 reg, u32 val)
{
	writel_relaxed(val, iomem + reg);
}

int stm32_dmamux_set_config(struct device *dev, void *route_data, u32 chan_id)
{
	struct stm32_dmamux_data *dmamux = dev_get_drvdata(dev);
	struct stm32_dmamux *mux = route_data;
	u32 request = mux->request;
	unsigned long flags;
	int ret;

	if (chan_id >= dmamux->dmamux_channels) {
		dev_err(dev, "invalid channel id\n");
		return -EINVAL;
	}

	/* Set dma request */
	spin_lock_irqsave(&dmamux->lock, flags);
	if (!IS_ERR(dmamux->clk)) {
		ret = clk_enable(dmamux->clk);
		if (ret < 0) {
			spin_unlock_irqrestore(&dmamux->lock, flags);
			dev_err(dev, "clk_prep_enable issue: %d\n", ret);
			return ret;
		}
	}

	stm32_dmamux_write(dmamux->iomem, STM32_DMAMUX_CCR(chan_id), request);

	mux->chan_id = chan_id;
	mux->busy = true;
	spin_unlock_irqrestore(&dmamux->lock, flags);

	dev_dbg(dev, "Mapping dma-router%dchan%d to request%d\n", dev->id,
		mux->chan_id, mux->request);

	return 0;
}

static void stm32_dmamux_free(struct device *dev, void *route_data)
{
	struct stm32_dmamux_data *dmamux = dev_get_drvdata(dev);
	struct stm32_dmamux *mux = route_data;
	unsigned long flags;

	/* Clear dma request */
	spin_lock_irqsave(&dmamux->lock, flags);
	if (!mux->busy) {
		spin_unlock_irqrestore(&dmamux->lock, flags);
		goto end;
	}

	stm32_dmamux_write(dmamux->iomem, STM32_DMAMUX_CCR(mux->chan_id), 0);
	if (!IS_ERR(dmamux->clk))
		clk_disable(dmamux->clk);
	spin_unlock_irqrestore(&dmamux->lock, flags);

	dev_dbg(dev, "Unmapping dma-router%dchan%d (was routed to request%d)\n",
		dev->id, mux->chan_id, mux->request);

end:
	kfree(mux);
}

static void *stm32_dmamux_route_allocate(struct of_phandle_args *dma_spec,
					 struct of_dma *ofdma)
{
	struct platform_device *pdev = of_find_device_by_node(ofdma->of_node);
	struct stm32_dmamux_data *dmamux = platform_get_drvdata(pdev);
	struct stm32_dmamux *mux;

	if (dma_spec->args_count != 3) {
		dev_err(&pdev->dev, "invalid number of dma mux args\n");
		return ERR_PTR(-EINVAL);
	}

	if (dma_spec->args[0] > dmamux->dmamux_requests) {
		dev_err(&pdev->dev, "invalid mux request number: %d\n",
			dma_spec->args[0]);
		return ERR_PTR(-EINVAL);
	}

	/* The of_node_put() will be done in of_dma_router_xlate function */
	dma_spec->np = of_parse_phandle(ofdma->of_node, "dma-masters", 0);
	if (!dma_spec->np) {
		dev_err(&pdev->dev, "can't get dma master\n");
		return ERR_PTR(-EINVAL);
	}

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux) {
		of_node_put(dma_spec->np);
		return ERR_PTR(-ENOMEM);
	}
	mux->request = dma_spec->args[0];

	dma_spec->args[3] = dma_spec->args[2];
	dma_spec->args[2] = dma_spec->args[1];
	dma_spec->args[1] = 0;
	dma_spec->args[0] = 0;
	dma_spec->args_count = 4;

	return mux;
}

static int stm32_dmamux_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct device_node *dma_node;
	struct stm32_dmamux_data *stm32_dmamux;
	struct resource *res;
	void __iomem *iomem;
	int i, ret;

	if (!node)
		return -ENODEV;

	stm32_dmamux = devm_kzalloc(&pdev->dev, sizeof(*stm32_dmamux),
				    GFP_KERNEL);
	if (!stm32_dmamux)
		return -ENOMEM;

	dma_node = of_parse_phandle(node, "dma-masters", 0);
	if (!dma_node) {
		dev_err(&pdev->dev, "Can't get DMA master node\n");
		return -ENODEV;
	}

	if (device_property_read_u32(&pdev->dev, "dma-channels",
				     &stm32_dmamux->dmamux_channels))
		stm32_dmamux->dmamux_channels = STM32_DMAMUX_MAX_CHANNELS;

	if (device_property_read_u32(&pdev->dev, "dma-requests",
				     &stm32_dmamux->dmamux_requests))
		stm32_dmamux->dmamux_requests = STM32_DMAMUX_MAX_REQUESTS;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	iomem = devm_ioremap_resource(&pdev->dev, res);
	if (!iomem)
		return -ENOMEM;

	spin_lock_init(&stm32_dmamux->lock);

	stm32_dmamux->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(stm32_dmamux->clk)) {
		dev_info(&pdev->dev, "Missing controller clock\n");
		return PTR_ERR(stm32_dmamux->clk);
	}

	stm32_dmamux->rst = devm_reset_control_get(&pdev->dev, NULL);
	if (!IS_ERR(stm32_dmamux->rst)) {
		reset_control_assert(stm32_dmamux->rst);
		udelay(2);
		reset_control_deassert(stm32_dmamux->rst);
	}

	stm32_dmamux->iomem = iomem;
	stm32_dmamux->dmarouter.dev = &pdev->dev;
	stm32_dmamux->dmarouter.route_free = stm32_dmamux_free;

	platform_set_drvdata(pdev, stm32_dmamux);

	if (!IS_ERR(stm32_dmamux->clk)) {
		ret = clk_prepare_enable(stm32_dmamux->clk);
		if (ret < 0) {
			dev_err(&pdev->dev, "clk_prep_enable issue: %d\n", ret);
			return ret;
		}
	}

	/* Reset the dmamux */
	for (i = 0; i < stm32_dmamux->dmamux_channels; i++)
		stm32_dmamux_write(stm32_dmamux->iomem, STM32_DMAMUX_CCR(i), 0);

	if (!IS_ERR(stm32_dmamux->clk))
		clk_disable(stm32_dmamux->clk);

	return of_dma_router_register(node, stm32_dmamux_route_allocate,
				     &stm32_dmamux->dmarouter);
}

static const struct of_device_id stm32_dmamux_match[] = {
	{ .compatible = "st,stm32h7-dmamux" },
	{},
};

static struct platform_driver stm32_dmamux_driver = {
	.probe	= stm32_dmamux_probe,
	.driver = {
		.name = "stm32-dmamux",
		.of_match_table = stm32_dmamux_match,
	},
};

static int __init stm32_dmamux_init(void)
{
	return platform_driver_register(&stm32_dmamux_driver);
}
arch_initcall(stm32_dmamux_init);
