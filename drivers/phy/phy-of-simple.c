// SPDX-License-Identifier: GPL-2.0
/*
 * phy-of-simple.c - phy driver for simple implementations
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com
 *
 */
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

static int phy_simple_power_on(struct phy *phy)
{
	if (phy->pwr)
		return regulator_enable(phy->pwr);

	return 0;
}

static int phy_simple_power_off(struct phy *phy)
{
	if (phy->pwr)
		return regulator_disable(phy->pwr);

	return 0;
}

static const struct phy_ops phy_simple_ops = {
	.power_on	= phy_simple_power_on,
	.power_off	= phy_simple_power_off,
	.owner		= THIS_MODULE,
};

int phy_simple_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct regulator *pwr = NULL;
	struct phy *phy;
	u32 bus_width = 0;
	u32 max_bitrate = 0;
	int ret;

	phy = devm_phy_create(dev, dev->of_node,
				      &phy_simple_ops);

	if (IS_ERR(phy)) {
		dev_err(dev, "Failed to create phy\n");
		return PTR_ERR(phy);
	}

	device_property_read_u32(dev, "bus-width", &bus_width);
	phy->attrs.bus_width = bus_width;
	device_property_read_u32(dev, "max-bitrate", &max_bitrate);
	phy->attrs.max_bitrate = max_bitrate;

	pwr = devm_regulator_get_optional(dev, "pwr");
	if (IS_ERR(pwr)) {
		ret = PTR_ERR(pwr);
		dev_err(dev, "Couldn't get regulator. ret=%d\n", ret);
		return ret;
	}
	phy->pwr = pwr;

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id phy_simple_dt_ids[] = {
	{ .compatible = "simple-phy"},
	{}
};

MODULE_DEVICE_TABLE(of, phy_simple_phy_dt_ids);

static struct platform_driver phy_simple_driver = {
	.probe		= phy_simple_probe,
	.driver		= {
		.name	= "phy-of-simple",
		.of_match_table = phy_simple_dt_ids,
	},
};

module_platform_driver(phy_simple_driver);

MODULE_AUTHOR("Faiz Abbas <faiz_abbas@ti.com>");
MODULE_DESCRIPTION("Simple PHY driver");
MODULE_LICENSE("GPL v2");
