/*
 * Freescale QorIQ USB3 phy driver
 *
 * Copyright 2016 Freescale Semiconductor, Inc.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Author: Sriram Dash <sriram.dash@nxp.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/phy/phy.h>
#include <linux/usb/phy.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/usb/of.h>
#include <linux/regmap.h>


/* Parameter control */
#define USB3PRM1CR		0x000
#define USB3PRM1CR_VAL		0x27672b2a

/*
 * struct qoriq_usb3_phy - driver data for USB 3.0 PHY
 * @dev: pointer to device instance of this platform device
 * @param_ctrl: usb3 phy parameter control register base
 * @phy_base: usb3 phy register memory base
 * @has_erratum_flag: keeps track of erratum applicable on device
 */
struct qoriq_usb3_phy {
	struct device *dev;
	void __iomem *param_ctrl;
	void __iomem *phy_base;
	u32 has_erratum_flag;
};

static inline u32 qoriq_usb3_phy_readl(void __iomem *addr, u32 offset)
{
	return __raw_readl(addr + offset);
}

static inline void qoriq_usb3_phy_writel(void __iomem *addr, u32 offset,
	u32 data)
{
	__raw_writel(data, addr + offset);
}

/*
 * Erratum A008751
 * SCFG USB3PRM1CR has incorrect default value
 * SCFG USB3PRM1CR reset value should be 32'h27672B2A instead of 32'h25E72B2A.
 */
static void erratum_a008751(struct qoriq_usb3_phy *phy)
{
	qoriq_usb3_phy_writel(phy->param_ctrl, USB3PRM1CR,
				USB3PRM1CR_VAL);
}

/*
 * qoriq_usb3_phy_erratum - List of phy erratum
 * @qoriq_phy_erratum - erratum application
 * @compat - comapt string for erratum
 */

struct qoriq_usb3_phy_erratum {
	void (*qoriq_phy_erratum)(struct qoriq_usb3_phy *phy);
	char *compat;
};

/* Erratum list */
struct qoriq_usb3_phy_erratum  phy_erratum_tbl[] = {
	{&erratum_a008751, "fsl,usb-erratum-a008751"},
	/* Add init time erratum here */
};

static int qoriq_usb3_phy_init(struct phy *x)
{
	struct qoriq_usb3_phy *phy = phy_get_drvdata(x);
	int i;

	for (i = 0; i < ARRAY_SIZE(phy_erratum_tbl); i++)
		if (phy->has_erratum_flag & 1 << i)
			phy_erratum_tbl[i].qoriq_phy_erratum(phy);
	return 0;
}

static const struct phy_ops ops = {
	.init		= qoriq_usb3_phy_init,
	.owner		= THIS_MODULE,
};

static int qoriq_usb3_phy_probe(struct platform_device *pdev)
{
	struct qoriq_usb3_phy *phy;
	struct phy *generic_phy;
	struct phy_provider *phy_provider;
	const struct of_device_id *of_id;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int i, ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;
	phy->dev = dev;

	of_id = of_match_device(dev->driver->of_match_table, dev);
	if (!of_id) {
		dev_err(dev, "failed to get device match\n");
		ret = -EINVAL;
		goto err_out;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "param_ctrl");
	if (!res) {
		dev_err(dev, "failed to get param_ctrl memory\n");
		ret = -ENOENT;
		goto err_out;
	}

	phy->param_ctrl = devm_ioremap_resource(dev, res);
	if (!phy->param_ctrl) {
		dev_err(dev, "failed to remap param_ctrl memory\n");
		ret = -ENOMEM;
		goto err_out;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy_base");
	if (!res) {
		dev_err(dev, "failed to get phy_base memory\n");
		ret = -ENOENT;
		goto err_out;
	}

	phy->phy_base = devm_ioremap_resource(dev, res);
	if (!phy->phy_base) {
		dev_err(dev, "failed to remap phy_base memory\n");
		ret = -ENOMEM;
		goto err_out;
	}

	phy->has_erratum_flag = 0;
	for (i = 0; i < ARRAY_SIZE(phy_erratum_tbl); i++)
		phy->has_erratum_flag |= device_property_read_bool(dev,
						phy_erratum_tbl[i].compat) << i;

	platform_set_drvdata(pdev, phy);

	generic_phy = devm_phy_create(dev, NULL, &ops);
	if (IS_ERR(generic_phy))
		return PTR_ERR(generic_phy);

	phy_set_drvdata(generic_phy, phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider))
		return PTR_ERR(phy_provider);

	return 0;
err_out:
	return ret;
}

static const struct of_device_id qoriq_usb3_phy_dt_ids[] = {
	{
		.compatible = "fsl,qoriq-usb3-phy"
	},
	{}
};
MODULE_DEVICE_TABLE(of, qoriq_usb3_phy_dt_ids);

static struct platform_driver qoriq_usb3_phy_driver = {
	.probe		= qoriq_usb3_phy_probe,
	.driver		= {
		.name	= "qoriq_usb3_phy",
		.of_match_table = qoriq_usb3_phy_dt_ids,
	},
};

module_platform_driver(qoriq_usb3_phy_driver);

MODULE_ALIAS("platform:qoriq_usb3_phy");
MODULE_AUTHOR("Sriram Dash <sriram.dash@nxp.com>");
MODULE_DESCRIPTION("Freescale QorIQ USB3 phy driver");
MODULE_LICENSE("GPL v2");
