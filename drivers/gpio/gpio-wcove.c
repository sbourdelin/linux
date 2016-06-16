/*
 * gpio-wcove.c - Intel Whiskey Cove GPIO Driver
 *
 * This driver is written based on gpio-crystalcove.c
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/seq_file.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/intel_soc_pmic.h>

#define DRV_NAME "bxt_wcove_gpio"

/*
 * Whiskey Cove PMIC has 13 physical GPIO pins divided into 3 banks:
 * Bank 0: Pin 0 - 6
 * Bank 1: Pin 7 - 10
 * Bank 2: Pin 11 -12
 * Each pin has one output control register and one input control register.
 */
#define BANK0_NR_PINS		7
#define BANK1_NR_PINS		4
#define BANK2_NR_PINS		2
#define WCOVE_GPIO_NUM		(BANK0_NR_PINS + BANK1_NR_PINS + BANK2_NR_PINS)
#define WCOVE_VGPIO_NUM		94
/* GPIO output control registers(one per pin): 0x4e44 - 0x4e50 */
#define GPIO_OUT_CTRL_BASE	0x4e44
/* GPIO input control registers(one per pin): 0x4e51 - 0x4e5d */
#define GPIO_IN_CTRL_BASE	0x4e51

/*
 * GPIO interrupts are organized in two groups:
 * Group 0: Bank 0 pins (Pin 0 - 6)
 * Group 1: Bank 1 and Bank 2 pins (Pin 7 - 12)
 * Each group has two registers(one bit per pin): status and mask.
 */
#define GROUP0_NR_IRQS		7
#define GROUP1_NR_IRQS		6
#define IRQ_MASK_BASE		0x4e19
#define IRQ_STATUS_BASE		0x4e0b
#define UPDATE_IRQ_TYPE		BIT(0)
#define UPDATE_IRQ_MASK		BIT(1)

#define CTLI_INTCNT_DIS		(0)
#define CTLI_INTCNT_NE		(1 << 1)
#define CTLI_INTCNT_PE		(2 << 1)
#define CTLI_INTCNT_BE		(3 << 1)

#define CTLO_DIR_IN		(0)
#define CTLO_DIR_OUT		(1 << 5)

#define CTLO_DRV_MASK		(1 << 4)
#define CTLO_DRV_OD		(0)
#define CTLO_DRV_CMOS		CTLO_DRV_MASK

#define CTLO_DRV_REN		(1 << 3)

#define CTLO_RVAL_2KDW		(0)
#define CTLO_RVAL_2KUP		(1 << 1)
#define CTLO_RVAL_50KDW		(2 << 1)
#define CTLO_RVAL_50KUP		(3 << 1)

#define CTLO_INPUT_SET	(CTLO_DRV_CMOS | CTLO_DRV_REN | CTLO_RVAL_2KUP)
#define CTLO_OUTPUT_SET	(CTLO_DIR_OUT | CTLO_INPUT_SET)

enum ctrl_register {
	CTRL_IN,
	CTRL_OUT,
};

/*
 * struct wcove_gpio - Whiskey Cove GPIO controller
 * @buslock: for bus lock/sync and unlock.
 * @chip: the abstract gpio_chip structure.
 * @regmap: the regmap from the parent device.
 * @update: pending IRQ setting update, to be written to the chip upon unlock.
 * @intcnt_value: the Interrupt Detect value to be written.
 * @set_irq_mask: true if the IRQ mask needs to be set, false to clear.
 */
struct wcove_gpio {
	struct mutex buslock; /* irq_bus_lock */
	struct gpio_chip chip;
	struct regmap *regmap;
	struct regmap_irq_chip_data *regmap_irq_chip;
	int update;
	int intcnt_value;
	bool set_irq_mask;
};

static inline unsigned int to_reg(int gpio, enum ctrl_register reg_type)
{
	unsigned int reg;
	int bank;

	if (gpio < BANK0_NR_PINS)
		bank = 0;
	else if (gpio < (BANK0_NR_PINS + BANK1_NR_PINS))
		bank = 1;
	else
		bank = 2;

	if (reg_type == CTRL_IN)
		reg = GPIO_IN_CTRL_BASE + bank;
	else
		reg = GPIO_OUT_CTRL_BASE + bank;

	return reg;
}

static void wcove_update_irq_mask(struct wcove_gpio *wg,
					int gpio)
{
	int group;
	unsigned int reg, bit;

	group = gpio < GROUP0_NR_IRQS ? 0 : 1;
	reg = IRQ_MASK_BASE + group;
	bit = (group == 0) ? BIT(gpio % GROUP0_NR_IRQS) :
		BIT((gpio - GROUP0_NR_IRQS) % GROUP1_NR_IRQS);

	if (wg->set_irq_mask)
		regmap_update_bits(wg->regmap, reg, bit, 1);
	else
		regmap_update_bits(wg->regmap, reg, bit, 0);
}

