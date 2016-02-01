/*
 * PIC32 RNG driver
 *
 * Joshua Henderson <joshua.henderson@microchip.com>
 * Copyright (C) 2016 Microchip Technology Inc.  All rights reserved.
 *
 * This program is free software; you can distribute it and/or modify it
 * under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/hw_random.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/clkdev.h>

#define RNGCON		0x04
#define  TRNGEN		BIT(8)
#define  PRNGEN		BIT(9)
#define  PRNGCONT	BIT(10)
#define  TRNGMOD	BIT(11)
#define  SEEDLOAD	BIT(12)
#define RNGPOLY1	0x08
#define RNGPOLY2	0x0C
#define RNGNUMGEN1	0x10
#define RNGNUMGEN2	0x14
#define RNGSEED1	0x18
#define RNGSEED2	0x1C
#define RNGRCNT		0x20
#define  RCNT_MASK	0x7F

struct pic32_rng {
	void __iomem	*base;
	struct hwrng	rng;
	struct clk	*clk;
};

static int pic32_rng_read(struct hwrng *rng, void *buf, size_t max,
			  bool wait)
{
	struct pic32_rng *prng = container_of(rng, struct pic32_rng, rng);
	u64 *data = buf;

	*data = ((u64)readl(prng->base + RNGNUMGEN2) << 32) +
		readl(prng->base + RNGNUMGEN1);
	return 4;
}

static int pic32_rng_probe(struct platform_device *pdev)
{
	struct pic32_rng *prng;
	struct resource *res;
	u32 v, t;
	int ret;

	prng = devm_kzalloc(&pdev->dev, sizeof(*prng), GFP_KERNEL);
	if (!prng)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	prng->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(prng->base))
		return PTR_ERR(prng->base);

	prng->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(prng->clk))
		return PTR_ERR(prng->clk);

	clk_prepare_enable(prng->clk);

	/* enable TRNG in enhanced mode */
	v = readl(prng->base + RNGCON);
	v &= ~(TRNGEN | PRNGEN | 0xff);
	v |= TRNGMOD;
	writel(v | TRNGEN, prng->base + RNGCON);

	/* wait for valid seed */
	usleep_range(100, 200);
	t = readl(prng->base + RNGRCNT) & RCNT_MASK;
	if (t < 0x2A)
		dev_warn(&pdev->dev, "seed not generated.\n");

	/* load initial seed */
	writel(v | SEEDLOAD, prng->base + RNGCON);

	/* load initial polynomial: 42bit poly */
	writel(0x00c00003, prng->base + RNGPOLY1);
	writel(0x00000000, prng->base + RNGPOLY2);

	/* start PRNG to generate 42bit random */
	v |= 0x2A | PRNGCONT | PRNGEN;
	writel(v, prng->base + RNGCON);

	prng->rng.name = pdev->name;
	prng->rng.read = pic32_rng_read;

	ret = hwrng_register(&prng->rng);
	if (ret)
		goto err_register;

	platform_set_drvdata(pdev, prng);

	return 0;

err_register:
	return ret;
}

static int pic32_rng_remove(struct platform_device *pdev)
{
	struct pic32_rng *rng = platform_get_drvdata(pdev);

	hwrng_unregister(&rng->rng);
	writel(0, rng->base + RNGCON);
	clk_disable_unprepare(rng->clk);
	return 0;
}

static const struct of_device_id pic32_rng_of_match[] = {
	{ .compatible	= "microchip,pic32mzda-rng", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pic32_rng_of_match);

static struct platform_driver pic32_rng_driver = {
	.probe		= pic32_rng_probe,
	.remove		= pic32_rng_remove,
	.driver		= {
		.name	= "pic32-rng",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(pic32_rng_of_match),
	},
};

module_platform_driver(pic32_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joshua Henderson <joshua.henderson@microchip.com>");
MODULE_DESCRIPTION("Microchip PIC32 RNG Driver");
