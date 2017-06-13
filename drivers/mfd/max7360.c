/*
 * Copyright (C) 2017 Mentor Graphics
 *
 * Author: Andrei Dranitca <Andrei_Dranitca@mentor.com>
 * Author: Valentin Sitdikov <Valentin.Sitdikov@mentor.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/slab.h>


static const struct mfd_cell max7360_devices[] = {
	{
		.name           = "max7360-gpio",
		.of_compatible	= "maxim,max7360-gpio",
	},
	{
		.name           = "max7360-keypad",
		.of_compatible	= "maxim,max7360-keypad",
	},
	{
		.name           = "max7360-pwm",
		.of_compatible	= "maxim,max7360-pwm",
	},
	{
		.name           = "max7360-rotary",
		.of_compatible	= "maxim,max7360-rotary",
	},
};

static irqreturn_t max7360_irq(int irq, void *data)
{
	struct max7360 *max7360 = data;
	int virq;

	virq = irq_find_mapping(max7360->domain, MAX7360_INT_GPIO);
	handle_nested_irq(virq);
	virq = irq_find_mapping(max7360->domain, MAX7360_INT_KEYPAD);
	handle_nested_irq(virq);
	virq = irq_find_mapping(max7360->domain, MAX7360_INT_ROTARY);
	handle_nested_irq(virq);

	return IRQ_HANDLED;
}

static irqreturn_t max7360_irqi(int irq, void *data)
{
	struct max7360 *max7360 = data;
	int virq;

	virq = irq_find_mapping(max7360->domain, MAX7360_INT_GPIO);
	handle_nested_irq(virq);
	virq = irq_find_mapping(max7360->domain, MAX7360_INT_ROTARY);
	handle_nested_irq(virq);

	return IRQ_HANDLED;
}

static irqreturn_t max7360_irqk(int irq, void *data)
{
	struct max7360 *max7360 = data;
	int virq;

	virq = irq_find_mapping(max7360->domain, MAX7360_INT_KEYPAD);
	handle_nested_irq(virq);

	return IRQ_HANDLED;
}

static int max7360_irq_map(struct irq_domain *d, unsigned int virq,
			  irq_hw_number_t hwirq)
{
	struct max7360 *max7360 = d->host_data;

	irq_set_chip_data(virq, max7360);
	irq_set_chip_and_handler(virq, &dummy_irq_chip,
				handle_edge_irq);
	irq_set_nested_thread(virq, 1);
	irq_set_noprobe(virq);

	return 0;
}

static void max7360_irq_unmap(struct irq_domain *d, unsigned int virq)
{
	irq_set_chip_and_handler(virq, NULL, NULL);
	irq_set_chip_data(virq, NULL);
}

static const struct irq_domain_ops max7360_irq_ops = {
	.map    = max7360_irq_map,
	.unmap  = max7360_irq_unmap,
	.xlate  = irq_domain_xlate_onecell,
};

static void max7360_free_irqs(struct max7360 *max7360)
{
	if (max7360->shared_irq)
		free_irq(max7360->shared_irq, max7360);
	else {
		free_irq(max7360->irq_inti, max7360);
		free_irq(max7360->irq_intk, max7360);
	}
}

static int max7360_irq_init(struct max7360 *max7360, struct device_node *np)
{
	int ret;

	max7360->irq_inti = of_irq_get_byname(np, "inti");
	if (max7360->irq_inti < 0) {
		dev_err(max7360->dev, "no inti provided");
		return -ENODEV;
	}

	max7360->irq_intk = of_irq_get_byname(np, "intk");
	if (max7360->irq_intk < 0) {
		dev_err(max7360->dev, "no intk provided");
		return -ENODEV;
	}

	if (max7360->irq_inti == max7360->irq_intk) {
		/*
		 * In case of pin inti and pin intk are the connected
		 * to the same soc`s irq pin.
		 */
		max7360->shared_irq = max7360->irq_inti;
		ret = request_threaded_irq(max7360->shared_irq, NULL,
					  max7360_irq,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  "max7360", max7360);
		if (ret) {
			dev_err(max7360->dev, "failed to request IRQ: %d\n",
				ret);
			return ret;
		}
	} else {
		max7360->shared_irq = 0;
		ret = request_threaded_irq(max7360->irq_inti, NULL,
					  max7360_irqi,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  "max7360", max7360);
		if (ret) {
			dev_err(max7360->dev, "failed to request inti IRQ: %d\n",
			       ret);
			return ret;
		}

		ret = request_threaded_irq(max7360->irq_intk, NULL,
					  max7360_irqk,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  "max7360", max7360);
		if (ret) {
			free_irq(max7360->irq_inti, max7360);
			dev_err(max7360->dev, "failed to request intk IRQ: %d\n",
				ret);
			return ret;
		}
	}

	max7360->domain = irq_domain_add_simple(np, MAX7360_NR_INTERNAL_IRQS,
					       0, &max7360_irq_ops, max7360);

	if (!max7360->domain) {
		max7360_free_irqs(max7360);
		dev_err(max7360->dev, "Failed to create irqdomain\n");
		return -ENODEV;
	}

	ret = irq_create_mapping(max7360->domain, MAX7360_INT_GPIO);
	if (!ret) {
		max7360_free_irqs(max7360);
		dev_err(max7360->dev, "Failed to map GPIO IRQ\n");
		return -EINVAL;
	}

	ret = irq_create_mapping(max7360->domain, MAX7360_INT_KEYPAD);
	if (!ret) {
		max7360_free_irqs(max7360);
		dev_err(max7360->dev, "Failed to map KEYPAD IRQ\n");
		return -EINVAL;
	}

	ret = irq_create_mapping(max7360->domain, MAX7360_INT_ROTARY);
	if (!ret) {
		max7360_free_irqs(max7360);
		dev_err(max7360->dev, "Failed to map ROTARY IRQ\n");
		return -EINVAL;
	}

	return 0;
}
static int max7360_device_init(struct max7360 *max7360)
{
	int ret;

	ret = devm_mfd_add_devices(max7360->dev, PLATFORM_DEVID_NONE,
			     max7360_devices,
			     ARRAY_SIZE(max7360_devices), NULL,
			     0, max7360->domain);
	if (ret)
		dev_err(max7360->dev, "failed to add child devices\n");

	return ret;
}

