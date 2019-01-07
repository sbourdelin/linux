/*
 * Broadcom Northstar USB 2.0 PHY Driver
 *
 * Copyright (C) 2016 Rafał Miłecki <zajec5@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/bcma/bcma.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CRU_USB2_CONTROL			0x64
#define CRU_CLKSET_KEY				0x80

struct bcm_ns_usb2 {
	struct device *dev;
	struct clk *ref_clk;
	struct phy *phy;
	struct regmap *cru;
	void __iomem *dmu;
};

static int bcm_ns_usb2_phy_init(struct phy *phy)
{
	struct bcm_ns_usb2 *usb2 = phy_get_drvdata(phy);
	struct device *dev = usb2->dev;
	struct regmap *cru = usb2->cru;
	void __iomem *dmu = usb2->dmu;
	u32 ref_clk_rate, usb2ctl, usb_pll_ndiv, usb_pll_pdiv;
	int err = 0;

	err = clk_prepare_enable(usb2->ref_clk);
	if (err < 0) {
		dev_err(dev, "Failed to prepare ref clock: %d\n", err);
		goto err_out;
	}

	ref_clk_rate = clk_get_rate(usb2->ref_clk);
	if (!ref_clk_rate) {
		dev_err(dev, "Failed to get ref clock rate\n");
		err = -EINVAL;
		goto err_clk_off;
	}

	if (cru)
		regmap_read(cru, CRU_USB2_CONTROL, &usb2ctl);
	else
		usb2ctl = readl(dmu + BCMA_DMU_CRU_USB2_CONTROL);

	if (usb2ctl & BCMA_DMU_CRU_USB2_CONTROL_USB_PLL_PDIV_MASK) {
		usb_pll_pdiv = usb2ctl;
		usb_pll_pdiv &= BCMA_DMU_CRU_USB2_CONTROL_USB_PLL_PDIV_MASK;
		usb_pll_pdiv >>= BCMA_DMU_CRU_USB2_CONTROL_USB_PLL_PDIV_SHIFT;
	} else {
		usb_pll_pdiv = 1 << 3;
	}

	/* Calculate ndiv based on a solid 1920 MHz that is for USB2 PHY */
	usb_pll_ndiv = (1920000000 * usb_pll_pdiv) / ref_clk_rate;

	/* Unlock DMU PLL settings with some magic value */
	if (cru)
		regmap_write(cru, CRU_CLKSET_KEY, 0x0000ea68);
	else
		writel(0x0000ea68, dmu + BCMA_DMU_CRU_CLKSET_KEY);

	/* Write USB 2.0 PLL control setting */
	usb2ctl &= ~BCMA_DMU_CRU_USB2_CONTROL_USB_PLL_NDIV_MASK;
	usb2ctl |= usb_pll_ndiv << BCMA_DMU_CRU_USB2_CONTROL_USB_PLL_NDIV_SHIFT;
	if (cru)
		regmap_write(cru, CRU_USB2_CONTROL, usb2ctl);
	else
		writel(usb2ctl, dmu + BCMA_DMU_CRU_USB2_CONTROL);

	/* Lock DMU PLL settings */
	if (cru)
		regmap_write(cru, CRU_CLKSET_KEY, 0x00000000);
	else
		writel(0x00000000, dmu + BCMA_DMU_CRU_CLKSET_KEY);

err_clk_off:
	clk_disable_unprepare(usb2->ref_clk);
err_out:
	return err;
}

static const struct phy_ops ops = {
	.init		= bcm_ns_usb2_phy_init,
	.owner		= THIS_MODULE,
};

static int bcm_ns_usb2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *cru_np;
	struct bcm_ns_usb2 *usb2;
	struct resource *res;
	struct phy_provider *phy_provider;

	usb2 = devm_kzalloc(&pdev->dev, sizeof(*usb2), GFP_KERNEL);
	if (!usb2)
		return -ENOMEM;
	usb2->dev = dev;

	cru_np = of_parse_phandle(dev->of_node, "syscon-cru", 0);
	usb2->cru = syscon_node_to_regmap(cru_np);
	if (IS_ERR(usb2->cru)) {
		usb2->cru = NULL;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dmu");
		usb2->dmu = devm_ioremap_resource(dev, res);
		if (IS_ERR(usb2->dmu)) {
			dev_err(dev, "Failed to map DMU regs\n");
			return PTR_ERR(usb2->dmu);
		}
	}

	usb2->ref_clk = devm_clk_get(dev, "phy-ref-clk");
	if (IS_ERR(usb2->ref_clk)) {
		dev_err(dev, "Clock not defined\n");
		return PTR_ERR(usb2->ref_clk);
	}

	usb2->phy = devm_phy_create(dev, NULL, &ops);
	if (IS_ERR(usb2->phy))
		return PTR_ERR(usb2->phy);

	phy_set_drvdata(usb2->phy, usb2);
	platform_set_drvdata(pdev, usb2);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id bcm_ns_usb2_id_table[] = {
	{ .compatible = "brcm,ns-usb2-phy", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm_ns_usb2_id_table);

static struct platform_driver bcm_ns_usb2_driver = {
	.probe		= bcm_ns_usb2_probe,
	.driver = {
		.name = "bcm_ns_usb2",
		.of_match_table = bcm_ns_usb2_id_table,
	},
};
module_platform_driver(bcm_ns_usb2_driver);

MODULE_LICENSE("GPL v2");
