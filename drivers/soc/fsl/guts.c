/*
 * Freescale QorIQ Platforms GUTS Driver
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/sys_soc.h>

#define GUTS_PVR	0x0a0
#define GUTS_SVR	0x0a4

struct guts {
	void __iomem *regs;
	bool little_endian;
	struct soc_device_attribute soc;
};

static u32 fsl_guts_get_svr(struct guts *guts)
{
	if (guts->little_endian)
		return ioread32(guts->regs + GUTS_SVR);
	else
		return ioread32be(guts->regs + GUTS_SVR);
}

static u32 fsl_guts_get_pvr(struct guts *guts)
{
	if (guts->little_endian)
		return ioread32(guts->regs + GUTS_PVR);
	else
		return ioread32be(guts->regs + GUTS_PVR);
}

/*
 * Table for matching compatible strings, for device tree
 * guts node, for Freescale QorIQ SOCs.
 */
static const struct of_device_id fsl_guts_of_match[] = {
	/* For T4 & B4 Series SOCs */
	{ .compatible = "fsl,qoriq-device-config-1.0", .data = "T4/B4 series" },
	/* For P Series SOCs */
	{ .compatible = "fsl,p1010-guts", .data = "P1010/P1014" },
	{ .compatible = "fsl,p1020-guts", .data = "P1020/P1011" },
	{ .compatible = "fsl,p1021-guts", .data = "P1021/P1012" },
	{ .compatible = "fsl,p1022-guts", .data = "P1022/P1013" },
	{ .compatible = "fsl,p1023-guts", .data = "P1013/P1017" },
	{ .compatible = "fsl,p2020-guts", .data = "P2010/P2020" },
	{ .compatible = "fsl,qoriq-device-config-2.0", .data = "P series" },
	/* For BSC Series SOCs */
	{ .compatible = "fsl,bsc9131-guts", .data = "BSC9131 Qonverge" },
	{ .compatible = "fsl,bsc9132-guts", .data = "BSC9132 Qonverge" },
	/* For MPC85xx Series SOCs */
	{ .compatible = "fsl,mpc8536-guts", .data = "PowerPC MPC8536" },
	{ .compatible = "fsl,mpc8544-guts", .data = "PowerPC MPC8544" },
	{ .compatible = "fsl,mpc8548-guts", .data = "PowerPC MPC8548" },
	{ .compatible = "fsl,mpc8568-guts", .data = "PowerPC MPC8568" },
	{ .compatible = "fsl,mpc8569-guts", .data = "PowerPC MPC8569" },
	{ .compatible = "fsl,mpc8572-guts", .data = "PowerPC MPC8572" },
	/* For Layerscape Series SOCs */
	{ .compatible = "fsl,ls1021a-dcfg", .data = "Layerscape LS1021A" },
	{ .compatible = "fsl,ls1043a-dcfg", .data = "Layerscape LS1043A" },
	{ .compatible = "fsl,ls2080a-dcfg", .data = "Layerscape LS2080A" },
	{}
};

static void fsl_guts_init(struct device *dev, struct guts *guts)
{
	const struct of_device_id *id;
	u32 svr = fsl_guts_get_svr(guts);

	guts->soc.family = "NXP QorIQ";
	id = of_match_node(fsl_guts_of_match, dev->of_node);
	guts->soc.soc_id = devm_kasprintf(dev, "%s (ver 0x%06x)" id->data,
					  svr >> 8;
	guts->soc.revision = devm_kasprintf(dev, GFP_KERNEL, "0x%02x",
					    svr & 0xff);
}

static int fsl_guts_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct guts *guts;
	int ret;

	guts = devm_kzalloc(dev, sizeof(*guts), GFP_KERNEL);
	if (!guts) {
		ret = -ENOMEM;
		goto out;

	}

	/*
	 * syscon devices default to little-endian, but on powerpc we have
	 * existing device trees with big-endian maps and an absent endianess
	 * "big-property"
	 */
	if (!IS_ENABLED(CONFIG_POWERPC) &&
	    !of_property_read_bool(dev->of_node, "big-endian"))
		guts->little_endian = true;

	guts->regs = devm_ioremap_resource(dev, 0);
	if (!guts->regs) {
		ret = -ENOMEM;
		kfree(guts);
		goto out;
	}

	fsl_guts_init(dev, guts);
	ret = 0;
out:
	return ret;
}

static struct platform_driver fsl_soc_guts = {
	.probe = fsl_guts_probe,
	.driver.of_match_table = fsl_guts_of_match,
};

module_platform_driver(fsl_soc_guts);