static const struct regmap_range max7360_volatile_ranges[] = {
	{
		.range_min = MAX7360_REG_KEYFIFO,
		.range_max = MAX7360_REG_KEYFIFO,
	}, {
		.range_min = MAX7360_REG_GPIOIN,
		.range_max = MAX7360_REG_GPIOIN,
	},
};

static const struct regmap_access_table max7360_volatile_table = {
	.yes_ranges = max7360_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(max7360_volatile_ranges),
};

static const struct regmap_config max7360_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.volatile_table = &max7360_volatile_table,
	.cache_type = REGCACHE_RBTREE,
};

static int max7360_probe(struct i2c_client *i2c,
			const struct i2c_device_id *id)
{
	struct device_node *np = i2c->dev.of_node;
	struct max7360 *max7360;
	int ret;

	max7360 = devm_kzalloc(&i2c->dev, sizeof(struct max7360),
			      GFP_KERNEL);
	if (!max7360)
		return -ENOMEM;

	max7360->dev = &i2c->dev;
	max7360->i2c = i2c;

	i2c_set_clientdata(i2c, max7360);

	max7360->regmap = devm_regmap_init_i2c(i2c, &max7360_regmap_config);

	ret = max7360_irq_init(max7360, np);
	if (ret)
		return ret;

	ret = max7360_device_init(max7360);
	if (ret) {
		dev_err(max7360->dev, "failed to add child devices\n");
		return ret;
	}

	return 0;
}

static const struct of_device_id max7360_match[] = {
	{ .compatible = "maxim,max7360" },
	{ }
};
MODULE_DEVICE_TABLE(of, max7360_match);

static struct i2c_driver max7360_driver = {
	.driver = {
		.name = "max7360",
		.of_match_table = max7360_match,
	},
	.probe = max7360_probe,
};
builtin_i2c_driver(max7360_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MAX7360 MFD core driver");
