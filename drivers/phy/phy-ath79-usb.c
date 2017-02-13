/*
 * Copyright (C) 2015-2017 Alban Bedel <albeu@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/reset.h>

struct ath79_usb_phy {
	struct reset_control *suspend_override;
};

static int ath79_usb_phy_power_on(struct phy *phy)
{
	struct ath79_usb_phy *priv = phy_get_drvdata(phy);
	int err = 0;

	if (priv->suspend_override)
		err = reset_control_assert(priv->suspend_override);

	return err;
}

static int ath79_usb_phy_power_off(struct phy *phy)
{
	struct ath79_usb_phy *priv = phy_get_drvdata(phy);
	int err = 0;

	if (priv->suspend_override)
		err = reset_control_deassert(priv->suspend_override);

	return err;
}

static const struct phy_ops ath79_usb_phy_ops = {
	.power_on	= ath79_usb_phy_power_on,
	.power_off	= ath79_usb_phy_power_off,
	.owner		= THIS_MODULE,
};

static int ath79_usb_phy_probe(struct platform_device *pdev)
{
	struct ath79_usb_phy *priv;
	struct phy *phy;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->suspend_override = devm_reset_control_get_optional(
		&pdev->dev, "suspend-override");
	if (IS_ERR(priv->suspend_override)) {
		if (PTR_ERR(priv->suspend_override) == -ENOENT)
			priv->suspend_override = NULL;
		else
			return PTR_ERR(priv->suspend_override);
	}

	phy = devm_phy_create(&pdev->dev, NULL, &ath79_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, priv);

	return PTR_ERR_OR_ZERO(devm_of_phy_provider_register(
				&pdev->dev, of_phy_simple_xlate));
}

static const struct of_device_id ath79_usb_phy_of_match[] = {
	{ .compatible = "qca,ar7100-usb-phy" },
	{}
};
MODULE_DEVICE_TABLE(of, ath79_usb_phy_of_match);

static struct platform_driver ath79_usb_phy_driver = {
	.probe	= ath79_usb_phy_probe,
	.driver = {
		.of_match_table	= ath79_usb_phy_of_match,
		.name		= "ath79-usb-phy",
	}
};
module_platform_driver(ath79_usb_phy_driver);

MODULE_DESCRIPTION("ATH79 USB PHY driver");
MODULE_AUTHOR("Alban Bedel <albeu@free.fr>");
MODULE_LICENSE("GPL");
