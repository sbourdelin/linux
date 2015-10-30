/*
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Author: Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether expressed or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 *
 * Based on the TPS65912 driver
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <linux/mfd/tps65086.h>

struct tps65086_gpio {
	struct gpio_chip gpio_chip;
	struct tps65086 *tps;
};

static inline struct tps65086_gpio *to_tps65086_gpio(struct gpio_chip *chip)
{
	return container_of(chip, struct tps65086_gpio, gpio_chip);
}

static int tps65086_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	struct tps65086_gpio *gpio = to_tps65086_gpio(gc);
	int ret, val;

	ret = regmap_read(gpio->tps->regmap, TPS65086_GPOCTRL, &val);
	if (ret < 0)
		return ret;

	return val & BIT(4 + offset);
}

static void tps65086_gpio_set(struct gpio_chip *gc, unsigned offset,
			      int value)
{
	struct tps65086_gpio *gpio = to_tps65086_gpio(gc);

	regmap_update_bits(gpio->tps->regmap, TPS65086_GPOCTRL,
			   BIT(4 + offset), value ? BIT(4 + offset) : 0);
}

static struct gpio_chip template_chip = {
	.label			= "tps65086-gpio",
	.owner			= THIS_MODULE,
	.get			= tps65086_gpio_get,
	.set			= tps65086_gpio_set,
	.can_sleep		= true,
	.ngpio			= 4,
	.base			= -1,
};

static int tps65086_gpio_probe(struct platform_device *pdev)
{
	struct tps65086_gpio *gpio;
	int ret;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->tps = dev_get_drvdata(pdev->dev.parent);
	gpio->gpio_chip = template_chip;
	ret = gpiochip_add(&gpio->gpio_chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not register gpiochip, %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, gpio);

	return 0;
}

static int tps65086_gpio_remove(struct platform_device *pdev)
{
	struct tps65086_gpio *gpio = platform_get_drvdata(pdev);

	gpiochip_remove(&gpio->gpio_chip);

	return 0;
}

static const struct platform_device_id tps65086_gpio_id_table[] = {
	{ "tps65912-regulator", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, tps65086_gpio_id_table);

static struct platform_driver tps65086_gpio_driver = {
	.driver = {
		.name = "tps65086-gpio",
	},
	.probe = tps65086_gpio_probe,
	.remove = tps65086_gpio_remove,
	.id_table = tps65086_gpio_id_table,
};
module_platform_driver(tps65086_gpio_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("TPS65086 GPIO driver");
MODULE_LICENSE("GPL v2");
