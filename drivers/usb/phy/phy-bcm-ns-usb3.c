/*
 * Broadcom Northstar USB 3.0 PHY Driver
 *
 * Copyright (C) 2016 Rafał Miłecki <zajec5@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/bcma/bcma.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/usb/otg.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>

#define BCM_NS_USB3_MII_MNG_TIMEOUT	1000	/* us */

struct bcm_ns_usb3 {
	struct device *dev;
	struct bcma_bus *bus;
	struct bcma_device *core;
	struct usb_phy phy;
};

static inline struct bcm_ns_usb3 *phy_to_usb3(struct usb_phy *phy)
{
	return container_of(phy, struct bcm_ns_usb3, phy);
}

static bool bcm_ns_usb3_wait_reg(struct bcm_ns_usb3 *usb3, void __iomem *addr,
				 u32 mask, u32 value, int timeout)
{
	unsigned long deadline = jiffies + timeout;
	u32 val;

	do {
		val = readl(addr);
		if ((val & mask) == value)
			return true;
		cpu_relax();
		udelay(10);
	} while (!time_after_eq(jiffies, deadline));

	dev_err(usb3->dev, "Timeout waiting for register %p\n", addr);

	return false;
}

static inline bool bcm_ns_usb3_mii_mng_wait_idle(struct bcm_ns_usb3 *usb3)
{
	struct bcma_drv_cc_b *ccb = &usb3->bus->drv_cc_b;

	return bcm_ns_usb3_wait_reg(usb3, ccb->mii + BCMA_CCB_MII_MNG_CTL,
				    0x0100, 0x0000,
				    BCM_NS_USB3_MII_MNG_TIMEOUT);
}

static void bcm_ns_usb3_mii_mng_write32(struct bcm_ns_usb3 *usb3, u32 value)
{
	struct bcma_drv_cc_b *ccb = &usb3->bus->drv_cc_b;

	bcm_ns_usb3_mii_mng_wait_idle(usb3);

	iowrite32(value, ccb->mii + BCMA_CCB_MII_MNG_CMD_DATA);
}

static void bcm_ns_usb3_phy_init_ns_bx(struct bcm_ns_usb3 *usb3)
{
	struct bcma_drv_cc_b *ccb = &usb3->bus->drv_cc_b;

	/* Enable MDIO. Setting MDCDIV as 26  */
	iowrite32(0x0000009a, ccb->mii + BCMA_CCB_MII_MNG_CTL);
	udelay(2);

	/* USB3 PLL Block */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e8000);

	/* Assert Ana_Pllseq start */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x58061000);

	/* Assert CML Divider ratio to 26 */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x582a6400);

	/* Asserting PLL Reset */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x582ec000);

	/* Deaaserting PLL Reset */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x582e8000);

	/* Waiting MII Mgt interface idle */
	bcm_ns_usb3_mii_mng_wait_idle(usb3);

	/* Deasserting USB3 system reset */
	bcma_awrite32(usb3->core, BCMA_RESET_CTL, 0);

	/* PLL frequency monitor enable */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x58069000);

	/* PIPE Block */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e8060);

	/* CMPMAX & CMPMINTH setting */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x580af30d);

	/* DEGLITCH MIN & MAX setting */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x580e6302);

	/* TXPMD block */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e8040);

	/* Enabling SSC */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x58061003);

	/* Waiting MII Mgt interface idle */
	bcm_ns_usb3_mii_mng_wait_idle(usb3);
}

static void bcm_ns_usb3_phy_init_ns_ax(struct bcm_ns_usb3 *usb3)
{
	struct bcma_drv_cc_b *ccb = &usb3->bus->drv_cc_b;

	/* Enable MDIO. Setting MDCDIV as 26  */
	iowrite32(0x0000009a, ccb->mii + BCMA_CCB_MII_MNG_CTL);
	udelay(2);

	/* PLL30 block */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e8000);

	bcm_ns_usb3_mii_mng_write32(usb3, 0x582a6400);

	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e80e0);

	bcm_ns_usb3_mii_mng_write32(usb3, 0x580a009c);

	/* Enable SSC */
	bcm_ns_usb3_mii_mng_write32(usb3, 0x587e8040);

	bcm_ns_usb3_mii_mng_write32(usb3, 0x580a21d3);

	bcm_ns_usb3_mii_mng_write32(usb3, 0x58061003);

	/* Waiting MII Mgt interface idle */
	bcm_ns_usb3_mii_mng_wait_idle(usb3);

	/* Deasserting USB3 system reset */
	bcma_awrite32(usb3->core, BCMA_RESET_CTL, 0);
}

