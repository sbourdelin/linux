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
static DEFINE_MUTEX(exar_list_mtx);
static struct ida ida_index;

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
	dev_dbg(chip->gpio_chip.parent, "%s regs=%p offset=%x\n",
		__func__, chip->regs, offset);

	return readb(chip->regs + offset);
}

static inline void write_exar_reg(struct exar_gpio_chip *chip, int offset,
				  int value)
{
	dev_dbg(chip->gpio_chip.parent,
		"%s regs=%p value=%x offset=%x\n",
		__func__, chip->regs, value, offset);
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
	exar_gpio->gpio_chip.owner = THIS_MODULE;
	exar_gpio->regs = p;
	exar_gpio->index = index;

	ret = gpiochip_add(&exar_gpio->gpio_chip);
	if (ret)
		goto err_destroy;

	list_add_tail(&exar_gpio->list, &exar_list);
	mutex_unlock(&exar_list_mtx);

	platform_set_drvdata(pdev, exar_gpio);

	return 0;

err_destroy:
	mutex_unlock(&exar_list_mtx);
	mutex_destroy(&exar_gpio->lock);
err_unmap:
	iounmap(p);
	return ret;
}

static int gpio_exar_remove(struct platform_device *pdev)
{
	struct exar_gpio_chip *exar_gpio, *exar_temp1, *exar_temp2;
	struct pci_dev *pcidev;
	int index;

	exar_gpio = platform_get_drvdata(pdev);
	pcidev = to_pci_dev(exar_gpio->gpio_chip.parent);
	index = exar_gpio->index;

	mutex_lock(&exar_list_mtx);
	list_for_each_entry_safe(exar_temp1, exar_temp2, &exar_list, list) {
		if (exar_temp1->index == exar_gpio->index) {
			list_del(&exar_temp1->list);
			break;
		}
	}
	mutex_unlock(&exar_list_mtx);

	gpiochip_remove(&exar_gpio->gpio_chip);
	mutex_destroy(&exar_gpio->lock);
	iounmap(exar_gpio->regs);
	ida_simple_remove(&ida_index, index);
	platform_set_drvdata(pdev, pcidev);

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

static int __init exar_gpio_init(void)
{
	ida_init(&ida_index);
	platform_driver_register(&gpio_exar_driver);
	return 0;
}

static void __exit exar_gpio_exit(void)
{
	platform_driver_unregister(&gpio_exar_driver);
	ida_destroy(&ida_index);
}

module_init(exar_gpio_init);
module_exit(exar_gpio_exit);

MODULE_DESCRIPTION("Exar GPIO driver");
MODULE_AUTHOR("Sudip Mukherjee <sudipm.mukherjee@gmail.com>");
MODULE_LICENSE("GPL");
