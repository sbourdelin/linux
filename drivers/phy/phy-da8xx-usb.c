/*
 * phy-da8xx-usb - TI DaVinci DA8xx USB PHY driver
 *
 * Copyright (C) 2016 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/mfd/da8xx-cfgchip.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-da8xx-usb.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>

struct da8xx_usbphy {
	struct phy_provider	*phy_provider;
	struct phy		*usb11_phy;
	struct phy		*usb20_phy;
	struct clk		*usb11_clk;
	struct clk		*usb20_clk;
	struct regmap		*regmap;
};

static int da8xx_usb11_phy_power_on(struct phy *phy)
{
	struct da8xx_usbphy *d_phy = phy_get_drvdata(phy);
	int ret;

	ret = clk_prepare_enable(d_phy->usb11_clk);
	if (ret)
		return ret;

	regmap_write_bits(d_phy->regmap, CFGCHIP2_REG, CFGCHIP2_USB1SUSPENDM,
			  CFGCHIP2_USB1SUSPENDM);

	return 0;
}

static int da8xx_usb11_phy_power_off(struct phy *phy)
{
	struct da8xx_usbphy *d_phy = phy_get_drvdata(phy);

	regmap_write_bits(d_phy->regmap, CFGCHIP2_REG, CFGCHIP2_USB1SUSPENDM, 0);

	clk_disable_unprepare(d_phy->usb11_clk);

	return 0;
}

static const struct phy_ops da8xx_usb11_phy_ops = {
	.power_on	= da8xx_usb11_phy_power_on,
	.power_off	= da8xx_usb11_phy_power_off,
	.owner		= THIS_MODULE,
};

static int da8xx_usb20_phy_init(struct phy *phy)
{
	struct da8xx_usbphy *d_phy = phy_get_drvdata(phy);
	int ret;

	ret = clk_prepare_enable(d_phy->usb20_clk);
	if (ret)
		return ret;

	regmap_write_bits(d_phy->regmap, CFGCHIP2_REG, CFGCHIP2_OTGPWRDN, 0);

	return 0;
}

static int da8xx_usb20_phy_shutdown(struct phy *phy)
{
	struct da8xx_usbphy *d_phy = phy_get_drvdata(phy);

	regmap_write_bits(d_phy->regmap, CFGCHIP2_REG, CFGCHIP2_OTGPWRDN,
			  CFGCHIP2_OTGPWRDN);

	clk_disable_unprepare(d_phy->usb20_clk);

	return 0;
}

static const struct phy_ops da8xx_usb20_phy_ops = {
	.power_on	= da8xx_usb20_phy_init,
	.power_off	= da8xx_usb20_phy_shutdown,
	.owner		= THIS_MODULE,
};

int da8xx_usb20_phy_set_mode(struct phy *phy, enum musb_mode mode)
{
	struct da8xx_usbphy *d_phy = phy_get_drvdata(phy);
	u32 val;

	switch (mode) {
	case MUSB_HOST:		/* Force VBUS valid, ID = 0 */
		val = CFGCHIP2_FORCE_HOST;
		break;
	case MUSB_PERIPHERAL:	/* Force VBUS valid, ID = 1 */
		val = CFGCHIP2_FORCE_DEVICE;
		break;
	case MUSB_OTG:		/* Don't override the VBUS/ID comparators */
		val = CFGCHIP2_NO_OVERRIDE;
		break;
	default:
		return -EINVAL;
	}

	regmap_write_bits(d_phy->regmap, CFGCHIP2_REG, CFGCHIP2_OTGMODE_MASK,
			  val);

	return 0;
}
EXPORT_SYMBOL_GPL(da8xx_usb20_phy_set_mode);

static struct phy *da8xx_usbphy_of_xlate(struct device *dev,
					 struct of_phandle_args *args)
{
	struct da8xx_usbphy *d_phy = dev_get_drvdata(dev);

	if (!d_phy)
		return ERR_PTR(-ENODEV);

	switch (args->args[0]) {
	case 1:
		return d_phy->usb11_phy;
	case 2:
		return d_phy->usb20_phy;
	default:
		return ERR_PTR(-EINVAL);
	}
}

