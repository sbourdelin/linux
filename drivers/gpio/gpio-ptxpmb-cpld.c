/*
 * Copyright (C) 2012 Juniper networks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/io.h>
#include <linux/module.h>

#include <linux/mfd/ptxpmb_cpld.h>

/**
 * struct ptxpmb_cpld_gpio - GPIO private data structure.
 * @base:			PCI base address of Memory mapped I/O register.
 * @dev:			Pointer to device structure.
 * @gpio:			Data for GPIO infrastructure.
 */
struct ptxpmb_cpld_gpio {
	void __iomem *base;
	struct device *dev;
	struct gpio_chip gpio;
	struct mutex lock;
};

static u8 *ptxpmb_cpld_gpio_get_addr(struct pmb_boot_cpld *cpld,
				     unsigned int nr)
{
	if (nr < 8)			/* 0..7: reset			*/
		return &cpld->reset;
	else if (nr < 16)		/* 8..15: control		*/
		return &cpld->control;
	else if (nr < 24)		/* 16..23: gpio1		*/
		return &cpld->gpio_1;
	else if (nr < 32)		/* 24..31: gpio2		*/
		return &cpld->gpio_2;
	else if (nr < 40)		/* 32..39: gp_reset1		*/
		return &cpld->gp_reset1;
	return &cpld->thermal_status;	/* 40..41: thermal status	*/
}

static void ptxpmb_cpld_gpio_set(struct gpio_chip *gpio, unsigned int nr,
				 int val)
{
	u32 reg;
	u8 bit = 1 << (nr & 7);
	struct ptxpmb_cpld_gpio *chip =
	  container_of(gpio, struct ptxpmb_cpld_gpio, gpio);
	u8 *addr = ptxpmb_cpld_gpio_get_addr(chip->base, nr);

	mutex_lock(&chip->lock);
	reg = ioread8(addr);
	if (val)
		reg |= bit;
	else
		reg &= ~bit;

	iowrite8(reg, addr);
	mutex_unlock(&chip->lock);
}

static int ptxpmb_cpld_gpio_get(struct gpio_chip *gpio, unsigned int nr)
{
	struct ptxpmb_cpld_gpio *chip = container_of(gpio,
						     struct ptxpmb_cpld_gpio,
						     gpio);
	u8 *addr = ptxpmb_cpld_gpio_get_addr(chip->base, nr);
	u8 bit = 1 << (nr & 7);

	return !!(ioread8(addr) & bit);
}

static int ptxpmb_cpld_gpio_direction_output(struct gpio_chip *gpio,
					     unsigned int nr, int val)
{
	return 0;
}

static int ptxpmb_cpld_gpio_direction_input(struct gpio_chip *gpio,
					    unsigned int nr)
{
	return 0;
}

static void ptxpmb_cpld_gpio_setup(struct ptxpmb_cpld_gpio *chip)
{
	struct gpio_chip *gpio = &chip->gpio;

	gpio->label = dev_name(chip->dev);
	gpio->owner = THIS_MODULE;
	gpio->direction_input = ptxpmb_cpld_gpio_direction_input;
	gpio->get = ptxpmb_cpld_gpio_get;
	gpio->direction_output = ptxpmb_cpld_gpio_direction_output;
	gpio->set = ptxpmb_cpld_gpio_set;
	gpio->dbg_show = NULL;
	gpio->base = -1;
	gpio->ngpio = 48;
	gpio->can_sleep = 0;
#ifdef CONFIG_OF_GPIO
	gpio->of_node = chip->dev->of_node;
#endif
}

static int ptxpmb_cpld_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct ptxpmb_cpld_gpio *chip;
	struct resource *res;
	struct device *dev = &pdev->dev;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	chip->dev = dev;
	platform_set_drvdata(pdev, chip);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	chip->base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!chip->base)
		return -ENOMEM;

	mutex_init(&chip->lock);
	ptxpmb_cpld_gpio_setup(chip);
	ret = gpiochip_add(&chip->gpio);
	if (ret) {
		dev_err(dev, "CPLD gpio: Failed to register GPIO\n");
		return ret;
	}
	return 0;
}

static int ptxpmb_cpld_gpio_remove(struct platform_device *pdev)
{
	struct ptxpmb_cpld_gpio *chip = platform_get_drvdata(pdev);

	gpiochip_remove(&chip->gpio);

	return 0;
}

static const struct of_device_id ptxpmb_cpld_gpio_ids[] = {
	{ .compatible = "jnx,gpio-ptxpmb-cpld", },
	{ },
};
MODULE_DEVICE_TABLE(of, ptxpmb_cpld_gpio_ids);

static struct platform_driver ptxpmb_cpld_gpio_driver = {
	.driver = {
		.name = "gpio-ptxpmb-cpld",
		.owner  = THIS_MODULE,
		.of_match_table = ptxpmb_cpld_gpio_ids,
	},
	.probe = ptxpmb_cpld_gpio_probe,
	.remove = ptxpmb_cpld_gpio_remove,
};

module_platform_driver(ptxpmb_cpld_gpio_driver);

MODULE_DESCRIPTION("CPLD FPGA GPIO Driver");
MODULE_LICENSE("GPL");
