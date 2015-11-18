/**
 * dwc3-xilinx.c - Xilinx ZynqMP specific Glue layer
 *
 * Copyright (C) 2015 Xilinx Inc.
 *
 * Author: Subbaraya Sundeep <sbhatta@xilinx.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

/**
 * struct xilinx_dwc3 - dwc3 xilinx glue structure
 * @dev: device pointer
 * @ref_clk: clock input to core during PHY power down
 * @bus_clk: bus clock input to core
 */
struct xilinx_dwc3 {
	struct device	*dev;
	struct clk	*ref_clk;
	struct clk	*bus_clk;
};

/**
 * xilinx_dwc3_probe - The device probe function for driver initialization.
 * @pdev: pointer to the platform device structure.
 *
 * Return: 0 for success and error value on failure
 */
static int xilinx_dwc3_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct xilinx_dwc3 *xdwc3;
	int ret;

	xdwc3 = devm_kzalloc(&pdev->dev, sizeof(*xdwc3), GFP_KERNEL);
	if (!xdwc3)
		return -ENOMEM;

	xdwc3->dev = &pdev->dev;

	xdwc3->bus_clk = devm_clk_get(xdwc3->dev, "bus_clk");
	if (IS_ERR(xdwc3->bus_clk)) {
		dev_err(xdwc3->dev, "unable to get usb bus clock");
		return PTR_ERR(xdwc3->bus_clk);
	}

	xdwc3->ref_clk = devm_clk_get(xdwc3->dev, "ref_clk");
	if (IS_ERR(xdwc3->ref_clk)) {
		dev_err(xdwc3->dev, "unable to get usb ref clock");
		return PTR_ERR(xdwc3->ref_clk);
	}

	ret = clk_prepare_enable(xdwc3->bus_clk);
	if (ret)
		goto err_bus_clk;
	ret = clk_prepare_enable(xdwc3->ref_clk);
	if (ret)
		goto err_ref_clk;

	platform_set_drvdata(pdev, xdwc3);

	ret = of_platform_populate(node, NULL, NULL, xdwc3->dev);
	if (ret) {
		dev_err(xdwc3->dev, "failed to create dwc3 core\n");
		goto err_dwc3_core;
	}

	return 0;

err_dwc3_core:
	clk_disable_unprepare(xdwc3->ref_clk);
err_ref_clk:
	clk_disable_unprepare(xdwc3->bus_clk);
err_bus_clk:
	return ret;
}

/**
 * xilinx_dwc3_remove - Releases the resources allocated during initialization.
 * @pdev: pointer to the platform device structure.
 *
 * Return: 0 always
 */
static int xilinx_dwc3_remove(struct platform_device *pdev)
{
	struct xilinx_dwc3 *xdwc3 = platform_get_drvdata(pdev);

	of_platform_depopulate(xdwc3->dev);

	clk_disable_unprepare(xdwc3->bus_clk);
	clk_disable_unprepare(xdwc3->ref_clk);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

/* Match table for of_platform binding */
static const struct of_device_id xilinx_dwc3_of_match[] = {
	{ .compatible = "xlnx,zynqmp-dwc3", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, xilinx_dwc3_of_match);

static struct platform_driver xilinx_dwc3_driver = {
	.probe		= xilinx_dwc3_probe,
	.remove		= xilinx_dwc3_remove,
	.driver		= {
		.name		= "xilinx-dwc3",
		.of_match_table	= xilinx_dwc3_of_match,
	},
};

module_platform_driver(xilinx_dwc3_driver);

MODULE_AUTHOR("Xilinx Inc.");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 Xilinx Glue Layer");
