/*
 * pv88080-i2c.c - I2C access driver for PV88080
 *
 * Copyright (C) 2016  Powerventure Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/mfd/pv88080.h>

static const struct regmap_config pv88080_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct of_device_id pv88080_of_match_table[] = {
	{ .compatible = "pvs,pv88080",    .data = (void *)TYPE_PV88080_AA },
	{ .compatible = "pvs,pv88080-aa", .data = (void *)TYPE_PV88080_AA },
	{ .compatible = "pvs,pv88080-ba", .data = (void *)TYPE_PV88080_BA },
	{ },
};
MODULE_DEVICE_TABLE(of, pv88080_of_match_table);

static int pv88080_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *ids)
{
	struct pv88080 *chip;
	const struct of_device_id *match;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (client->dev.of_node) {
		match = of_match_node(pv88080_of_match_table,
						client->dev.of_node);
		if (!match) {
			dev_err(&client->dev, "Failed to get of_match_node\n");
			return -EINVAL;
		}
		chip->type = (unsigned long)match->data;
	} else {
		chip->type = ids->driver_data;
	}

	i2c_set_clientdata(client, chip);

	chip->dev = &client->dev;
	chip->regmap = devm_regmap_init_i2c(client, &pv88080_regmap_config);
	if (IS_ERR(chip->regmap)) {
		dev_err(chip->dev, "Failed to initialize register map\n");
		return PTR_ERR(chip->regmap);
	}

	return pv88080_device_init(chip, client->irq);
}

static int pv88080_i2c_remove(struct i2c_client *client)
{
	struct pv88080 *chip = i2c_get_clientdata(client);

	return pv88080_device_exit(chip);
}

static const struct i2c_device_id pv88080_i2c_id[] = {
	{ "pv88080",	TYPE_PV88080_AA },
	{ "pv88080-aa", TYPE_PV88080_AA },
	{ "pv88080-ba", TYPE_PV88080_BA },
	{ },
};
MODULE_DEVICE_TABLE(i2c, pv88080_i2c_id);

static struct i2c_driver pv88080_i2c_driver = {
	.driver	 = {
		.name	 = "pv88080",
		.of_match_table = of_match_ptr(pv88080_of_match_table),
	},
	.probe	= pv88080_i2c_probe,
	.remove	= pv88080_i2c_remove,
	.id_table	 = pv88080_i2c_id,
};
module_i2c_driver(pv88080_i2c_driver);

MODULE_AUTHOR("Eric Jeong <eric.jeong.opensource@diasemi.com>");
MODULE_DESCRIPTION("I2C driver for Powerventure PV88080");
MODULE_LICENSE("GPL");