static int da8xx_usbphy_probe(struct platform_device *pdev)
{
	struct device		*dev = &pdev->dev;
	struct device_node	*node = dev->of_node;
	struct da8xx_usbphy	*d_phy;

	d_phy = devm_kzalloc(dev, sizeof(*d_phy), GFP_KERNEL);
	if (!d_phy)
		return -ENOMEM;

	if (node)
		d_phy->regmap = syscon_regmap_lookup_by_compatible(
							"ti,da830-cfgchip");
	else
		d_phy->regmap = syscon_regmap_lookup_by_pdevname("syscon.0");
	if (IS_ERR(d_phy->regmap)) {
		dev_err(dev, "Failed to get syscon.\n");
		return PTR_ERR(d_phy->regmap);
	}

	d_phy->usb11_clk = devm_clk_get(dev, "usb11_phy");
	if (IS_ERR(d_phy->usb11_clk)) {
		dev_err(dev, "Failed to get usb11_phy clock.\n");
		return PTR_ERR(d_phy->usb11_clk);
	}

	d_phy->usb20_clk = devm_clk_get(dev, "usb20_phy");
	if (IS_ERR(d_phy->usb20_clk)) {
		dev_err(dev, "Failed to get usb20_phy clock.\n");
		return PTR_ERR(d_phy->usb20_clk);
	}

	d_phy->usb11_phy = devm_phy_create(dev, node, &da8xx_usb11_phy_ops);
	if (IS_ERR(d_phy->usb11_phy)) {
		dev_err(dev, "Failed to create usb11 phy.\n");
		return PTR_ERR(d_phy->usb11_phy);
	}

	d_phy->usb20_phy = devm_phy_create(dev, node, &da8xx_usb20_phy_ops);
	if (IS_ERR(d_phy->usb20_phy)) {
		dev_err(dev, "Failed to create usb20 phy.\n");
		return PTR_ERR(d_phy->usb20_phy);
	}

	platform_set_drvdata(pdev, d_phy);
	phy_set_drvdata(d_phy->usb11_phy, d_phy);
	phy_set_drvdata(d_phy->usb20_phy, d_phy);

	if (node) {
		d_phy->phy_provider = devm_of_phy_provider_register(dev,
							da8xx_usbphy_of_xlate);
		if (IS_ERR(d_phy->phy_provider)) {
			dev_err(dev, "Failed to create phy provider.\n");
			return PTR_ERR(d_phy->phy_provider);
		}
	} else {
		int ret;

		ret = phy_create_lookup(d_phy->usb11_phy, "usbphy", "ohci.0");
		if (ret)
			dev_warn(dev, "Failed to create usb11 phy lookup .\n");
		ret = phy_create_lookup(d_phy->usb20_phy, "usbphy", "musb-da8xx");
		if (ret)
			dev_warn(dev, "Failed to create usb20 phy lookup .\n");
	}

	return 0;
}

static int da8xx_usbphy_remove(struct platform_device *pdev)
{
	struct da8xx_usbphy *d_phy = platform_get_drvdata(pdev);

	if (!pdev->dev.of_node) {
		phy_remove_lookup(d_phy->usb20_phy, "usbphy", "musb-da8xx");
		phy_remove_lookup(d_phy->usb11_phy, "usbphy", "ohci.0");
	}

	return 0;
}

static const struct of_device_id da8xx_usbphy_ids[] = {
	{ .compatible = "ti,da830-usbphy" },
	{ }
};
MODULE_DEVICE_TABLE(of, da8xx_usbphy_ids);

static struct platform_driver da8xx_usbphy_driver = {
	.probe	= da8xx_usbphy_probe,
	.remove	= da8xx_usbphy_remove,
	.driver	= {
		.name	= "da8xx-usbphy",
		.of_match_table = da8xx_usbphy_ids,
	},
};

module_platform_driver(da8xx_usbphy_driver);

MODULE_ALIAS("platform:da8xx-usbphy");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_DESCRIPTION("TI DA8xx USB PHY driver");
MODULE_LICENSE("GPL v2");
