// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 ROHM Semiconductors
// bd71837.c -- ROHM BD71837MWV mfd driver
//
// Datasheet available from
// https://www.rohm.com/datasheet/BD71837MWV/bd71837mwv-e

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/gpio.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/mfd/core.h>
#include <linux/mfd/bd71837.h>

static const struct resource irqs[] = {
	{
		.start = BD71837_INT_PWRBTN,
		.end = BD71837_INT_PWRBTN,
		.flags = IORESOURCE_IRQ,
		.name = "pwr-btn",
	}, {
		.start = BD71837_INT_PWRBTN_L,
		.end = BD71837_INT_PWRBTN_L,
		.flags = IORESOURCE_IRQ,
		.name = "pwr-btn-l",
	}, {
		.start = BD71837_INT_PWRBTN_S,
		.end = BD71837_INT_PWRBTN_S,
		.flags = IORESOURCE_IRQ,
		.name = "pwr-btn-s",
	},
};

/* bd71837 multi function cells */
static struct mfd_cell bd71837_mfd_cells[] = {
	{
		.name = "bd71837-clk",
	}, {
		.name = "bd718xx-pwrkey",
		.resources = &irqs[0],
		.num_resources = ARRAY_SIZE(irqs),
	}, {
		.name = "bd71837-pmic",
	},
};

static const struct regmap_irq bd71837_irqs[] = {
	REGMAP_IRQ_REG(BD71837_INT_SWRST, 0, BD71837_INT_SWRST_MASK),
	REGMAP_IRQ_REG(BD71837_INT_PWRBTN_S, 0, BD71837_INT_PWRBTN_S_MASK),
	REGMAP_IRQ_REG(BD71837_INT_PWRBTN_L, 0, BD71837_INT_PWRBTN_L_MASK),
	REGMAP_IRQ_REG(BD71837_INT_PWRBTN, 0, BD71837_INT_PWRBTN_MASK),
	REGMAP_IRQ_REG(BD71837_INT_WDOG, 0, BD71837_INT_WDOG_MASK),
	REGMAP_IRQ_REG(BD71837_INT_ON_REQ, 0, BD71837_INT_ON_REQ_MASK),
	REGMAP_IRQ_REG(BD71837_INT_STBY_REQ, 0, BD71837_INT_STBY_REQ_MASK),
};

static struct regmap_irq_chip bd71837_irq_chip = {
	.name = "bd71837-irq",
	.irqs = bd71837_irqs,
	.num_irqs = ARRAY_SIZE(bd71837_irqs),
	.num_regs = 1,
	.irq_reg_stride = 1,
	.status_base = BD71837_REG_IRQ,
	.mask_base = BD71837_REG_MIRQ,
	.ack_base = BD71837_REG_IRQ,
	.init_ack_masked = true,
	.mask_invert = false,
};

static int bd71837_irq_exit(struct bd71837 *bd71837)
{
	if (bd71837->chip_irq > 0)
		regmap_del_irq_chip(bd71837->chip_irq, bd71837->irq_data);
	return 0;
}

static const struct regmap_range pmic_status_range = {
	.range_min = BD71837_REG_IRQ,
	.range_max = BD71837_REG_POW_STATE,
};

static const struct regmap_access_table volatile_regs = {
	.yes_ranges = &pmic_status_range,
	.n_yes_ranges = 1,
};

static const struct regmap_config bd71837_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &volatile_regs,
	.max_register = BD71837_MAX_REGISTER - 1,
	.cache_type = REGCACHE_RBTREE,
};

#ifdef CONFIG_OF
static const struct of_device_id bd71837_of_match[] = {
	{ .compatible = "rohm,bd71837", .data = (void *)0},
	{ },
};
MODULE_DEVICE_TABLE(of, bd71837_of_match);
#endif //CONFIG_OF

static int bd71837_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct bd71837 *bd71837;
	struct bd71837_board *board_info;
	int ret = -EINVAL;

	board_info = dev_get_platdata(&i2c->dev);

	if (!board_info) {
		board_info = devm_kzalloc(&i2c->dev, sizeof(*board_info),
					  GFP_KERNEL);
		if (!board_info) {
			ret = -ENOMEM;
			goto err_out;
		} else if (i2c->irq) {
			board_info->gpio_intr = i2c->irq;
		} else {
			ret = -ENOENT;
			goto err_out;
		}
	}

	if (!board_info)
		goto err_out;

	bd71837 = devm_kzalloc(&i2c->dev, sizeof(struct bd71837), GFP_KERNEL);
	if (bd71837 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, bd71837);
	bd71837->dev = &i2c->dev;
	bd71837->i2c_client = i2c;
	bd71837->chip_irq = board_info->gpio_intr;

	bd71837->regmap = devm_regmap_init_i2c(i2c, &bd71837_regmap_config);
	if (IS_ERR(bd71837->regmap)) {
		ret = PTR_ERR(bd71837->regmap);
		dev_err(&i2c->dev, "regmap initialization failed: %d\n", ret);
		goto err_out;
	}

	ret = bd71837_reg_read(bd71837, BD71837_REG_REV);
	if (ret < 0) {
		dev_err(bd71837->dev,
			"%s(): Read BD71837_REG_DEVICE failed!\n", __func__);
		goto err_out;
	}

	ret = regmap_add_irq_chip(bd71837->regmap, bd71837->chip_irq,
		IRQF_ONESHOT, 0,
		&bd71837_irq_chip, &bd71837->irq_data);
	if (ret < 0) {
		dev_err(bd71837->dev, "Failed to add irq_chip %d\n", ret);
		goto err_out;
	}

	ret = mfd_add_devices(bd71837->dev, PLATFORM_DEVID_AUTO,
			      bd71837_mfd_cells, ARRAY_SIZE(bd71837_mfd_cells),
			      NULL, 0,
			      regmap_irq_get_domain(bd71837->irq_data));
	if (ret)
		regmap_del_irq_chip(bd71837->chip_irq, bd71837->irq_data);
err_out:

	return ret;
}

static int bd71837_i2c_remove(struct i2c_client *i2c)
{
	struct bd71837 *bd71837 = i2c_get_clientdata(i2c);

	bd71837_irq_exit(bd71837);
	mfd_remove_devices(bd71837->dev);

	return 0;
}

static const struct i2c_device_id bd71837_i2c_id[] = {
	{ .name = "bd71837", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bd71837_i2c_id);

static struct i2c_driver bd71837_i2c_driver = {
	.driver = {
		.name = "bd71837-mfd",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(bd71837_of_match),
	},
	.probe = bd71837_i2c_probe,
	.remove = bd71837_i2c_remove,
	.id_table = bd71837_i2c_id,
};

static int __init bd71837_i2c_init(void)
{
	return i2c_add_driver(&bd71837_i2c_driver);
}
/* init early so consumer devices can complete system boot */
subsys_initcall(bd71837_i2c_init);

static void __exit bd71837_i2c_exit(void)
{
	i2c_del_driver(&bd71837_i2c_driver);
}
module_exit(bd71837_i2c_exit);

MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_DESCRIPTION("BD71837 chip multi-function driver");
MODULE_LICENSE("GPL");
