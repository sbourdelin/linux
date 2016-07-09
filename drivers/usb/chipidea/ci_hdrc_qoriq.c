/*
 * QorIQ SoC USB 2.0 Controller driver
 *
 * Copyright 2016 Freescale Semiconductor, Inc.
 * Author: Rajesh Bhagat <rajesh.bhagat@nxp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>
#include <linux/usb/of.h>
#include <linux/usb/chipidea.h>
#include <linux/clk.h>

#include "ci.h"
#include "ci_hdrc_qoriq.h"

struct ci_hdrc_qoriq_data {
	struct phy *phy;
	struct clk *clk;
	void __iomem *qoriq_regs;
	struct platform_device *ci_pdev;
	enum usb_phy_interface phy_mode;
};

/*
 * clock helper functions
 */
static int ci_hdrc_qoriq_get_clks(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct ci_hdrc_qoriq_data *data = platform_get_drvdata(pdev);

	data->clk = devm_clk_get(dev, "usb2-clock");
	if (IS_ERR(data->clk)) {
		dev_err(dev, "failed to get clk, err=%ld\n",
					PTR_ERR(data->clk));
			return ret;
	}
	return 0;
}

static int ci_hdrc_qoriq_prepare_enable_clks(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct ci_hdrc_qoriq_data *data = platform_get_drvdata(pdev);

	ret = clk_prepare_enable(data->clk);
	if (ret) {
		dev_err(dev, "failed to prepare/enable clk, err=%d\n", ret);
		return ret;
	}
	return 0;
}

static void ci_hdrc_qoriq_disable_unprepare_clks(struct platform_device *pdev)
{
	struct ci_hdrc_qoriq_data *data = platform_get_drvdata(pdev);

	clk_disable_unprepare(data->clk);
}

static int ci_hdrc_qoriq_usb_setup(struct platform_device *pdev)
{
	u32 reg;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct ci_hdrc_qoriq_data *data = platform_get_drvdata(pdev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to get I/O memory\n");
		return -ENOENT;
	}

	dev_dbg(dev, "res->start %llx, resource_size(res) %llx\n", res->start,
		resource_size(res));
	data->qoriq_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(data->qoriq_regs)) {
		dev_err(dev, "failed to remap I/O memory\n");
		return -ENOMEM;
	}

	data->phy_mode = of_usb_get_phy_mode(pdev->dev.of_node);
	dev_dbg(dev, "phy_mode %d\n", data->phy_mode);

	reg = ioread32be(data->qoriq_regs + QORIQ_SOC_USB_CTRL);
	switch (data->phy_mode) {
	case USBPHY_INTERFACE_MODE_ULPI:
		iowrite32be(reg | ~UTMI_PHY_EN,
			    data->qoriq_regs + QORIQ_SOC_USB_CTRL);
		reg = ioread32be(data->qoriq_regs + QORIQ_SOC_USB_CTRL);
		iowrite32be(reg | USB_CTRL_USB_EN,
			    data->qoriq_regs + QORIQ_SOC_USB_CTRL);
		break;
	default:
		dev_err(dev, "unsupported phy_mode %d\n", data->phy_mode);
		return -EINVAL;
	}

	/* Setup Snooping for all the 4GB space */
	/* SNOOP1 starts from 0x0, size 2G */
	iowrite32be(SNOOP_SIZE_2GB, data->qoriq_regs + QORIQ_SOC_USB_SNOOP1);
	/* SNOOP2 starts from 0x80000000, size 2G */
	iowrite32be(SNOOP_SIZE_2GB | 0x80000000,
		data->qoriq_regs + QORIQ_SOC_USB_SNOOP2);

	iowrite32be(PRICTRL_PRI_LVL, data->qoriq_regs + QORIQ_SOC_USB_PRICTRL);
	iowrite32be(AGECNTTHRSH_THRESHOLD, data->qoriq_regs +
		    QORIQ_SOC_USB_AGECNTTHRSH);
	iowrite32be(SICTRL_RD_PREFETCH_32_BYTE, data->qoriq_regs +
		    QORIQ_SOC_USB_SICTRL);

	devm_iounmap(dev, data->qoriq_regs);
	return 0;
}

static int ci_hdrc_qoriq_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct ci_hdrc_qoriq_data *data;
	struct ci_hdrc_platform_data pdata = {
		.name		= dev_name(dev),
		.capoffset	= DEF_CAPOFFSET,
		.flags		= CI_HDRC_DISABLE_STREAMING,
	};

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);

	ret = ci_hdrc_qoriq_get_clks(pdev);
	if (ret)
		goto err_out;

	ret = ci_hdrc_qoriq_prepare_enable_clks(pdev);
	if (ret)
		goto err_out;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "failed to set coherent dma mask, err=%d\n", ret);
		goto err_clks;
	}

	ret = ci_hdrc_qoriq_usb_setup(pdev);
	if (ret) {
		dev_err(dev, "failed to perform qoriq_usb2 setup, err=%d\n",
			ret);
		goto err_clks;
	}

	data->phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(data->phy)) {
		ret = PTR_ERR(data->phy);
		/* Return -EINVAL if no usbphy is available */
		if (ret == -ENODEV)
			ret = -EINVAL;
		dev_err(dev, "failed get phy device, err=%d\n", ret);
		goto err_clks;
	}
	pdata.phy = data->phy;

	data->ci_pdev = ci_hdrc_add_device(dev,
				pdev->resource, pdev->num_resources,
				&pdata);
	if (IS_ERR(data->ci_pdev)) {
		ret = PTR_ERR(data->ci_pdev);
		dev_err(dev,
			"failed to register ci_hdrc platform device, err=%d\n",
			ret);
		goto err_clks;
	}

	pm_runtime_no_callbacks(dev);
	pm_runtime_enable(dev);

	dev_dbg(dev, "initialized\n");
	return 0;

err_clks:
	ci_hdrc_qoriq_disable_unprepare_clks(pdev);
err_out:
	return ret;
}

static int ci_hdrc_qoriq_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ci_hdrc_qoriq_data *data = platform_get_drvdata(pdev);

	pm_runtime_disable(dev);
	ci_hdrc_remove_device(data->ci_pdev);
	ci_hdrc_qoriq_disable_unprepare_clks(pdev);
	dev_dbg(dev, "de-initialized\n");
	return 0;
}

static void ci_hdrc_qoriq_shutdown(struct platform_device *pdev)
{
	ci_hdrc_qoriq_remove(pdev);
}

static const struct of_device_id ci_hdrc_qoriq_dt_ids[] = {
	{ .compatible = "fsl,ci-qoriq-usb2"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ci_hdrc_qoriq_dt_ids);

static struct platform_driver ci_hdrc_qoriq_driver = {
	.probe = ci_hdrc_qoriq_probe,
	.remove = ci_hdrc_qoriq_remove,
	.shutdown = ci_hdrc_qoriq_shutdown,
	.driver = {
		.name = "ci_qoriq_usb2",
		.of_match_table = ci_hdrc_qoriq_dt_ids,
	 },
};

module_platform_driver(ci_hdrc_qoriq_driver);

MODULE_ALIAS("platform:ci-qoriq-usb2");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CI HDRC QORIQ USB binding");
MODULE_AUTHOR("Rajesh Bhagat <rajesh.bhagat@nxp.com>");
