/*
 * GPIO driver for Exar XR17V35X chip
 *
 * Copyright (C) 2015 Sudip Mukherjee <sudipm.mukherjee@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/gpio.h>

#define EXAR_OFFSET_MPIOLVL_LO 0x90
#define EXAR_OFFSET_MPIOSEL_LO 0x93
#define EXAR_OFFSET_MPIOLVL_HI 0x96
#define EXAR_OFFSET_MPIOSEL_HI 0x99

static LIST_HEAD(exar_list);
static DEFINE_MUTEX(exar_mtx); /* lock while manipulating the list */

struct exar_gpio_chip {
	struct gpio_chip gpio_chip;
	struct mutex lock;
	struct list_head list;
	int index;
	void __iomem *regs;
	char name[16];
};

#define to_exar_chip(n) container_of(n, struct exar_gpio_chip, gpio_chip)

static inline unsigned int read_exar_reg(struct exar_gpio_chip *chip,
					 int offset)
{
	pr_debug("%s regs=%p offset=%x\n", __func__, chip->regs, offset);
	return readb(chip->regs + offset);
}

static inline void write_exar_reg(struct exar_gpio_chip *chip, int offset,
				  int value)
{
	pr_debug("%s regs=%p value=%x offset=%x\n", __func__, chip->regs,
		 value, offset);
	writeb(value, chip->regs + offset);
}

static void exar_set(struct gpio_chip *chip, unsigned int reg, int val,
		     unsigned int offset)
{
	struct exar_gpio_chip *exar_gpio = to_exar_chip(chip);
	int temp;

	mutex_lock(&exar_gpio->lock);
	temp = read_exar_reg(exar_gpio, reg);
	temp &= ~(1 << offset);
	temp |= val << offset;
	write_exar_reg(exar_gpio, reg, temp);
	mutex_unlock(&exar_gpio->lock);
}

static int exar_direction_output(struct gpio_chip *chip, unsigned int offset,
				 int value)
{
	if (offset < 8)
		exar_set(chip, EXAR_OFFSET_MPIOSEL_LO, 0, offset);
	else
		exar_set(chip, EXAR_OFFSET_MPIOSEL_HI, 0, offset - 8);
	return 0;
}

static int exar_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	if (offset < 8)
		exar_set(chip, EXAR_OFFSET_MPIOSEL_LO, 1, offset);
	else
		exar_set(chip, EXAR_OFFSET_MPIOSEL_HI, 1, offset - 8);
	return 0;
}

static int exar_get(struct gpio_chip *chip, unsigned int reg)
{
	int value;
	struct exar_gpio_chip *exar_gpio = to_exar_chip(chip);

	if (!exar_gpio) {
		pr_err("%s exar_gpio is NULL\n", __func__);
		return -ENOMEM;
	}

	mutex_lock(&exar_gpio->lock);
	value = read_exar_reg(exar_gpio, reg);
	mutex_unlock(&exar_gpio->lock);

	return value;
}

static int exar_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	int val;

	if (offset < 8) {
		val = exar_get(chip, EXAR_OFFSET_MPIOSEL_LO);
	} else {
		val = exar_get(chip, EXAR_OFFSET_MPIOSEL_HI);
		offset -= 8;
	}

	if (val > 0) {
		val >>= offset;
		val &= 0x01;
	}

	return val;
}

static int exar_get_value(struct gpio_chip *chip, unsigned int offset)
{
	int val;

	if (offset < 8) {
		val = exar_get(chip, EXAR_OFFSET_MPIOLVL_LO);
	} else {
		val = exar_get(chip, EXAR_OFFSET_MPIOLVL_HI);
		offset -= 8;
	}
	val >>= offset;
	val &= 0x01;

	return val;
}

static void exar_set_value(struct gpio_chip *chip, unsigned int offset,
			   int value)
{
	if (offset < 8)
		exar_set(chip, EXAR_OFFSET_MPIOLVL_LO, value, offset);
	else
		exar_set(chip, EXAR_OFFSET_MPIOLVL_HI, value, offset - 8);
}

static int gpio_exar_probe(struct platform_device *pdev)
{
	struct pci_dev *dev = platform_get_drvdata(pdev);
	struct exar_gpio_chip *exar_gpio, *exar_temp;
	void __iomem *p;
	int index = 1;
	int ret;

	if (dev->vendor != PCI_VENDOR_ID_EXAR)
		return -ENODEV;

	p = pci_ioremap_bar(dev, 0);
	if (!p)
		return -ENOMEM;

	exar_gpio = devm_kzalloc(&dev->dev, sizeof(*exar_gpio), GFP_KERNEL);
	if (!exar_gpio) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	mutex_init(&exar_gpio->lock);
	INIT_LIST_HEAD(&exar_gpio->list);

	mutex_lock(&exar_mtx);
	/* find the first unused index */
	list_for_each_entry(exar_temp, &exar_list, list) {
		if (exar_temp->index == index) {
			index++;
			continue;
		}
	}

	sprintf(exar_gpio->name, "exar_gpio%d", index);
	exar_gpio->gpio_chip.label = exar_gpio->name;
	exar_gpio->gpio_chip.parent = &dev->dev;
	exar_gpio->gpio_chip.direction_output = exar_direction_output;
	exar_gpio->gpio_chip.direction_input = exar_direction_input;
	exar_gpio->gpio_chip.get_direction = exar_get_direction;
	exar_gpio->gpio_chip.get = exar_get_value;
	exar_gpio->gpio_chip.set = exar_set_value;
	exar_gpio->gpio_chip.base = -1;
	exar_gpio->gpio_chip.ngpio = 16;
	exar_gpio->gpio_chip.owner = THIS_MODULE;
	exar_gpio->regs = p;
	exar_gpio->index = index;

	ret = gpiochip_add(&exar_gpio->gpio_chip);
	if (ret)
		goto err_destroy;

	list_add_tail(&exar_gpio->list, &exar_list);
	mutex_unlock(&exar_mtx);

	platform_set_drvdata(pdev, exar_gpio);

	return 0;

err_destroy:
	mutex_unlock(&exar_mtx);
	mutex_destroy(&exar_gpio->lock);
err_unmap:
	iounmap(p);
	return ret;
}

static int gpio_exar_remove(struct platform_device *pdev)
{
	struct exar_gpio_chip *exar_gpio, *exar_temp1, *exar_temp2;

	exar_gpio = platform_get_drvdata(pdev);

	mutex_lock(&exar_mtx);
	list_for_each_entry_safe(exar_temp1, exar_temp2, &exar_list, list) {
		if (exar_temp1->index == exar_gpio->index) {
			list_del(&exar_temp1->list);
			break;
		}
	}
	mutex_unlock(&exar_mtx);

	gpiochip_remove(&exar_gpio->gpio_chip);
	mutex_destroy(&exar_gpio->lock);
	iounmap(exar_gpio->regs);

	return 0;
}

static struct platform_driver gpio_exar_driver = {
	.probe	= gpio_exar_probe,
	.remove	= gpio_exar_remove,
	.driver	= {
		.name = "gpio_exar",
	},
};

static const struct platform_device_id gpio_exar_id[] = {
	{ "gpio_exar", 0},
	{ },
};

MODULE_DEVICE_TABLE(platform, gpio_exar_id);

module_platform_driver(gpio_exar_driver)

MODULE_DESCRIPTION("Exar GPIO driver");
MODULE_AUTHOR("Sudip Mukherjee <sudipm.mukherjee@gmail.com>");
MODULE_LICENSE("GPL");
