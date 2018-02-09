/*
 * ADS1015 Texas Instruments ADC, I2C bits
 *
 * Copyright (C) 2018 Georgiana Chelu <georgiana.chelu93@gmail.com>
 *
 * IIO driver for ADS1015 ADC 7-bit I2C slave address:
 *	* 0x48 - ADDR connected to Ground
 *	* 0x49 - ADDR connected to Vdd
 *	* 0x4A - ADDR connected to SDA
 *	* 0x4B - ADDR connected to SCL
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#include "ti-ads1015.h"

static int ads1015_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct regmap *regmap;
	const char *name = NULL;

	regmap = devm_regmap_init_i2c(client, &ads1015_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "failed to allocate i2c register map\n");
		return PTR_ERR(regmap);
	}

	if (id)
		name = id->name;

	return ads1015_core_probe(&client->dev, regmap, name,
				  client->irq, id->driver_data);
}

static int ads1015_i2c_remove(struct i2c_client *client)
{
	return ads1015_core_remove(&client->dev);
}

static const struct i2c_device_id ads1015_i2c_id[] = {
	{"ads1015", ADS1015},
	{"ads1115", ADS1115},
	{},
};
MODULE_DEVICE_TABLE(i2c, ads1015_i2c_id);

static const struct of_device_id ads1015_of_i2c_match[] = {
	{
		.compatible = "ti,ads1015",
		.data = (void *)ADS1015
	},
	{
		.compatible = "ti,ads1115",
		.data = (void *)ADS1115
	},
	{},
};
MODULE_DEVICE_TABLE(of, ads1015_of_i2c_match);

static struct i2c_driver ads1015_i2c_driver = {
	.driver = {
		.name = ADS1015_DRV_NAME,
		.of_match_table = of_match_ptr(ads1015_of_i2c_match),
		.pm = &ads1015_pm_ops,
	},
	.probe		= ads1015_i2c_probe,
	.remove		= ads1015_i2c_remove,
	.id_table	= ads1015_i2c_id,
};

module_i2c_driver(ads1015_i2c_driver);

MODULE_AUTHOR("Georgiana Chelu <georgiana.chelu93@gmail.com>");
MODULE_DESCRIPTION("Texas Instruments ADS1015 ADC driver I2C");
MODULE_LICENSE("GPL v2");
