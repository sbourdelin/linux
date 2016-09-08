/**
 * dwc3-octeon.c - Cavium OCTEON III DWC3 Specific Glue Layer
 *
 * Copyright (C) 2016 Cavium Networks
 *
 * Author: Steven J. Hill <steven.hill@cavium.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Inspired by dwc3-exynos.c and dwc3-st.c files.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>

struct dwc3_octeon {
	struct device		*dev;
	void __iomem		*usbctl;
	int			index;
};

static int dwc3_octeon_probe(struct platform_device *pdev)
{
	struct device		*dev = &pdev->dev;
	struct resource		*res;
	struct dwc3_octeon	*octeon;
	int			ret;

	octeon = devm_kzalloc(dev, sizeof(*octeon), GFP_KERNEL);
	if (!octeon)
		return - ENOMEM;

	/*
	 * Right now device-tree probed devices don't get dma_mask set.
	 * Since shared usb code relies on it, set it here for now.
	 */
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	platform_set_drvdata(pdev, octeon);
	octeon->dev = dev;

	/* Resources for lower level OCTEON USB control. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	octeon->usbctl = devm_ioremap_resource(dev, res);
	if (IS_ERR(octeon->usbctl))
		return PTR_ERR(octeon->usbctl);

	/* Controller index. */
	octeon->index = ((u64)octeon->usbctl >> 24) & 1;

	return 0;
}

static int dwc3_octeon_remove(struct platform_device *pdev)
{
	struct dwc3_octeon *octeon = platform_get_drvdata(pdev);

	octeon->usbctl = NULL;
	octeon->index = -1;

	return 0;
}

static const struct of_device_id octeon_dwc3_match[] = {
	{ .compatible = "cavium,octeon-7130-usb-uctl", },
	{},
};
MODULE_DEVICE_TABLE(of, octeon_dwc3_match);

static struct platform_driver dwc3_octeon_driver = {
	.probe		= dwc3_octeon_probe,
	.remove		= dwc3_octeon_remove,
	.driver		= {
		.name	= "octeon-dwc3",
		.of_match_table = octeon_dwc3_match,
		.pm	= NULL,
	},
};
module_platform_driver(dwc3_octeon_driver);

MODULE_ALIAS("platform:octeon-dwc3");
MODULE_AUTHOR("Steven J. Hill <steven.hill@cavium.com");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 OCTEON Glue Layer");
