/*
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Author: Keerthy <j-keerthy@ti.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

#include <linux/mfd/lp87565.h>

static const struct regmap_config lp87565_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LP87565_REG_MAX,
};

static const struct mfd_cell lp87565_cells[] = {
	{ .name = "lp87565-q1-regulator", },
	{ .name = "lp87565-q1-gpio", },
};

static const struct of_device_id of_lp87565_match_table[] = {
	{
		.compatible = "ti,lp87565",
	},
	{
		.compatible = "ti,lp87565-q1",
		.data = (void *)LP87565_DEVICE_TYPE_LP87565_Q1,
	},
	{}
};
MODULE_DEVICE_TABLE(of, of_lp87565_match_table);

static int lp87565_probe(struct i2c_client *client)
{
	struct lp87565 *lp87565;
	const struct of_device_id *of_id;
	int ret;
	unsigned int otpid;

	lp87565 = devm_kzalloc(&client->dev, sizeof(*lp87565), GFP_KERNEL);
	if (!lp87565)
		return -ENOMEM;

	lp87565->dev = &client->dev;

	lp87565->regmap = devm_regmap_init_i2c(client, &lp87565_regmap_config);
	if (IS_ERR(lp87565->regmap)) {
		ret = PTR_ERR(lp87565->regmap);
		dev_err(lp87565->dev,
			"Failed to initialize register map: %d\n", ret);
		return ret;
	}

	ret = regmap_read(lp87565->regmap, LP87565_REG_OTP_REV, &otpid);
	if (ret) {
		dev_err(lp87565->dev, "Failed to read OTP ID\n");
		return ret;
	}

	lp87565->rev = otpid & LP87565_OTP_REV_OTP_ID;

	of_id = of_match_device(of_lp87565_match_table, &client->dev);
	if (of_id)
		lp87565->dev_type = (enum lp87565_device_type)of_id->data;

	i2c_set_clientdata(client, lp87565);

	ret = mfd_add_devices(lp87565->dev, PLATFORM_DEVID_AUTO, lp87565_cells,
			      ARRAY_SIZE(lp87565_cells), NULL, 0, NULL);

	return ret;
}

static struct i2c_driver lp87565_driver = {
	.driver	= {
		.name	= "lp87565",
		.of_match_table = of_lp87565_match_table,
	},
	.probe_new		= lp87565_probe,
};
module_i2c_driver(lp87565_driver);

MODULE_AUTHOR("J Keerthy <j-keerthy@ti.com>");
MODULE_DESCRIPTION("lp87565 chip family Multi-Function Device driver");
MODULE_LICENSE("GPL v2");
