/*
 * TSC2004 touchscreen driver
 *
 * Copyright (C) 2015 EMAC Inc.
 * Copyright (C) 2015 QWERTY Embedded Design
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/pm.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include "tsc200x-core.h"

static int tsc2004_probe(struct i2c_client *i2c,
			 const struct i2c_device_id *id)

{
	return tsc200x_probe(&i2c->dev, i2c->irq, BUS_I2C,
			     devm_regmap_init_i2c(i2c,
						  &tsc2005_regmap_config));
}

static int tsc2004_remove(struct i2c_client *i2c)
{
	return tsc200x_remove(&i2c->dev);
}

static const struct i2c_device_id tsc2004_idtable[] = {
	{ "tsc2004", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, tsc2004_idtable);

#ifdef CONFIG_OF
static const struct of_device_id tsc2004_of_match[] = {
	{ .compatible = "ti,tsc2004" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tsc2004_of_match);
#endif

static SIMPLE_DEV_PM_OPS(tsc2004_pm_ops, tsc200x_suspend, tsc200x_resume);

static struct i2c_driver tsc2004_driver = {
	.driver = {
		.name	= "tsc2004",
		.of_match_table = of_match_ptr(tsc2004_of_match),
		.pm	= &tsc2004_pm_ops,
	},
	.id_table	= tsc2004_idtable,
	.probe		= tsc2004_probe,
	.remove		= tsc2004_remove,
};

module_i2c_driver(tsc2004_driver);

MODULE_AUTHOR("Michael Welling <mwelling@ieee.org>");
MODULE_DESCRIPTION("TSC2004 Touchscreen Driver");
MODULE_LICENSE("GPL");
