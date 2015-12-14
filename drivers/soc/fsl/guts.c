/*
 * Freescale QorIQ Platforms GUTS Driver
 *
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 *
 * Author: Yangbo Lu <yangbo.lu@freescale.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/fsl/guts.h>

/*
 * Table for matching compatible strings, for device tree
 * guts node, for Freescale QorIQ SOCs.
 * "fsl,qoriq-device-config-2.0" corresponds to T4 & B4
 * SOCs. For the older SOCs "fsl,qoriq-device-config-1.0"
 * string would be used.
 */
static const struct of_device_id guts_device_ids[] = {
	{ .compatible = "fsl,qoriq-device-config-1.0", },
	{ .compatible = "fsl,qoriq-device-config-2.0", },
	{}
};

struct ccsr_guts __iomem *guts_regmap(void)
{
	struct device_node *guts_node;
	struct ccsr_guts __iomem *guts;

	guts_node = of_find_matching_node(NULL, guts_device_ids);
	if (!guts_node)
		return NULL;

	guts = of_iomap(guts_node, 0);
	if (!guts)
		return NULL;

	of_node_put(guts_node);
	return guts;
}
EXPORT_SYMBOL_GPL(guts_regmap);

u8 guts_get_reg8(void __iomem *reg)
{
	u8 val;

	val = ioread8(reg);
	return val;
}
EXPORT_SYMBOL_GPL(guts_get_reg8);

void guts_set_reg8(void __iomem *reg, u8 value)
{
	iowrite8(value, reg);
}
EXPORT_SYMBOL_GPL(guts_set_reg8);

u32 guts_get_reg32(void __iomem *reg)
{
	struct device_node *guts_node;
	u32 val;

	guts_node = of_find_matching_node(NULL, guts_device_ids);
	if (!guts_node)
		return 0;

	if (of_property_read_bool(guts_node, "little-endian"))
		val = ioread32(reg);
	else
		val = ioread32be(reg);

	return val;
}
EXPORT_SYMBOL_GPL(guts_get_reg32);

void guts_set_reg32(void __iomem *reg, u32 value)
{
	struct device_node *guts_node;

	guts_node = of_find_matching_node(NULL, guts_device_ids);
	if (!guts_node)
		return;

	if (of_property_read_bool(guts_node, "little-endian"))
		iowrite32(value, reg);
	else
		iowrite32be(value, reg);
}
EXPORT_SYMBOL_GPL(guts_set_reg32);

static int __init guts_drv_init(void)
{
	pr_info("guts: Freescale QorIQ Platforms GUTS Driver\n");
	return 0;
}
module_init(guts_drv_init);

static void __exit guts_drv_exit(void)
{
}
module_exit(guts_drv_exit);

MODULE_AUTHOR("Yangbo Lu <yangbo.lu@freescale.com>");
MODULE_DESCRIPTION("Freescale QorIQ Platforms GUTS Driver");
MODULE_LICENSE("GPL v2");
