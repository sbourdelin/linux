/*
 * Copyright (C) 2017 Mentor Graphics
 *
 * Author: Valentin Sitdikov <Valentin.Sitdikov@mentor.com>
 * Author: Andrei Dranitca <Andrei_Dranitca@mentor.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
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


int max7360_request_pin(struct max7360 *max7360, u8 pin)
{
	struct i2c_client *client = max7360->i2c;
	int ret = 0;

	spin_lock(&max7360->lock);
	if (max7360->gpio_pins & BIT(pin)) {
		dev_err(&client->dev, "pin %d already requested, mask %x",
		       pin, max7360->gpio_pins);
		spin_unlock(&max7360->lock);
		return -EBUSY;
	}
	max7360->gpio_pins |= BIT(pin);
	dev_dbg(&client->dev, "pin %d requested successfully", pin);
	spin_unlock(&max7360->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(max7360_request_pin);

void max7360_free_pin(struct max7360 *max7360, u8 pin)
{
	spin_lock(&max7360->lock);
	max7360->gpio_pins &= ~BIT(pin);
	spin_unlock(&max7360->lock);
}
EXPORT_SYMBOL_GPL(max7360_free_pin);

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

static int max7360_irq_init(struct max7360 *max7360, struct device_node *np)
{
	int ret;

	max7360->inti = of_irq_get_byname(np, "inti");
	max7360->intk = of_irq_get_byname(np, "intk");

	if (max7360->inti < 0) {
		dev_err(max7360->dev, "no inti provided");
		return -ENODEV;
	}

	if (max7360->intk < 0) {
		dev_err(max7360->dev, "no intk provided");
		return -ENODEV;
	}

	if (max7360->inti == max7360->intk) {
		max7360->shared_irq = max7360->inti;
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
		ret = request_threaded_irq(max7360->inti, NULL, max7360_irqi,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  "max7360", max7360);
		if (ret) {
			dev_err(max7360->dev, "failed to request inti IRQ: %d\n",
			       ret);
			return ret;
		}

		ret = request_threaded_irq(max7360->intk, NULL, max7360_irqk,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  "max7360", max7360);
		if (ret) {
			free_irq(max7360->inti, max7360);
			dev_err(max7360->dev, "failed to request intk IRQ: %d\n",
				ret);
			return ret;
		}
	}

	max7360->domain = irq_domain_add_simple(np, MAX7360_NR_INTERNAL_IRQS,
					       0, &max7360_irq_ops, max7360);

	if (!max7360->domain) {
		if (max7360->shared_irq)
			free_irq(max7360->shared_irq, max7360);
		else {
			free_irq(max7360->inti, max7360);
			free_irq(max7360->intk, max7360);
		}
		dev_err(max7360->dev, "Failed to create irqdomain\n");
		return -ENODEV;
	}

	irq_create_mapping(max7360->domain, MAX7360_INT_GPIO);
	irq_create_mapping(max7360->domain, MAX7360_INT_KEYPAD);
	irq_create_mapping(max7360->domain, MAX7360_INT_ROTARY);

	return 0;
}

void max7360_fall_deepsleep(struct max7360 *max7360)
{
	max7360_write_reg(max7360, MAX7360_REG_SLEEP, MAX7360_AUTOSLEEP_8192);
}
EXPORT_SYMBOL_GPL(max7360_fall_deepsleep);

void max7360_take_catnap(struct max7360 *max7360)
{
	max7360_write_reg(max7360, MAX7360_REG_SLEEP, MAX7360_AUTOSLEEP_256);
}
EXPORT_SYMBOL_GPL(max7360_take_catnap);

static int max7360_chip_init(struct max7360 *max7360)
{
	max7360->gpio_pins = MAX7360_MAX_GPIO;
	max7360->gpo_count = 0;
	max7360->col_count = MAX7360_COL_GPO_PINS;
	return 0;
}

static int max7360_device_init(struct max7360 *max7360)
{
	int ret = 0;

	ret = mfd_add_devices(max7360->dev, -1, max7360_devices,
			     ARRAY_SIZE(max7360_devices), NULL,
			     0, max7360->domain);
	if (ret)
		dev_err(max7360->dev, "failed to add child devices\n");

	return ret;
}

int max7360_request_gpo_pin_count(struct max7360 *max7360, u8 count)
{
	if (count > MAX7360_MAX_GPO)
		return -EINVAL;
	if (max7360->col_count + count > MAX7360_COL_GPO_PINS) {
		dev_err(max7360->dev,
		       "trying to request %d pins as gpo while %d pins already used as COL\n",
		       count, max7360->col_count);
		return -EINVAL;
	}
	max7360->gpo_count = count;
	return 0;
}
EXPORT_SYMBOL_GPL(max7360_request_gpo_pin_count);

int max7360_request_col_count(struct max7360 *max7360, u8 count)
{
	if (max7360->gpo_count + count > MAX7360_COL_GPO_PINS) {
		dev_err(max7360->dev,
		       "trying to request %d pins as COL while %d pins already used as gpo\n",
		       count, max7360->gpo_count);
		return -EINVAL;
	}
	max7360->col_count = count;
	return 0;
}
EXPORT_SYMBOL_GPL(max7360_request_col_count);

static const struct regmap_range max7360_volatile_ranges[] = {
	{
		.range_min = MAX7360_REG_KEYFIFO,
		.range_max = MAX7360_REG_KEYFIFO,
	}, {
		.range_min = 0x48,
		.range_max = 0x4a,
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

	spin_lock_init(&max7360->lock);

	max7360->dev = &i2c->dev;
	max7360->i2c = i2c;

	i2c_set_clientdata(i2c, max7360);

	max7360->regmap = devm_regmap_init_i2c(i2c, &max7360_regmap_config);
	ret = max7360_chip_init(max7360);
	if (ret)
		return ret;

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

static int max7360_remove(struct i2c_client *client)
{
	struct max7360 *max7360 = i2c_get_clientdata(client);

	mfd_remove_devices(max7360->dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int max7360_suspend(struct device *dev)
{
	return 0;
}

static int max7360_resume(struct device *dev)
{
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(max7360_dev_pm_ops, max7360_suspend, max7360_resume);

static const struct of_device_id max7360_match[] = {
	{ .compatible = "maxim,max7360" },
	{ }
};

MODULE_DEVICE_TABLE(of, max7360_match);

static const struct i2c_device_id max7360_id[] = {
	{ "max7360", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max7360_id);

static struct i2c_driver max7360_driver = {
	.driver = {
		.name	= "max7360",
		.pm	= &max7360_dev_pm_ops,
		.of_match_table = max7360_match,
	},
	.probe		= max7360_probe,
	.remove		= max7360_remove,
	.id_table	= max7360_id,
};

static int __init max7360_init(void)
{
	return i2c_add_driver(&max7360_driver);
}
subsys_initcall(max7360_init);

static void __exit max7360_exit(void)
{
	i2c_del_driver(&max7360_driver);
}
module_exit(max7360_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MAX7360 MFD core driver");
