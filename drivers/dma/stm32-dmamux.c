/*
 * DMA Router driver for STM32 DMA MUX
 *
 * Copyright (C) 2015 M'Boumba Cedric Madianga <cedric.madianga@gmail.com>
 *
 * Based on LPC18xx/43xx DMA MUX and TI DMA XBAR
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

#define STM32_DMAMUX_CCR(x)		(0x4 * (x))
#define STM32_DMAMUX_MAX_CHANNELS	32
#define STM32_DMAMUX_MAX_REQUESTS	255

struct stm32_dmamux {
	u32 chan_id;
	u32 request;
	bool busy;
};

struct stm32_dmamux_data {
	struct dma_router dmarouter;
	struct stm32_dmamux *muxes;
	struct clk *clk;
	void __iomem *iomem;
	u32 dmamux_requests; /* number of DMA requests connected to DMAMUX */
	u32 dmamux_channels; /* Number of DMA channels supported */
};

static inline u32 stm32_dmamux_read(void __iomem *iomem, u32 reg)
{
	return readl_relaxed(iomem + reg);
}

static inline void stm32_dmamux_write(void __iomem *iomem, u32 reg, u32 val)
{
	writel_relaxed(val, iomem + reg);
}

static void stm32_dmamux_free(struct device *dev, void *route_data)
{
	struct stm32_dmamux_data *dmamux = dev_get_drvdata(dev);
	struct stm32_dmamux *mux = route_data;

	/* Clear dma request for the right channel */
	stm32_dmamux_write(dmamux->iomem, STM32_DMAMUX_CCR(mux->chan_id), 0);
	clk_disable(dmamux->clk);
	mux->busy = false;

	dev_dbg(dev, "Unmapping dma-router%dchan%d (was routed to request%d)\n",
		dev->id, mux->chan_id, mux->request);
}

static void *stm32_dmamux_route_allocate(struct of_phandle_args *dma_spec,
					 struct of_dma *ofdma)
{
	struct platform_device *pdev = of_find_device_by_node(ofdma->of_node);
	struct stm32_dmamux_data *dmamux = platform_get_drvdata(pdev);
	struct stm32_dmamux *mux;
	u32 chan_id;
	int ret;

	if (dma_spec->args_count != 4) {
		dev_err(&pdev->dev, "invalid number of dma mux args\n");
		return ERR_PTR(-EINVAL);
	}

	if (dma_spec->args[0] >= dmamux->dmamux_channels) {
		dev_err(&pdev->dev, "invalid channel id: %d\n",
			dma_spec->args[0]);
		return ERR_PTR(-EINVAL);
	}

	if (dma_spec->args[1] > dmamux->dmamux_requests) {
		dev_err(&pdev->dev, "invalid mux request number: %d\n",
			dma_spec->args[1]);
		return ERR_PTR(-EINVAL);
	}

	/* The of_node_put() will be done in of_dma_router_xlate function */
	dma_spec->np = of_parse_phandle(ofdma->of_node, "dma-masters", 0);
	if (!dma_spec->np) {
		dev_err(&pdev->dev, "can't get dma master\n");
		return ERR_PTR(-EINVAL);
	}

	chan_id = dma_spec->args[0];
	mux = &dmamux->muxes[chan_id];
	mux->chan_id = chan_id;
	mux->request = dma_spec->args[1];

	if (mux->busy) {
		dev_err(&pdev->dev, "dma channel %d busy with request %d\n",
			chan_id, mux->request);
		return ERR_PTR(-EBUSY);
	}

	ret = clk_enable(dmamux->clk);
	if (ret < 0) {
		dev_err(&pdev->dev, "clk_enable failed: %d\n", ret);
		return ERR_PTR(ret);
	}

	stm32_dmamux_write(dmamux->iomem, STM32_DMAMUX_CCR(mux->chan_id),
			   mux->request);
	mux->busy = true;

	dev_dbg(&pdev->dev, "Mapping dma-router%dchan%d to request%d\n",
		pdev->dev.id, mux->chan_id, mux->request);

	return mux;
}

static int stm32_dmamux_probe(struct platform_device *pdev)
{
	struct device_node *dma_node, *node = pdev->dev.of_node;
	struct stm32_dmamux_data *dmamux;
	struct reset_control *rst;
	struct resource *res;
	int ret;

	if (!node)
		return -ENODEV;

	dmamux = devm_kzalloc(&pdev->dev, sizeof(struct stm32_dmamux_data),
			      GFP_KERNEL);
	if (!dmamux)
		return -ENOMEM;

	dma_node = of_parse_phandle(node, "dma-masters", 0);
	if (!dma_node) {
		dev_err(&pdev->dev, "Can't get DMA master node\n");
		return -ENODEV;
	}
	of_node_put(dma_node);

	ret = of_property_read_u32(node, "dma-channels",
				   &dmamux->dmamux_channels);
	if (ret)
		dmamux->dmamux_channels = STM32_DMAMUX_MAX_CHANNELS;

	ret = of_property_read_u32(node, "dma-requests",
				   &dmamux->dmamux_requests);
	if (ret)
		dmamux->dmamux_requests = STM32_DMAMUX_MAX_REQUESTS;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	dmamux->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (!dmamux->iomem)
		return -ENOMEM;

	dmamux->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dmamux->clk)) {
		dev_err(&pdev->dev, "Missing controller clock\n");
		return PTR_ERR(dmamux->clk);
	}
	ret = clk_prepare(dmamux->clk);
	if (ret < 0) {
		dev_err(&pdev->dev, "clk_prep failed: %d\n", ret);
		return ret;
	}

	dmamux->muxes = devm_kcalloc(&pdev->dev, dmamux->dmamux_channels,
				     sizeof(struct stm32_dmamux),
				     GFP_KERNEL);
	if (!dmamux->muxes)
		return -ENOMEM;

	rst = devm_reset_control_get(&pdev->dev, NULL);
	if (!IS_ERR(rst)) {
		ret = clk_enable(dmamux->clk);
		if (ret < 0) {
			dev_err(&pdev->dev, "clk_enable failed: %d\n", ret);
			return ret;
		}
		reset_control_assert(rst);
		udelay(2);
		reset_control_deassert(rst);
		clk_disable(dmamux->clk);
	}

	dmamux->dmarouter.dev = &pdev->dev;
	dmamux->dmarouter.route_free = stm32_dmamux_free;
	platform_set_drvdata(pdev, dmamux);

	ret = of_dma_router_register(node, stm32_dmamux_route_allocate,
				     &dmamux->dmarouter);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"STM32 DMAMUX DMA OF registration failed %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "STM32 DMAMUX driver registered\n");

	return 0;
}

static const struct of_device_id stm32_dmamux_match[] = {
	{ .compatible = "st,stm32-dmamux" },
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
