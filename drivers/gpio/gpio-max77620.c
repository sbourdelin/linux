/*
 * MAXIM MAX77620 GPIO driver
 *
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/mfd/max77620.h>

#define GPIO_REG_ADDR(offset) (MAX77620_REG_GPIO0 + offset)

struct max77620_gpio {
	struct gpio_chip	gpio_chip;
	struct device		*parent;
	struct device		*dev;
	int			gpio_irq;
	int			irq_base;
	int			gpio_base;
};

static const struct regmap_irq max77620_gpio_irqs[] = {
	[MAX77620_IRQ_GPIO0 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE0,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO1 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE1,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO2 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE2,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO3 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE3,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO4 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE4,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO5 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE5,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO6 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE6,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_GPIO7 - MAX77620_IRQ_GPIO0] = {
		.mask = MAX77620_IRQ_LVL2_GPIO_EDGE7,
		.reg_offset = 0,
	},
};

static struct regmap_irq_chip max77620_gpio_irq_chip = {
	.name = "max77620-gpio",
	.irqs = max77620_gpio_irqs,
	.num_irqs = ARRAY_SIZE(max77620_gpio_irqs),
	.num_regs = 1,
	.irq_reg_stride = 1,
	.status_base = MAX77620_REG_IRQ_LVL2_GPIO,
};

static int max77620_gpio_dir_input(struct gpio_chip *gc, unsigned offset)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	int ret;

	ret = max77620_reg_update(mgpio->parent, MAX77620_PWR_SLAVE,
		GPIO_REG_ADDR(offset), MAX77620_CNFG_GPIO_DIR_MASK,
				MAX77620_CNFG_GPIO_DIR_INPUT);
	if (ret < 0)
		dev_err(mgpio->dev, "CNFG_GPIOx dir update failed: %d\n", ret);

	return ret;
}

static int max77620_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	u8 val;
	int ret;

	ret = max77620_reg_read(mgpio->parent, MAX77620_PWR_SLAVE,
				GPIO_REG_ADDR(offset), &val);
	if (ret < 0) {
		dev_err(mgpio->dev, "CNFG_GPIOx read failed: %d\n", ret);
		return ret;
	}

	return !!(val & MAX77620_CNFG_GPIO_INPUT_VAL_MASK);
}

static int max77620_gpio_dir_output(struct gpio_chip *gc, unsigned offset,
				int value)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	u8 val;
	int ret;

	val = (value) ? MAX77620_CNFG_GPIO_OUTPUT_VAL_HIGH :
				MAX77620_CNFG_GPIO_OUTPUT_VAL_LOW;

	ret = max77620_reg_update(mgpio->parent, MAX77620_PWR_SLAVE,
			GPIO_REG_ADDR(offset),
			MAX77620_CNFG_GPIO_OUTPUT_VAL_MASK, val);
	if (ret < 0) {
		dev_err(mgpio->dev, "CNFG_GPIOx val update failed: %d\n", ret);
		return ret;
	}

	ret = max77620_reg_update(mgpio->parent, MAX77620_PWR_SLAVE,
		GPIO_REG_ADDR(offset), MAX77620_CNFG_GPIO_DIR_MASK,
				MAX77620_CNFG_GPIO_DIR_OUTPUT);
	if (ret < 0)
		dev_err(mgpio->dev, "CNFG_GPIOx dir update failed: %d\n", ret);

	return ret;
}

static int max77620_gpio_set_debounce(struct gpio_chip *gc,
		unsigned offset, unsigned debounce)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	u8 val;
	int ret;

	switch (debounce) {
	case 0:
		val = MAX77620_CNFG_GPIO_DBNC_None;
		break;
	case 1 ... 8:
		val = MAX77620_CNFG_GPIO_DBNC_8ms;
		break;
	case 9 ... 16:
		val = MAX77620_CNFG_GPIO_DBNC_16ms;
		break;
	case 17 ... 32:
		val = MAX77620_CNFG_GPIO_DBNC_32ms;
		break;
	default:
		dev_err(mgpio->dev, "Illegal value %u\n", debounce);
		return -EINVAL;
	}

	ret = max77620_reg_update(mgpio->parent, MAX77620_PWR_SLAVE,
		GPIO_REG_ADDR(offset), MAX77620_CNFG_GPIO_DBNC_MASK, val);
	if (ret < 0)
		dev_err(mgpio->dev, "CNFG_GPIOx_DBNC update failed: %d\n", ret);

	return ret;
}

static void max77620_gpio_set(struct gpio_chip *gc, unsigned offset,
			int value)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	u8 val;
	int ret;

	val = (value) ? MAX77620_CNFG_GPIO_OUTPUT_VAL_HIGH :
				MAX77620_CNFG_GPIO_OUTPUT_VAL_LOW;

	ret = max77620_reg_update(mgpio->parent, MAX77620_PWR_SLAVE,
			GPIO_REG_ADDR(offset),
			MAX77620_CNFG_GPIO_OUTPUT_VAL_MASK, val);
	if (ret < 0)
		dev_err(mgpio->dev, "CNFG_GPIO_OUT update failed: %d\n", ret);
}

static int max77620_gpio_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct max77620_gpio *mgpio = gpiochip_get_data(gc);
	struct max77620_chip *chip = dev_get_drvdata(mgpio->dev->parent);

	return regmap_irq_get_virq(chip->gpio_irq_data, offset);
}

static void max77620_gpio_irq_remove(struct max77620_gpio *mgpio)
{
	struct max77620_chip *chip = dev_get_drvdata(mgpio->dev->parent);

	regmap_del_irq_chip(mgpio->gpio_irq, chip->gpio_irq_data);
	chip->gpio_irq_data = NULL;
}

static int max77620_gpio_probe(struct platform_device *pdev)
{
	struct max77620_gpio *mgpio;
	struct max77620_chip *chip =  dev_get_drvdata(pdev->dev.parent);
	int ret;
	int gpio_irq;

	gpio_irq = platform_get_irq(pdev, 0);
	if (gpio_irq <= 0) {
		dev_err(&pdev->dev, "Gpio irq not available %d\n", gpio_irq);
		return -ENODEV;
	}

	mgpio = devm_kzalloc(&pdev->dev, sizeof(*mgpio), GFP_KERNEL);
	if (!mgpio)
		return -ENOMEM;

	mgpio->parent = pdev->dev.parent;
	mgpio->dev = &pdev->dev;
	mgpio->gpio_irq = gpio_irq;

	mgpio->gpio_chip.label = pdev->name;
	mgpio->gpio_chip.parent = &pdev->dev;
	mgpio->gpio_chip.direction_input = max77620_gpio_dir_input;
	mgpio->gpio_chip.get = max77620_gpio_get;
	mgpio->gpio_chip.direction_output = max77620_gpio_dir_output;
	mgpio->gpio_chip.set_debounce = max77620_gpio_set_debounce;
	mgpio->gpio_chip.set = max77620_gpio_set;
	mgpio->gpio_chip.to_irq = max77620_gpio_to_irq;
	mgpio->gpio_chip.ngpio = MAX77620_GPIO_NR;
	mgpio->gpio_chip.can_sleep = 1;
	mgpio->gpio_chip.base = -1;
	mgpio->irq_base = -1;
#ifdef CONFIG_OF_GPIO
	mgpio->gpio_chip.of_node = pdev->dev.parent->of_node;
#endif

	platform_set_drvdata(pdev, mgpio);

	ret = gpiochip_add_data(&mgpio->gpio_chip, mgpio);
	if (ret < 0) {
		dev_err(&pdev->dev, "gpio_init: Failed to add max77620_gpio\n");
		return ret;
	}
	mgpio->gpio_base = mgpio->gpio_chip.base;

	ret = regmap_add_irq_chip(chip->rmap[MAX77620_PWR_SLAVE],
		mgpio->gpio_irq, IRQF_ONESHOT | IRQF_EARLY_RESUME,
		mgpio->irq_base,
		&max77620_gpio_irq_chip, &chip->gpio_irq_data);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to add gpio irq_chip %d\n", ret);
		goto fail;
	}

	return 0;

fail:
	gpiochip_remove(&mgpio->gpio_chip);

	return ret;
}

static int max77620_gpio_remove(struct platform_device *pdev)
{
	struct max77620_gpio *mgpio = platform_get_drvdata(pdev);

	max77620_gpio_irq_remove(mgpio);
	gpiochip_remove(&mgpio->gpio_chip);

	return 0;
}

static const struct platform_device_id max77620_gpio_devtype[] = {
	{
		.name = "max77620-gpio",
	},
	{
		.name = "max20024-gpio",
	},
};

static struct platform_driver max77620_gpio_driver = {
	.driver.name	= "max77620-gpio",
	.driver.owner	= THIS_MODULE,
	.probe		= max77620_gpio_probe,
	.remove		= max77620_gpio_remove,
	.id_table	= max77620_gpio_devtype,
};

static int __init max77620_gpio_init(void)
{
	return platform_driver_register(&max77620_gpio_driver);
}
subsys_initcall(max77620_gpio_init);

static void __exit max77620_gpio_exit(void)
{
	platform_driver_unregister(&max77620_gpio_driver);
}
module_exit(max77620_gpio_exit);

MODULE_DESCRIPTION("GPIO interface for MAX77620 and MAX20024 PMIC");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_AUTHOR("Chaitanya Bandi <bandik@nvidia.com>");
MODULE_ALIAS("platform:max77620-gpio");
MODULE_LICENSE("GPL v2");
