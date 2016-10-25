/*
 * gpio-pv88080.c - GPIO device driver for PV88080
 * Copyright (C) 2016  Powerventure Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>

#include <linux/mfd/pv88080.h>

#define DEFAULT_PIN_NUMBER		2

#define PV88080_PORT_DIRECTION_INPUT	0
#define PV88080_PORT_DIRECTION_OUTPUT	1

struct pv88080_gpio {
	struct pv88080 *chip;
	struct gpio_chip gpio_chip;
	unsigned int input_reg;
	unsigned int gpio_base_reg;
};

static int pv88080_gpio_get_direction(struct gpio_chip *gc,
				unsigned int offset)
{
	struct pv88080_gpio *priv = gpiochip_get_data(gc);
	struct pv88080 *chip = priv->chip;
	unsigned int reg;
	int ret;

	ret = regmap_read(chip->regmap, priv->gpio_base_reg + offset, &reg);
	if (ret)
		return ret;

	reg = reg & PV88080_GPIO_DIRECTION_MASK;

	return !reg;
}

static int pv88080_gpio_direction_input(struct gpio_chip *gc,
				unsigned int offset)
{
	struct pv88080_gpio *priv = gpiochip_get_data(gc);
	struct pv88080 *chip = priv->chip;
	int ret;

	/* Set the initial value */
	ret = regmap_update_bits(chip->regmap, priv->gpio_base_reg + offset,
					PV88080_GPIO_OUTPUT_MASK, 0);
	if (ret)
		return ret;

	return regmap_update_bits(chip->regmap, priv->gpio_base_reg + offset,
				PV88080_GPIO_DIRECTION_MASK, 0);
}

static int pv88080_gpio_direction_output(struct gpio_chip *gc,
				unsigned int offset, int value)
{
	struct pv88080_gpio *priv = gpiochip_get_data(gc);
	struct pv88080 *chip = priv->chip;
	int ret;

	ret = regmap_update_bits(chip->regmap, priv->gpio_base_reg + offset,
			PV88080_GPIO_DIRECTION_MASK,
			PV88080_GPIO_DIRECTION_MASK);

	return ret;
}

static int pv88080_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct pv88080_gpio *priv = gpiochip_get_data(gc);
	struct pv88080 *chip = priv->chip;
	unsigned int reg = 0, direction;
	int ret;

	ret = regmap_read(chip->regmap, priv->gpio_base_reg + offset, &reg);
	if (ret)
		return ret;

	direction = (reg & PV88080_GPIO_DIRECTION_MASK);
	if (direction == PV88080_PORT_DIRECTION_OUTPUT) {
		if (reg & PV88080_GPIO_OUTPUT_EN)
			return 1;
		ret = 0;
	} else {
		ret = regmap_read(chip->regmap, priv->input_reg, &reg);
		if (ret < 0)
			return ret;
		ret = (reg & (PV88080_GPIO_INPUT_MASK << offset)) >> offset;
	}

	return ret;
}

static void pv88080_gpio_set(struct gpio_chip *gc, unsigned int offset,
				int value)
{
	struct pv88080_gpio *priv = gpiochip_get_data(gc);
	struct pv88080 *chip = priv->chip;

	if (value)
		regmap_update_bits(chip->regmap, priv->gpio_base_reg + offset,
				PV88080_GPIO_OUTPUT_MASK,
				PV88080_GPIO_OUTPUT_EN);
	else
		regmap_update_bits(chip->regmap, priv->gpio_base_reg + offset,
				PV88080_GPIO_OUTPUT_MASK,
				PV88080_GPIO_OUTPUT_DIS);
}

static const struct gpio_chip template_gpio = {
	.label = "pv88080-gpio",
	.owner = THIS_MODULE,
	.get_direction = pv88080_gpio_get_direction,
	.direction_input = pv88080_gpio_direction_input,
	.direction_output = pv88080_gpio_direction_output,
	.get = pv88080_gpio_get,
	.set = pv88080_gpio_set,
	.base = -1,
	.ngpio = DEFAULT_PIN_NUMBER,
};

static int pv88080_gpio_probe(struct platform_device *pdev)
{
	struct pv88080 *chip = dev_get_drvdata(pdev->dev.parent);
	struct pv88080_pdata *pdata = dev_get_platdata(chip->dev);
	struct pv88080_gpio *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev,
			sizeof(struct pv88080_gpio), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->chip = chip;
	priv->gpio_chip = template_gpio;
	priv->gpio_chip.parent = chip->dev;
	if (pdata && pdata->gpio_base)
		priv->gpio_chip.base = pdata->gpio_base;

	switch (chip->type) {
	case TYPE_PV88080_AA:
		priv->input_reg = PV88080AA_REG_GPIO_INPUT;
		priv->gpio_base_reg = PV88080AA_REG_GPIO_GPIO0;
		break;
	case TYPE_PV88080_BA:
		priv->input_reg = PV88080BA_REG_GPIO_INPUT;
		priv->gpio_base_reg = PV88080BA_REG_GPIO_GPIO0;
		break;
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &priv->gpio_chip, priv);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register gpiochip\n");
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct platform_device_id pv88080_gpio_id_table[] = {
	{ "pv88080-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, pv88080_gpio_id_table);

static struct platform_driver pv88080_gpio_driver = {
	.driver = {
		.name = "pv88080-gpio",
	},
	.probe = pv88080_gpio_probe,
	.id_table = pv88080_gpio_id_table,
};
module_platform_driver(pv88080_gpio_driver);

MODULE_AUTHOR("Eric Jeong <eric.jeong.opensource@diasemi.com>");
MODULE_DESCRIPTION("GPIO device driver for Powerventure PV88080");
MODULE_LICENSE("GPL");

