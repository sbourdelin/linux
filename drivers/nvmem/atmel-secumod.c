/*
 * Driver for SAMA5D2 secure module (SECUMOD).
 *
 * Copyright (C) 2016 eGauge Systems LLC
 *
 * David Mosberger <davidm@egauge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static int
secumod_reg_read(void *context, unsigned int reg, void *_val, size_t bytes)
{
	void __iomem *base = context;
	u32 *val = _val;
	int i = 0, words = bytes / 4;

	while (words--)
		*val++ = readl(base + reg + (i++ * 4));

	return 0;
}

static int
secumod_reg_write(void *context, unsigned int reg, void *_val, size_t bytes)
{
	void __iomem *base = context;
	u32 *val = _val;
	int i = 0, words = bytes / 4;

	while (words--)
		writel(*val++, base + reg + (i++ * 4));

	return 0;
}

static struct nvmem_config econfig = {
	.name = "secumod",
	.owner = THIS_MODULE,
	.stride = 4,
	.word_size = 1,
	.reg_read = secumod_reg_read,
	.reg_write = secumod_reg_write,
};

/*
 * Security-module register definitions:
 */
#define SECUMOD_RAMRDY	0x0014

/*
 * Since the secure module may need to automatically erase some of the
 * RAM, it may take a while for it to be ready.  As far as I know,
 * it's not documented how long this might take in the worst-case.
 */
static void
secumod_wait_ready (void *regs)
{
	unsigned long start, stop;

	start = jiffies;
	while (!(readl(regs + SECUMOD_RAMRDY) & 1))
		msleep_interruptible(1);
	stop = jiffies;
	if (stop != start)
		pr_info("nvmem-atmel-secumod: it took %u msec for SECUMOD "
			"to become ready...\n", jiffies_to_msecs(stop - start));
	else
		pr_info("nvmem-atmel-secumod: ready\n");
}

static int secumod_remove(struct platform_device *pdev)
{
	struct nvmem_device *nvmem = platform_get_drvdata(pdev);

	return nvmem_unregister(nvmem);
}

static int secumod_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct nvmem_device *nvmem;
	void __iomem *base;

	/*
	 * Map controller address temporarily so we can ensure that
	 * the hardware is ready:
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	secumod_wait_ready(base);
	devm_iounmap(dev, base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);

	if (IS_ERR(base))
		return PTR_ERR(base);

	econfig.size = resource_size(res);
	econfig.dev = dev;
	econfig.priv = base;

	nvmem = nvmem_register(&econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static const struct of_device_id secumod_of_match[] = {
	{ .compatible = "atmel,sama5d2-secumod",},
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, secumod_of_match);

static struct platform_driver secumod_driver = {
	.probe = secumod_probe,
	.remove = secumod_remove,
	.driver = {
		.name = "atmel,sama5d2-secumod",
		.of_match_table = secumod_of_match,
	},
};
module_platform_driver(secumod_driver);
MODULE_AUTHOR("David Mosberger <davidm@egauge.net>");
MODULE_DESCRIPTION("Atmel Secumod driver");
MODULE_LICENSE("GPL v2");
