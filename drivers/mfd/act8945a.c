/*
 * MFD driver for Active-semi ACT8945a PMIC
 *
 * Copyright (C) 2015 Atmel Corporation.
 *                    Wenyou Yang <wenyou.yang@atmel.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/mfd/act8945a.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

static const struct mfd_cell act8945a_devs[] = {
	{
		.name = "act8945a-pmic",
		.of_compatible = "active-semi,act8945a-regulator",
	},
	{
		.name = "act8945a-charger",
		.of_compatible = "active-semi,act8945a-charger",
	},
};

static const struct regmap_config act8945a_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int act8945a_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct act8945a_dev *act8945a;
	int ret;

	act8945a = devm_kzalloc(&client->dev, sizeof(*act8945a), GFP_KERNEL);
	if (act8945a == NULL)
		return -ENOMEM;

	i2c_set_clientdata(client, act8945a);

	act8945a->dev = &client->dev;
	act8945a->i2c_client = client;
	act8945a->regmap = devm_regmap_init_i2c(client,
						&act8945a_regmap_config);
	if (IS_ERR(act8945a->regmap)) {
		ret = PTR_ERR(act8945a->regmap);
		dev_err(&client->dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	ret = mfd_add_devices(act8945a->dev, -1, act8945a_devs,
			      ARRAY_SIZE(act8945a_devs), NULL, 0, NULL);
	if (ret < 0) {
		dev_err(&client->dev, "mfd_add_devices failed: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "added %zu mfd sub-devices\n",
		 ARRAY_SIZE(act8945a_devs));

	return 0;

}

static int act8945a_i2c_remove(struct i2c_client *i2c)
{
	struct act8945a_dev *act8945a = i2c_get_clientdata(i2c);

	mfd_remove_devices(act8945a->dev);

	return 0;
}

static const struct i2c_device_id act8945a_i2c_id[] = {
	{ "act8945a", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, act8945a_i2c_id);

static const struct of_device_id act8945a_of_match[] = {
	{.compatible = "active-semi,act8945a", },
	{},
};
MODULE_DEVICE_TABLE(of, act8945a_of_match);

static struct i2c_driver act8945a_i2c_driver = {
	.driver = {
		   .name = "act8945a",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(act8945a_of_match),
	},
	.probe = act8945a_i2c_probe,
	.remove = act8945a_i2c_remove,
	.id_table = act8945a_i2c_id,
};

static int __init act8945a_i2c_init(void)
{
	return i2c_add_driver(&act8945a_i2c_driver);
}
subsys_initcall(act8945a_i2c_init);

static void __exit act8945a_i2c_exit(void)
{
	i2c_del_driver(&act8945a_i2c_driver);
}
module_exit(act8945a_i2c_exit);

MODULE_DESCRIPTION("ACT8945A PMIC multi-function driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Wenyou Yang <wenyou.yang@atmel.com>");
