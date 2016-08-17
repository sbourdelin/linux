/*
 * jz4780-rng.c - Random Number Generator driver for J4780
 *
 * Copyright 2016 (C) PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/hw_random.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/err.h>

#define REG_RNG_CTRL	0x0
#define REG_RNG_DATA	0x4

struct jz4780_rng {
	struct device *dev;
	struct hwrng rng;
	void __iomem *mem;
};

static u32 jz4780_rng_readl(struct jz4780_rng *rng, u32 offset)
{
	return readl(rng->mem + offset);
}

static void jz4780_rng_writel(struct jz4780_rng *rng, u32 val, u32 offset)
{
	writel(val, rng->mem + offset);
}

static int jz4780_rng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct jz4780_rng *jz4780_rng = container_of(rng, struct jz4780_rng,
							rng);
	u32 *data = buf;
	*data = jz4780_rng_readl(jz4780_rng, REG_RNG_DATA);
	return 4;
}

static int jz4780_rng_probe(struct platform_device *pdev)
{
	struct jz4780_rng *jz4780_rng;
	struct resource *res;
	resource_size_t size;
	int ret;

	jz4780_rng = devm_kzalloc(&pdev->dev, sizeof(struct jz4780_rng),
					GFP_KERNEL);
	if (!jz4780_rng)
		return -ENOMEM;

	jz4780_rng->dev = &pdev->dev;
	jz4780_rng->rng.name = "jz4780";
	jz4780_rng->rng.read = jz4780_rng_read;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);

	jz4780_rng->mem = devm_ioremap(&pdev->dev, res->start, size);
	if (IS_ERR(jz4780_rng->mem))
		return PTR_ERR(jz4780_rng->mem);

	platform_set_drvdata(pdev, jz4780_rng);
	jz4780_rng_writel(jz4780_rng, 1, REG_RNG_CTRL);
	ret = hwrng_register(&jz4780_rng->rng);

	return ret;
}

static int jz4780_rng_remove(struct platform_device *pdev)
{
	struct jz4780_rng *jz4780_rng = platform_get_drvdata(pdev);

	jz4780_rng_writel(jz4780_rng, 0, REG_RNG_CTRL);
	hwrng_unregister(&jz4780_rng->rng);

	return 0;
}

static const struct of_device_id jz4780_rng_dt_match[] = {
	{ .compatible = "ingenic,jz4780-rng", },
	{ },
};
MODULE_DEVICE_TABLE(of, jz4780_rng_dt_match);

static struct platform_driver jz4780_rng_driver = {
	.driver		= {
		.name	= "jz4780-rng",
		.of_match_table = jz4780_rng_dt_match,
	},
	.probe		= jz4780_rng_probe,
	.remove		= jz4780_rng_remove,
};
module_platform_driver(jz4780_rng_driver);

MODULE_DESCRIPTION("Ingenic JZ4780 H/W Random Number Generator driver");
MODULE_AUTHOR("PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>");
MODULE_LICENSE("GPL");
