/*
 * ADXL345 3-Axis Digital Accelerometer
 *
 * Copyright (c) 2017 Eva Rachel Retuya <eraretuya@gmail.com>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License. See the file COPYING in the main
 * directory of this archive for more details.
 *
 * I2C driver for ADXL345
 * 7-bit I2C slave address: 0x1D (ALT ADDRESS pin tied to VDDIO) or
 * 0x53 (ALT ADDRESS pin grounded)
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include "adxl345.h"

static const struct regmap_config adxl345_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int adxl345_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct regmap *regmap;
	const char *name = NULL;

	regmap = devm_regmap_init_i2c(client, &adxl345_i2c_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "Error initializing i2c regmap: %d\n",
			(int)PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	if (id)
		name = id->name;

	return adxl345_common_probe(&client->dev, regmap, name);
}

static int adxl345_i2c_remove(struct i2c_client *client)
{
	return adxl345_common_remove(&client->dev);
}

static const struct i2c_device_id adxl345_i2c_id[] = {
	{ "adxl345", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, adxl345_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id adxl345_of_match[] = {
	{ .compatible = "adi,adxl345" },
	{ },
};

MODULE_DEVICE_TABLE(of, adxl345_of_match);
#endif

static struct i2c_driver adxl345_i2c_driver = {
	.driver = {
		.name	= "adxl345_i2c",
		.of_match_table = of_match_ptr(adxl345_of_match),
	},
	.probe		= adxl345_i2c_probe,
	.remove		= adxl345_i2c_remove,
	.id_table	= adxl345_i2c_id,
};

module_i2c_driver(adxl345_i2c_driver);

MODULE_AUTHOR("Eva Rachel Retuya <eraretuya@gmail.com>");
MODULE_DESCRIPTION("ADXL345 3-Axis Digital Accelerometer I2C driver");
MODULE_LICENSE("GPL v2");
