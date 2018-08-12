// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sam Ravnborg
 *
 * Author: Sam Ravnborg <sam@ravnborg.org>
 *
 * Based on atmel-hlcdc.c wich is:
 * Copyright (C) 2014 Free Electrons
 * Copyright (C) 2014 Atmel
 * Author: Boris BREZILLON <boris.brezillon@free-electrons.com>
 */

#include <linux/mfd/atmel-lcdc.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/io.h>

#define ATMEL_LCDC_REG_MAX		(0x1000 - 0x4)

struct lcdc_regmap {
	void __iomem *regs;
};

static const struct mfd_cell lcdc_cells[] = {
	{
		.name = "atmel-lcdc-pwm",
		.of_compatible = "atmel,lcdc-pwm",
	},
	{
		.name = "atmel-lcdc-dc",
		.of_compatible = "atmel,lcdc-display-controller",
	},
};

static int regmap_lcdc_reg_write(void *context, unsigned int reg,
				 unsigned int val)
{
	struct lcdc_regmap *regmap = context;

	writel(val, regmap->regs + reg);

	return 0;
}

static int regmap_lcdc_reg_read(void *context, unsigned int reg,
				unsigned int *val)
{
	struct lcdc_regmap *regmap = context;

	*val = readl(regmap->regs + reg);

	return 0;
}

static const struct regmap_config lcdc_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = ATMEL_LCDC_REG_MAX,
	.reg_write = regmap_lcdc_reg_write,
	.reg_read = regmap_lcdc_reg_read,
	.fast_io = true,
};

static int lcdc_probe(struct platform_device *pdev)
{
	struct atmel_mfd_lcdc *lcdc;
	struct lcdc_regmap *regmap;
	struct resource *res;
	struct device *dev;
	int ret;

	dev = &pdev->dev;

	regmap = devm_kzalloc(dev, sizeof(*regmap), GFP_KERNEL);
	if (!regmap)
		return -ENOMEM;

	lcdc = devm_kzalloc(dev, sizeof(*lcdc), GFP_KERNEL);
	if (!lcdc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regmap->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regmap->regs)) {
		dev_err(dev, "Failed to allocate IO mem (%ld)\n",
			PTR_ERR(regmap->regs));
		return PTR_ERR(regmap->regs);
	}

	lcdc->irq = platform_get_irq(pdev, 0);
	if (lcdc->irq < 0) {
		dev_err(dev, "Failed to get irq (%d)\n", lcdc->irq);
		return lcdc->irq;
	}

	lcdc->lcdc_clk = devm_clk_get(dev, "lcdc_clk");
	if (IS_ERR(lcdc->lcdc_clk)) {
		dev_err(dev, "failed to get lcdc clock (%ld)\n",
			PTR_ERR(lcdc->lcdc_clk));
		return PTR_ERR(lcdc->lcdc_clk);
	}

	lcdc->bus_clk = devm_clk_get(dev, "hclk");
	if (IS_ERR(lcdc->bus_clk)) {
		dev_err(dev, "failed to get bus clock (%ld)\n",
			PTR_ERR(lcdc->bus_clk));
		return PTR_ERR(lcdc->bus_clk);
	}

	lcdc->regmap = devm_regmap_init(dev, NULL, regmap,
					 &lcdc_regmap_config);
	if (IS_ERR(lcdc->regmap)) {
		dev_err(dev, "Failed to init regmap (%ld)\n",
			PTR_ERR(lcdc->regmap));
		return PTR_ERR(lcdc->regmap);
	}

	dev_set_drvdata(dev, lcdc);

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE,
				   lcdc_cells, ARRAY_SIZE(lcdc_cells),
				   NULL, 0, NULL);
	if (ret < 0)
		dev_err(dev, "Failed to add %d mfd devices (%d)\n",
			ARRAY_SIZE(lcdc_cells), ret);

	return ret;
}

static const struct of_device_id lcdc_match[] = {
	{ .compatible = "atmel,at91sam9261-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9263-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9g10-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9g45-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9g46-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9m10-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9m11-lcdc-mfd" },
	{ .compatible = "atmel,at91sam9rl-lcdc-mfd" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, lcdc_match);

static struct platform_driver lcdc_driver = {
	.probe = lcdc_probe,
	.driver = {
		.name = "atmel-lcdc",
		.of_match_table = lcdc_match,
	},
};
module_platform_driver(lcdc_driver);

MODULE_ALIAS("platform:atmel-lcdc");
MODULE_AUTHOR("Sam Ravnborg <sam@ravnborg.org>");
MODULE_DESCRIPTION("Atmel LCDC mfd driver");
MODULE_LICENSE("GPL v2");
