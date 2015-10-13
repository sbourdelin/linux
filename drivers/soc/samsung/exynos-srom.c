/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *	      http://www.samsung.com/
 *
 * EXYNOS - SROM Controller support
 * Author: Pankaj Dubey <pankaj.dubey@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "exynos-srom.h"

static void __iomem *exynos_srom_base;

static const unsigned long exynos_srom_offsets[] = {
	/* SROM side */
	EXYNOS_SROM_BW,
	EXYNOS_SROM_BC0,
	EXYNOS_SROM_BC1,
	EXYNOS_SROM_BC2,
	EXYNOS_SROM_BC3,
};

/**
 * struct exynos_srom_reg_dump: register dump of SROM Controller registers.
 * @offset: srom register offset from the controller base address.
 * @value: the value of register under the offset.
 */
struct exynos_srom_reg_dump {
	u32     offset;
	u32     value;
};

static struct exynos_srom_reg_dump *exynos_srom_regs;

static struct exynos_srom_reg_dump *exynos_srom_alloc_reg_dump(
		const unsigned long *rdump,
		unsigned long nr_rdump)
{
	struct exynos_srom_reg_dump *rd;
	unsigned int i;

	rd = kcalloc(nr_rdump, sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	for (i = 0; i < nr_rdump; ++i)
		rd[i].offset = rdump[i];

	return rd;
}

static int exynos_srom_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct device *dev = &pdev->dev;

	np = dev->of_node;
	exynos_srom_base = of_iomap(np, 0);

	if (!exynos_srom_base) {
		pr_err("iomap of exynos srom controller failed\n");
		return -ENOMEM;
	}

	exynos_srom_regs = exynos_srom_alloc_reg_dump(exynos_srom_offsets,
			sizeof(exynos_srom_offsets));

	if (!exynos_srom_regs) {
		iounmap(exynos_srom_regs);
		return -ENOMEM;
	}

	return 0;
}

static int exynos_srom_remove(struct platform_device *pdev)
{
	iounmap(exynos_srom_base);
	exynos_srom_base = NULL;

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static void exynos_srom_save(void __iomem *base,
				    struct exynos_srom_reg_dump *rd,
				    unsigned int num_regs)
{
	for (; num_regs > 0; --num_regs, ++rd)
		rd->value = readl(base + rd->offset);

}

static void exynos_srom_restore(void __iomem *base,
				      const struct exynos_srom_reg_dump *rd,
				      unsigned int num_regs)
{
	for (; num_regs > 0; --num_regs, ++rd)
		writel(rd->value, base + rd->offset);

}

static int exynos_srom_suspend(struct device *dev)
{
	exynos_srom_save(exynos_srom_base, exynos_srom_regs,
				ARRAY_SIZE(exynos_srom_offsets));

	return 0;
}

static int exynos_srom_resume(struct device *dev)
{
	exynos_srom_restore(exynos_srom_base, exynos_srom_regs,
				ARRAY_SIZE(exynos_srom_offsets));

	return 0;
}
#endif

static const struct of_device_id of_exynos_srom_ids[] = {
	{
		.compatible	= "samsung,exynos-srom",
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_exynos_srom_ids);

static SIMPLE_DEV_PM_OPS(exynos_srom_pm_ops, exynos_srom_suspend, exynos_srom_resume);

static struct platform_driver exynos_srom_driver = {
	.probe = exynos_srom_probe,
	.remove = exynos_srom_remove,
	.driver = {
		.name = "exynos-srom",
		.of_match_table = of_exynos_srom_ids,
		.pm = &exynos_srom_pm_ops,
	},
};
module_platform_driver(exynos_srom_driver);

MODULE_AUTHOR("Pankaj Dubey <pankaj.dubey@samsung.com>");
MODULE_DESCRIPTION("Exynos SROM Controller Driver");
MODULE_LICENSE("GPL");
