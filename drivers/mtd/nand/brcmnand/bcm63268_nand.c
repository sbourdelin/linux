/*
 * Copyright 2015 Simon Arlott
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Derived from bcm63138_nand.c:
 * Copyright © 2015 Broadcom Corporation
 *
 * Derived from bcm963xx_4.12L.06B_consumer/shared/opensource/include/bcm963xx/63268_map_part.h:
 * Copyright 2000-2010 Broadcom Corporation
 *
 * Derived from bcm963xx_4.12L.06B_consumer/shared/opensource/flash/nandflash.c:
 * Copyright 2000-2010 Broadcom Corporation
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "brcmnand.h"

struct bcm63268_nand_soc {
	struct brcmnand_soc soc;
	void __iomem *base;
	struct clk *clk;
};

#define BCM63268_NAND_INT		0x00
#define  BCM63268_NAND_STATUS_SHIFT	0
#define  BCM63268_NAND_STATUS_MASK	(0xfff << BCM63268_NAND_STATUS_SHIFT)
#define  BCM63268_NAND_ENABLE_SHIFT	16
#define  BCM63268_NAND_ENABLE_MASK	(0xffff << BCM63268_NAND_ENABLE_SHIFT)
#define BCM63268_NAND_BASE_ADDR0	0x04
#define BCM63268_NAND_BASE_ADDR1	0x0c

enum {
	BCM63268_NP_READ	= BIT(0),
	BCM63268_BLOCK_ERASE	= BIT(1),
	BCM63268_COPY_BACK	= BIT(2),
	BCM63268_PAGE_PGM	= BIT(3),
	BCM63268_CTRL_READY	= BIT(4),
	BCM63268_DEV_RBPIN	= BIT(5),
	BCM63268_ECC_ERR_UNC	= BIT(6),
	BCM63268_ECC_ERR_CORR	= BIT(7),
};

static bool bcm63268_nand_intc_ack(struct brcmnand_soc *soc)
{
	struct bcm63268_nand_soc *priv =
			container_of(soc, struct bcm63268_nand_soc, soc);
	void __iomem *mmio = priv->base + BCM63268_NAND_INT;
	u32 val = brcmnand_readl(mmio);

	if (val & (BCM63268_CTRL_READY << BCM63268_NAND_STATUS_SHIFT)) {
		/* Ack interrupt */
		val &= ~BCM63268_NAND_STATUS_MASK;
		val |= BCM63268_CTRL_READY << BCM63268_NAND_STATUS_SHIFT;
		brcmnand_writel(val, mmio);
		return true;
	}

	return false;
}

static void bcm63268_nand_intc_set(struct brcmnand_soc *soc, bool en)
{
	struct bcm63268_nand_soc *priv =
			container_of(soc, struct bcm63268_nand_soc, soc);
	void __iomem *mmio = priv->base + BCM63268_NAND_INT;
	u32 val = brcmnand_readl(mmio);

	/* Don't ack any interrupts */
	val &= ~BCM63268_NAND_STATUS_MASK;

	if (en)
		val |= BCM63268_CTRL_READY << BCM63268_NAND_ENABLE_SHIFT;
	else
		val &= ~(BCM63268_CTRL_READY << BCM63268_NAND_ENABLE_SHIFT);

	brcmnand_writel(val, mmio);
}

static int bcm63268_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcm63268_nand_soc *priv;
	struct brcmnand_soc *soc;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	soc = &priv->soc;

	res = platform_get_resource_byname(pdev,
		IORESOURCE_MEM, "nand-intr-base");
	if (!res)
		return -EINVAL;

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = of_clk_get(dev->of_node, 0);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		clk_put(priv->clk);
		return ret;
	}

	soc->ctlrdy_ack = bcm63268_nand_intc_ack;
	soc->ctlrdy_set_enabled = bcm63268_nand_intc_set;

	/* Disable and ack all interrupts  */
	brcmnand_writel(0, priv->base + BCM63268_NAND_INT);
	brcmnand_writel(BCM63268_NAND_STATUS_MASK,
			priv->base + BCM63268_NAND_INT);

	ret = brcmnand_probe(pdev, soc);
	if (ret) {
		clk_disable_unprepare(priv->clk);
		clk_put(priv->clk);
	}

	return ret;
}

static int bcm63268_nand_remove(struct platform_device *pdev)
{
	struct brcmnand_controller *ctrl = dev_get_drvdata(&pdev->dev);
	struct bcm63268_nand_soc *priv =
			container_of(ctrl->soc, struct bcm63268_nand_soc, soc);

	clk_disable_unprepare(priv->clk);
	clk_put(priv->clk);

	return brcmnand_remove(pdev);
}

static const struct of_device_id bcm63268_nand_of_match[] = {
	{ .compatible = "brcm,nand-bcm63268" },
	{},
};
MODULE_DEVICE_TABLE(of, bcm63268_nand_of_match);

static struct platform_driver bcm63268_nand_driver = {
	.probe			= bcm63268_nand_probe,
	.remove			= bcm63268_nand_remove,
	.driver = {
		.name		= "bcm63268_nand",
		.pm		= &brcmnand_pm_ops,
		.of_match_table	= bcm63268_nand_of_match,
	}
};
module_platform_driver(bcm63268_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Arlott");
MODULE_DESCRIPTION("NAND driver for BCM63268");