static int bcm_ns_usb3_phy_init(struct usb_phy *phy)
{
	struct bcm_ns_usb3 *usb3 = phy_to_usb3(phy);
	struct bcma_chipinfo *chipinfo = &usb3->bus->chipinfo;

	/* Perform USB3 system soft reset */
	bcma_awrite32(usb3->core, BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);

	if (chipinfo->id == BCMA_CHIP_ID_BCM53018 ||
	    (chipinfo->id == BCMA_CHIP_ID_BCM4707 && (chipinfo->rev == 4 || chipinfo->rev == 6)) ||
	    chipinfo->id == BCMA_CHIP_ID_BCM47094) {
		bcm_ns_usb3_phy_init_ns_bx(usb3);
	} else if (chipinfo->id == BCMA_CHIP_ID_BCM4707) {
		bcm_ns_usb3_phy_init_ns_ax(usb3);
	} else {
		WARN_ON(1);
		return -ENOTSUPP;
	}

	return 0;
}

static struct bcma_bus *bcm_ns_usb3_get_bus(struct platform_device *pdev)
{
	struct device_node *node;
	struct platform_device *bus_pdev;

	node = of_parse_phandle(pdev->dev.of_node, "bus", 0);
	if (!node)
		return NULL;

	bus_pdev = of_find_device_by_node(node);
	if (!bus_pdev)
		return NULL;

	return platform_get_drvdata(bus_pdev);
}

static int bcm_ns_usb3_probe(struct platform_device *pdev)
{
	struct bcm_ns_usb3 *usb3;
	struct bcma_bus *bus;
	int err;

	bus = bcm_ns_usb3_get_bus(pdev);
	if (!bus)
		return -EPROBE_DEFER;

	usb3 = devm_kzalloc(&pdev->dev, sizeof(*usb3), GFP_KERNEL);
	if (!usb3)
		return -ENOMEM;

	usb3->dev		= &pdev->dev;
	usb3->bus		= bus;
	usb3->phy.dev		= usb3->dev;
	usb3->phy.label		= "bcm_ns_usb3";
	usb3->phy.init		= bcm_ns_usb3_phy_init;

	usb3->core = bcma_find_core(usb3->bus, BCMA_CORE_NS_USB30);
	if (!usb3->core)
		return -ENODEV;

	err = usb_add_phy(&usb3->phy, USB_PHY_TYPE_USB3);
	if (err) {
		dev_err(usb3->dev, "Failed to add PHY: %d\n", err);
		return err;
	}

	platform_set_drvdata(pdev, usb3);

	dev_info(usb3->dev, "Registered driver for Broadcom Northstar USB PHY for bcma chip with id %d\n",
		 usb3->bus->chipinfo.id);

	return 0;
}

static int bcm_ns_usb3_remove(struct platform_device *pdev)
{
	struct bcm_ns_usb3 *usb3 = platform_get_drvdata(pdev);

	usb_remove_phy(&usb3->phy);

	return 0;
}

static const struct of_device_id bcm_ns_usb3_id_table[] = {
	{ .compatible = "brcm,ns-usb3-phy", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm_ns_usb3_id_table);

static struct platform_driver bcm_ns_usb3_driver = {
	.probe		= bcm_ns_usb3_probe,
	.remove		= bcm_ns_usb3_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "bcm_ns_usb3",
		.of_match_table = bcm_ns_usb3_id_table,
	},
};
module_platform_driver(bcm_ns_usb3_driver);

MODULE_LICENSE("GPL");
