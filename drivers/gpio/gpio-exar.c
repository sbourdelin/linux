/*
 * GPIO driver for Exar XR17V35X chip
 *
 * Copyright (C) 2015 Sudip Mukherjee <sudip.mukherjee@codethink.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#define EXAR_OFFSET_MPIOLVL_LO 0x90
#define EXAR_OFFSET_MPIOSEL_LO 0x93
#define EXAR_OFFSET_MPIOLVL_HI 0x96
#define EXAR_OFFSET_MPIOSEL_HI 0x99

#define DRIVER_NAME "gpio_exar"

static LIST_HEAD(exar_list);
static DEFINE_MUTEX(exar_list_mtx);
DEFINE_IDA(ida_index);

struct exar_gpio_chip {
	struct gpio_chip gpio_chip;
	struct pci_dev *pcidev;
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
	dev_dbg(chip->gpio_chip.parent, "regs=%p offset=%x\n",
		chip->regs, offset);

	return readb(chip->regs + offset);
}

static inline void write_exar_reg(struct exar_gpio_chip *chip, int offset,
				  int value)
{
	dev_dbg(chip->gpio_chip.parent,
		"regs=%p value=%x offset=%x\n", chip->regs, value,
		offset);
	writeb(value, chip->regs + offset);
}

static void exar_update(struct gpio_chip *chip, unsigned int reg, int val,
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

static int exar_set_direction(struct gpio_chip *chip, int direction,
			      unsigned int offset)
{
	if (offset < 8)
		exar_update(chip, EXAR_OFFSET_MPIOSEL_LO, direction,
			    offset);
	else
		exar_update(chip, EXAR_OFFSET_MPIOSEL_HI, direction,
			    offset - 8);
	return 0;
}

static int exar_direction_output(struct gpio_chip *chip, unsigned int offset,
				 int value)
{
	return exar_set_direction(chip, 0, offset);
}

static int exar_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	return exar_set_direction(chip, 1, offset);
}

static int exar_get(struct gpio_chip *chip, unsigned int reg)
{
	struct exar_gpio_chip *exar_gpio = to_exar_chip(chip);
	int value;

	mutex_lock(&exar_gpio->lock);
	value = read_exar_reg(exar_gpio, reg);
	mutex_unlock(&exar_gpio->lock);

	return value;
}

static int exar_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	int val;

	if (offset < 8)
		val = exar_get(chip, EXAR_OFFSET_MPIOSEL_LO) >> offset;
	else
		val = exar_get(chip, EXAR_OFFSET_MPIOSEL_HI) >>
			       (offset - 8);

	return val & 0x01;
}

static int exar_get_value(struct gpio_chip *chip, unsigned int offset)
{
	int val;

	if (offset < 8)
		val = exar_get(chip, EXAR_OFFSET_MPIOLVL_LO) >> offset;
	else
		val = exar_get(chip, EXAR_OFFSET_MPIOLVL_HI) >>
			       (offset - 8);
	return val & 0x01;
}

static void exar_set_value(struct gpio_chip *chip, unsigned int offset,
			   int value)
{
	if (offset < 8)
		exar_update(chip, EXAR_OFFSET_MPIOLVL_LO, value,
			    offset);
	else
		exar_update(chip, EXAR_OFFSET_MPIOLVL_HI, value,
			    offset - 8);
}

static int gpio_exar_probe(struct platform_device *pdev)
{
	struct pci_dev *dev = platform_get_drvdata(pdev);
	struct exar_gpio_chip *exar_gpio;
	void __iomem *p;
	int index = 1;
	int ret;

	if (dev->vendor != PCI_VENDOR_ID_EXAR)
		return -ENODEV;

	p = pci_ioremap_bar(dev, 0);
	if (!p)
		return -ENOMEM;

	exar_gpio = devm_kzalloc(&dev->dev, sizeof(*exar_gpio), GFP_KERNEL);
	if (!exar_gpio)
		return -ENOMEM;

	mutex_init(&exar_gpio->lock);
	INIT_LIST_HEAD(&exar_gpio->list);

	index = ida_simple_get(&ida_index, 0, 0, GFP_KERNEL);
	mutex_lock(&exar_list_mtx);

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
	exar_gpio->regs = p;
	exar_gpio->index = index;
	exar_gpio->pcidev = dev;

	ret = gpiochip_add(&exar_gpio->gpio_chip);
	if (ret)
		goto err_destroy;

	list_add_tail(&exar_gpio->list, &exar_list);
	mutex_unlock(&exar_list_mtx);

	return 0;

err_destroy:
	mutex_unlock(&exar_list_mtx);
	mutex_destroy(&exar_gpio->lock);
	return ret;
}

static int gpio_exar_remove(struct platform_device *pdev)
{
	struct exar_gpio_chip *exar_gpio, *exar_temp;
	struct pci_dev *pcidev;
	int index;

	pcidev = platform_get_drvdata(pdev);

	mutex_lock(&exar_list_mtx);
	list_for_each_entry_safe(exar_gpio, exar_temp, &exar_list, list) {
		if (exar_gpio->pcidev == pcidev) {
			list_del(&exar_gpio->list);
			break;
		}
	}
	mutex_unlock(&exar_list_mtx);

	index = exar_gpio->index;
	gpiochip_remove(&exar_gpio->gpio_chip);
	mutex_destroy(&exar_gpio->lock);
	ida_simple_remove(&ida_index, index);

	return 0;
}

static struct platform_driver gpio_exar_driver = {
	.probe	= gpio_exar_probe,
	.remove	= gpio_exar_remove,
	.driver	= {
		.name = DRIVER_NAME,
	},
};

module_platform_driver(gpio_exar_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("Exar GPIO driver");
MODULE_AUTHOR("Sudip Mukherjee <sudip.mukherjee@codethink.co.uk>");
MODULE_LICENSE("GPL");