static void wcove_update_irq_ctrl(struct wcove_gpio *wg, int gpio)
{
	unsigned int reg = to_reg(gpio, CTRL_IN);

	regmap_update_bits(wg->regmap, reg, CTLI_INTCNT_BE, wg->intcnt_value);
}

static int wcove_gpio_dir_in(struct gpio_chip *chip, unsigned int gpio)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);

	return regmap_write(wg->regmap, to_reg(gpio, CTRL_OUT),
			    CTLO_INPUT_SET);
}

static int wcove_gpio_dir_out(struct gpio_chip *chip, unsigned int gpio,
				    int value)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);

	return regmap_write(wg->regmap, to_reg(gpio, CTRL_OUT),
			    CTLO_OUTPUT_SET | value);
}

static int wcove_gpio_get(struct gpio_chip *chip, unsigned int gpio)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);
	int ret;
	unsigned int val;

	ret = regmap_read(wg->regmap, to_reg(gpio, CTRL_IN), &val);
	if (ret)
		return ret;

	return val & 0x1;
}

static void wcove_gpio_set(struct gpio_chip *chip,
				 unsigned int gpio, int value)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);

	if (value)
		regmap_update_bits(wg->regmap, to_reg(gpio, CTRL_OUT), 1, 1);
	else
		regmap_update_bits(wg->regmap, to_reg(gpio, CTRL_OUT), 1, 0);
}

static int wcove_gpio_set_single_ended(struct gpio_chip *chip,
					unsigned int gpio,
					enum single_ended_mode mode)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);

	switch (mode) {
	case LINE_MODE_OPEN_DRAIN:
		return regmap_update_bits(wg->regmap, to_reg(gpio, CTRL_OUT),
						CTLO_DRV_MASK, CTLO_DRV_OD);
	case LINE_MODE_PUSH_PULL:
		return regmap_update_bits(wg->regmap, to_reg(gpio, CTRL_OUT),
						CTLO_DRV_MASK, CTLO_DRV_CMOS);
	default:
		break;
	}

	return -ENOTSUPP;
}

static int wcove_irq_type(struct irq_data *data, unsigned int type)
{
	struct wcove_gpio *wg = gpiochip_get_data(
		irq_data_get_irq_chip_data(data));

	switch (type) {
	case IRQ_TYPE_NONE:
		wg->intcnt_value = CTLI_INTCNT_DIS;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		wg->intcnt_value = CTLI_INTCNT_BE;
		break;
	case IRQ_TYPE_EDGE_RISING:
		wg->intcnt_value = CTLI_INTCNT_PE;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		wg->intcnt_value = CTLI_INTCNT_NE;
		break;
	default:
		return -EINVAL;
	}

	wg->update |= UPDATE_IRQ_TYPE;

	return 0;
}

static void wcove_bus_lock(struct irq_data *data)
{
	struct wcove_gpio *wg = gpiochip_get_data(
		irq_data_get_irq_chip_data(data));

	mutex_lock(&wg->buslock);
}

static void wcove_bus_sync_unlock(struct irq_data *data)
{
	struct wcove_gpio *wg = gpiochip_get_data(
		irq_data_get_irq_chip_data(data));
	int gpio = data->hwirq;

	if (wg->update & UPDATE_IRQ_TYPE)
		wcove_update_irq_ctrl(wg, gpio);
	if (wg->update & UPDATE_IRQ_MASK)
		wcove_update_irq_mask(wg, gpio);
	wg->update = 0;

	mutex_unlock(&wg->buslock);
}

static void wcove_irq_unmask(struct irq_data *data)
{
	struct wcove_gpio *wg = gpiochip_get_data(
		irq_data_get_irq_chip_data(data));

	wg->set_irq_mask = false;
	wg->update |= UPDATE_IRQ_MASK;
}

static void wcove_irq_mask(struct irq_data *data)
{
	struct wcove_gpio *wg = gpiochip_get_data(
		irq_data_get_irq_chip_data(data));

	wg->set_irq_mask = true;
	wg->update |= UPDATE_IRQ_MASK;
}

static struct irq_chip wcove_irqchip = {
	.name			= "Whiskey Cove",
	.irq_mask		= wcove_irq_mask,
	.irq_unmask		= wcove_irq_unmask,
	.irq_set_type		= wcove_irq_type,
	.irq_bus_lock		= wcove_bus_lock,
	.irq_bus_sync_unlock	= wcove_bus_sync_unlock,
};

