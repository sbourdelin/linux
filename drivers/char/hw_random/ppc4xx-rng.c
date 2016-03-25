/*
 * Generic PowerPC 44x RNG driver
 *
 * Copyright 2011 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/hw_random.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <asm/io.h>

#define PPC4XX_TRNG_CTRL 0x0008
#define PPC4XX_TRNG_CTRL_DALM 0x20
#define PPC4XX_TRNG_STAT 0x0004
#define PPC4XX_TRNG_STAT_B 0x1
#define PPC4XX_TRNG_DATA 0x0000

#define MODULE_NAME "ppc4xx_rng"

static int ppc4xx_rng_data_present(struct hwrng *rng, int wait)
{
	void __iomem *rng_regs = (void __iomem *) rng->priv;
	int busy, i, present = 0;

	for (i = 0; i < 20; i++) {
		busy = (in_le32(rng_regs + PPC4XX_TRNG_STAT) & PPC4XX_TRNG_STAT_B);
		if (!busy || !wait) {
			present = 1;
			break;
		}
		udelay(10);
	}
	return present;
}

static int ppc4xx_rng_data_read(struct hwrng *rng, u32 *data)
{
	void __iomem *rng_regs = (void __iomem *) rng->priv;
	*data = in_le32(rng_regs + PPC4XX_TRNG_DATA);
	return 4;
}

static struct hwrng ppc4xx_rng = {
	.name = MODULE_NAME,
	.data_present = ppc4xx_rng_data_present,
	.data_read = ppc4xx_rng_data_read,
};

static int ppc4xx_rng_probe(struct platform_device *dev)
{
	void __iomem *rng_regs;
	int err = 0;

	rng_regs = of_iomap(dev->dev.of_node, 0);
	if (!rng_regs)
		return -ENODEV;

	out_le32(rng_regs + PPC4XX_TRNG_CTRL, PPC4XX_TRNG_CTRL_DALM);
	ppc4xx_rng.priv = (unsigned long) rng_regs;

	err = hwrng_register(&ppc4xx_rng);

	return err;
}

static int ppc4xx_rng_remove(struct platform_device *dev)
{
	void __iomem *rng_regs = (void __iomem *) ppc4xx_rng.priv;

	hwrng_unregister(&ppc4xx_rng);
	iounmap(rng_regs);

	return 0;
}

static const struct of_device_id ppc4xx_rng_match[] = {
	{ .compatible = "ppc4xx-rng", },
	{ .compatible = "amcc,ppc460ex-rng", },
	{ .compatible = "amcc,ppc440epx-rng", },
	{},
};
MODULE_DEVICE_TABLE(of, ppc4xx_rng_match);

static struct platform_driver ppc4xx_rng_driver = {
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = ppc4xx_rng_match,
	},
	.probe = ppc4xx_rng_probe,
	.remove = ppc4xx_rng_remove,
};

module_platform_driver(ppc4xx_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Josh Boyer <jwboyer@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("HW RNG driver for PPC 4xx processors");
