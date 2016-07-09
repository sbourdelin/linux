/*
 * QorIQ SoC USB 2.0 PHY driver
 *
 * Copyright 2016 Freescale Semiconductor, Inc.
 * Author: Rajesh Bhagat <rajesh.bhagat@nxp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/usb/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/usb/phy.h>
#include <linux/usb/ulpi.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "phy-qoriq-usb2.h"

static int qoriq_usb2_phy_init(struct phy *_phy)
{
	struct qoriq_usb2_phy_ctx *ctx = phy_get_drvdata(_phy);
	struct device *dev = ctx->dev;

	if (ctx->ulpi_phy) {
		if (usb_phy_init(ctx->ulpi_phy)) {
			dev_err(dev, "unable to init transceiver, probably missing\n");
			return -ENODEV;
		}
	}

	return 0;
}

static int qoriq_usb2_phy_power_on(struct phy *_phy)
{
	struct qoriq_usb2_phy_ctx *ctx = phy_get_drvdata(_phy);
	u32 flags;

	if (ctx->ulpi_phy) {
		flags = usb_phy_io_read(ctx->ulpi_phy, ULPI_OTG_CTRL);
		usb_phy_io_write(ctx->ulpi_phy, flags |
				 (ULPI_OTG_CTRL_DRVVBUS_EXT |
				 ULPI_OTG_CTRL_EXTVBUSIND), ULPI_OTG_CTRL);
		flags = usb_phy_io_read(ctx->ulpi_phy, ULPI_IFC_CTRL);
		usb_phy_io_write(ctx->ulpi_phy, flags |
				 (ULPI_IFC_CTRL_EXTERNAL_VBUS |
				 ULPI_IFC_CTRL_PASSTHRU), ULPI_IFC_CTRL);
	}

	return 0;
}

static int qoriq_usb2_phy_power_off(struct phy *_phy)
{
	/* TODO: Add logic to power off phy */

	return 0;
}

static int qoriq_usb2_phy_exit(struct phy *_phy)
{
	struct qoriq_usb2_phy_ctx *ctx = phy_get_drvdata(_phy);

	if (ctx->ulpi_phy)
		usb_phy_shutdown(ctx->ulpi_phy);

	return 0;
}

static const struct phy_ops ops = {
	.init		= qoriq_usb2_phy_init,
	.power_on	= qoriq_usb2_phy_power_on,
	.power_off	= qoriq_usb2_phy_power_off,
	.exit		= qoriq_usb2_phy_exit,
	.owner		= THIS_MODULE,
};


static enum qoriq_usb2_phy_ver of_usb_get_phy_version(struct device_node *np)
{
	enum qoriq_usb2_phy_ver phy_version = QORIQ_PHY_UNKNOWN;

	if (of_device_is_compatible(np, "fsl,qoriq-usb2-phy")) {
		if (of_device_is_compatible(np, "fsl,qoriq-usb2-phy-v1.0"))
			phy_version = QORIQ_PHY_LEGACY;
		else if (of_device_is_compatible(np, "fsl,qoriq-usb2-phy-v2.0"))
			phy_version = QORIQ_PHY_NXP_ISP1508;
	}
	return phy_version;
}

static int qoriq_usb2_phy_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;
	struct qoriq_usb2_phy_ctx *ctx;
	struct device *dev = &pdev->dev;
	const struct of_device_id *of_id;
	struct phy_provider *phy_provider;
	struct device_node *np = pdev->dev.of_node;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;

	of_id = of_match_device(dev->driver->of_match_table, dev);
	if (!of_id) {
		dev_err(dev, "failed to get device match\n");
		ret = -EINVAL;
		goto err_out;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to get I/O memory\n");
		ret = -ENOENT;
		goto err_out;
	}

	ctx->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!ctx->regs) {
		dev_err(dev, "failed to remap I/O memory\n");
		ret = -ENOMEM;
		goto err_out;
	}

	platform_set_drvdata(pdev, ctx);

	ctx->phy = devm_phy_create(ctx->dev, NULL, &ops);
	if (IS_ERR(ctx->phy)) {
		dev_err(dev, "failed to create PHY\n");
		ret = PTR_ERR(ctx->phy);
		goto err_out;
	}
	phy_set_drvdata(ctx->phy, ctx);

	ctx->phy_version = of_usb_get_phy_version(np);
	if (ctx->phy_version == QORIQ_PHY_UNKNOWN) {
		ret = -EINVAL;
		dev_err(dev, "failed to get PHY version\n");
		goto err_out;
	}

	ctx->phy_type = of_usb_get_phy_mode(np);
	switch (ctx->phy_type) {
	case USBPHY_INTERFACE_MODE_ULPI:
		switch (ctx->phy_version) {
		case QORIQ_PHY_NXP_ISP1508:
			ctx->ulpi_phy = qoriq_otg_ulpi_create(0);
			if (!ctx->ulpi_phy) {
				dev_err(dev, "qoriq_otg_ulpi_create returned NULL\n");
				ret = -ENOMEM;
				goto err_out;
			}
			ctx->ulpi_phy->io_priv = ctx->regs + ULPI_VIEWPORT;
			break;
		default:
			ctx->ulpi_phy = NULL;
			break;
		}
		break;
	default:
		dev_err(&pdev->dev, "phy_type %d is invalid or unsupported\n",
			ctx->phy_type);
		ret = -EINVAL;
		goto err_out;
	}

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		dev_err(dev, "failed to register phy_provider\n");
		ret = PTR_ERR_OR_ZERO(phy_provider);
		goto err_out;
	}

	dev_dbg(dev, "initialized\n");
	return 0;

err_out:
	return ret;
}

static int qoriq_usb2_phy_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qoriq_usb2_phy_ctx *ctx = platform_get_drvdata(pdev);

	devm_phy_destroy(ctx->dev, ctx->phy);
	devm_iounmap(dev, ctx->regs);
	dev_dbg(dev, "de-initialized\n");
	return 0;
}

static const struct of_device_id qoriq_usb2_phy_dt_ids[] = {
	{ .compatible = "fsl,qoriq-usb2-phy"},
	{}
};

MODULE_DEVICE_TABLE(of, qoriq_usb2_phy_dt_ids);

static struct platform_driver qoriq_usb2_phy_driver = {
	.probe		= qoriq_usb2_phy_probe,
	.remove		= qoriq_usb2_phy_remove,
	.driver		= {
		.name	= "qoriq_usb2_phy",
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(qoriq_usb2_phy_dt_ids),
	},
};

module_platform_driver(qoriq_usb2_phy_driver);

MODULE_ALIAS("platform:qoriq-usb2-phy");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QorIQ SoC USB PHY driver");
MODULE_AUTHOR("Rajesh Bhagat <rajesh.bhagat@nxp.com>");