static irqreturn_t wcove_gpio_irq_handler(int irq, void *data)
{
	struct wcove_gpio *wg = data;
	unsigned int p0, p1, virq;
	int pending, gpio;

	if (regmap_read(wg->regmap, IRQ_STATUS_BASE, &p0) ||
	    regmap_read(wg->regmap, IRQ_STATUS_BASE + 1, &p1)) {
		pr_err("%s(): regmap_read() failed.\n", __func__);
		return IRQ_NONE;
	}

	pending = p0 | (p1 << 8);

	for (gpio = 0; gpio < WCOVE_GPIO_NUM; gpio++) {
		if (pending & BIT(gpio)) {
			virq = irq_find_mapping(wg->chip.irqdomain, gpio);
			handle_nested_irq(virq);
		}
	}

	regmap_write(wg->regmap, IRQ_STATUS_BASE, p0);
	regmap_write(wg->regmap, IRQ_STATUS_BASE + 1, p1);

	return IRQ_HANDLED;
}

static void wcove_gpio_dbg_show(struct seq_file *s,
				      struct gpio_chip *chip)
{
	struct wcove_gpio *wg = gpiochip_get_data(chip);
	int gpio, offset, group;
	unsigned int ctlo, ctli, irq_mask, irq_status;

	for (gpio = 0; gpio < WCOVE_GPIO_NUM; gpio++) {
		group = gpio < GROUP0_NR_IRQS ? 0 : 1;
		regmap_read(wg->regmap, to_reg(gpio, CTRL_OUT), &ctlo);
		regmap_read(wg->regmap, to_reg(gpio, CTRL_IN), &ctli);
		regmap_read(wg->regmap, IRQ_MASK_BASE + group, &irq_mask);
		regmap_read(wg->regmap, IRQ_STATUS_BASE + group, &irq_status);

		offset = gpio % 8;
		seq_printf(s, " gpio-%-2d %s %s %s %s ctlo=%2x,%s %s\n",
			   gpio, ctlo & CTLO_DIR_OUT ? "out" : "in ",
			   ctli & 0x1 ? "hi" : "lo",
			   ctli & CTLI_INTCNT_NE ? "fall" : "    ",
			   ctli & CTLI_INTCNT_PE ? "rise" : "    ",
			   ctlo,
			   irq_mask & BIT(offset) ? "mask  " : "unmask",
			   irq_status & BIT(offset) ? "pending" : "       ");
	}
}

static int wcove_gpio_probe(struct platform_device *pdev)
{
	int virq, retval, irq = platform_get_irq(pdev, 0);
	struct wcove_gpio *wg;
	struct device *dev = pdev->dev.parent;
	struct intel_soc_pmic *pmic = dev_get_drvdata(dev);

	if (irq < 0)
		return irq;

	wg = devm_kzalloc(&pdev->dev, sizeof(*wg), GFP_KERNEL);
	if (!wg)
		return -ENOMEM;

	wg->regmap_irq_chip = pmic->irq_chip_data_level2;

	platform_set_drvdata(pdev, wg);

	mutex_init(&wg->buslock);
	wg->chip.label = KBUILD_MODNAME;
	wg->chip.direction_input = wcove_gpio_dir_in;
	wg->chip.direction_output = wcove_gpio_dir_out;
	wg->chip.get = wcove_gpio_get;
	wg->chip.set = wcove_gpio_set;
	wg->chip.set_single_ended = wcove_gpio_set_single_ended,
	wg->chip.base = -1;
	wg->chip.ngpio = WCOVE_VGPIO_NUM;
	wg->chip.can_sleep = true;
	wg->chip.parent = dev;
	wg->chip.dbg_show = wcove_gpio_dbg_show;
	wg->regmap = pmic->regmap;

	retval = devm_gpiochip_add_data(&pdev->dev, &wg->chip, wg);
	if (retval) {
		dev_warn(&pdev->dev, "add gpio chip error: %d\n", retval);
		return retval;
	}

	gpiochip_irqchip_add(&wg->chip, &wcove_irqchip, 0,
			     handle_simple_irq, IRQ_TYPE_NONE);

	virq = regmap_irq_get_virq(wg->regmap_irq_chip, irq);
	if (virq < 0) {
		dev_err(&pdev->dev,
				"failed to get virtual interrupt=%d\n", irq);
		retval = virq;
		goto out_remove_gpio;
	}

	retval = devm_request_threaded_irq(&pdev->dev, virq,
			NULL, wcove_gpio_irq_handler,
			IRQF_ONESHOT, pdev->name, wg);

	if (retval) {
		dev_warn(&pdev->dev, "request irq failed: %d, virq: %d\n",
							retval, virq);
		goto out_remove_gpio;
	}

	return 0;

out_remove_gpio:
	gpiochip_remove(&wg->chip);
	return retval;
}

static int wcove_gpio_remove(struct platform_device *pdev)
{
	struct wcove_gpio *wg = platform_get_drvdata(pdev);

	gpiochip_remove(&wg->chip);
	return 0;
}

static struct platform_driver wcove_gpio_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = wcove_gpio_probe,
	.remove = wcove_gpio_remove,
};

module_platform_driver(wcove_gpio_driver);

MODULE_AUTHOR("Ajay Thomas <ajay.thomas.david.rajamanickam@intel.com>");
MODULE_DESCRIPTION("Intel Whiskey Cove GPIO Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
