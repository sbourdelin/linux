/*
 *  Atheros AR71XX/AR724X/AR913X GPIO API support
 *
 *  Copyright (C) 2010-2011 Jaiganesh Narayanan <jnarayanan@atheros.com>
 *  Copyright (C) 2008-2011 Gabor Juhos <juhosg@openwrt.org>
 *  Copyright (C) 2008 Imre Kaloz <kaloz@openwrt.org>
 *
 *  Parts of this file are based on Atheros' 2.6.15/2.6.31 BSP
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/gpio/driver.h>
#include <linux/platform_data/gpio-ath79.h>
#include <linux/of_device.h>
#include <linux/basic_mmio_gpio.h>

#include <asm/mach-ath79/ar71xx_regs.h>

struct ath79_gpio {
	struct bgpio_chip bgc;
};

static const struct of_device_id ath79_gpio_of_match[] = {
	{ .compatible = "qca,ar7100-gpio" },
	{ .compatible = "qca,ar9340-gpio" },
	{},
};

static int ath79_gpio_probe(struct platform_device *pdev)
{
	struct ath79_gpio_platform_data *pdata = pdev->dev.platform_data;
	struct device_node *np = pdev->dev.of_node;
	void __iomem *ath79_gpio_base;
	struct ath79_gpio *ctrl;
	struct resource *res;
	u32 ath79_gpio_count;
	bool oe_inverted;
	int err;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	if (np) {
		err = of_property_read_u32(np, "ngpios", &ath79_gpio_count);
		if (err) {
			dev_err(&pdev->dev, "ngpios property is not valid\n");
			return err;
		}
		if (ath79_gpio_count >= 32) {
			dev_err(&pdev->dev, "ngpios must be less than 32\n");
			return -EINVAL;
		}
		oe_inverted = of_device_is_compatible(np, "qca,ar9340-gpio");
	} else if (pdata) {
		ath79_gpio_count = pdata->ngpios;
		oe_inverted = pdata->oe_inverted;
	} else {
		dev_err(&pdev->dev, "No DT node or platform data found\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ath79_gpio_base = devm_ioremap_nocache(
		&pdev->dev, res->start, resource_size(res));
	if (!ath79_gpio_base)
		return -ENOMEM;

	err = bgpio_init(&ctrl->bgc, &pdev->dev, 4,
			ath79_gpio_base + AR71XX_GPIO_REG_IN,
			ath79_gpio_base + AR71XX_GPIO_REG_SET,
			ath79_gpio_base + AR71XX_GPIO_REG_CLEAR,
			oe_inverted ?
				NULL : ath79_gpio_base + AR71XX_GPIO_REG_OE,
			oe_inverted ?
				ath79_gpio_base + AR71XX_GPIO_REG_OE : NULL,
			0);
	if (err) {
		dev_err(&pdev->dev, "bgpio_init failed\n");
		return err;
	}

	ctrl->bgc.gc.label = "ath79";
	ctrl->bgc.gc.base = 0;
	ctrl->bgc.gc.ngpio = ath79_gpio_count;

	err = gpiochip_add(&ctrl->bgc.gc);
	if (err) {
		dev_err(&pdev->dev,
			"cannot add AR71xx GPIO chip, error=%d", err);
		return err;
	}

	return 0;
}

static struct platform_driver ath79_gpio_driver = {
	.driver = {
		.name = "ath79-gpio",
		.of_match_table	= ath79_gpio_of_match,
	},
	.probe = ath79_gpio_probe,
};

module_platform_driver(ath79_gpio_driver);
