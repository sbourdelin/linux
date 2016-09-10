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

static int dwc3_octeon_probe(struct platform_device *pdev)
{
	struct device		*dev = &pdev->dev;
	int			ret;

	/*
	 * Right now device-tree probed devices do not provide
	 * "dma-ranges" or "dma-coherent" properties.
	 */
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id octeon_dwc3_match[] = {
	{ .compatible = "cavium,octeon-7130-usb-uctl", },
	{},
};
MODULE_DEVICE_TABLE(of, octeon_dwc3_match);

static struct platform_driver dwc3_octeon_driver = {
	.probe		= dwc3_octeon_probe,
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
