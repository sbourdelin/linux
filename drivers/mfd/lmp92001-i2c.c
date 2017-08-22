/*
 * lmp92001-i2c.c - I2C access for TI LMP92001
 *
 * Copyright 2016-2017 Celestica Ltd.
 *
 * Author: Abhisit Sangjan <s.abhisit@gmail.com>
 *
 * Inspired by wm831x driver.
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

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/mfd/lmp92001/core.h>

static const unsigned short lmp92001_i2c_addresses[] = {
	0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E, 0x50, I2C_CLIENT_END
};

/* TODO: To read/write block access, it may need to re-ordering endianness! */
static int lmp92001_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg > 0xff)
		return -EINVAL;

	switch (reg) {
	case LMP92001_ID ... LMP92001_CTRIG:
	case LMP92001_CREF:
		ret = i2c_smbus_read_byte_data(i2c, reg);
		break;
	case LMP92001_ADC1 ... LMP92001_LIL11:
	case LMP92001_DAC1 ... LMP92001_DALL:
		ret = i2c_smbus_read_word_swapped(i2c, reg);
		break;
	case LMP92001_BLK0 ... LMP92001_BLK5:
		ret = i2c_smbus_read_block_data(i2c, reg,
						(u8 *)((uintptr_t)*val));
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	if (reg <= LMP92001_DALL)
		*val = ret;

	return 0;
}

static int lmp92001_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg > 0xff)
		return -EINVAL;

	switch (reg) {
	case LMP92001_ID ... LMP92001_CTRIG:
	case LMP92001_CREF:
		ret = i2c_smbus_write_byte_data(i2c, reg, val);
		break;
	case LMP92001_ADC1 ... LMP92001_LIL11:
	case LMP92001_DAC1 ... LMP92001_DALL:
		ret = i2c_smbus_write_word_swapped(i2c, reg, val);
		break;
	/* To call this function/case, must be passed val as pointer */
	case LMP92001_BLK0:
	case LMP92001_BLK4:
		ret = i2c_smbus_write_block_data(i2c, reg, 24,
						(u8 *)((uintptr_t)val));
		break;
	case LMP92001_BLK1:
	case LMP92001_BLK5:
		ret = i2c_smbus_write_block_data(i2c, reg, 12,
						(u8 *)((uintptr_t)val));
		break;
	case LMP92001_BLK2:
		ret = i2c_smbus_write_block_data(i2c, reg, 34,
						(u8 *)((uintptr_t)val));
		break;
	case LMP92001_BLK3:
		ret = i2c_smbus_write_block_data(i2c, reg, 18,
						(u8 *)((uintptr_t)val));
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static int lmp92001_i2c_probe(struct i2c_client *i2c,
	const struct i2c_device_id *id)
{
	struct lmp92001 *lmp92001;
	int ret;

	lmp92001 = devm_kzalloc(&i2c->dev, sizeof(struct lmp92001), GFP_KERNEL);
	if (!lmp92001)
		return -ENOMEM;

	i2c_set_clientdata(i2c, lmp92001);
	lmp92001->dev = &i2c->dev;

	lmp92001_regmap_config.reg_read = lmp92001_reg_read;
	lmp92001_regmap_config.reg_write = lmp92001_reg_write;

	lmp92001->regmap = devm_regmap_init(&i2c->dev, NULL, &i2c->dev,
						&lmp92001_regmap_config);
	if (IS_ERR(lmp92001->regmap)) {
		ret = PTR_ERR(lmp92001->regmap);
		dev_err(lmp92001->dev, "failed to allocate register map: %d\n",
				ret);
		return ret;
	}

	return lmp92001_device_init(lmp92001, id->driver_data, i2c->irq);
}

static int lmp92001_i2c_remove(struct i2c_client *i2c)
{
	struct lmp92001 *lmp92001 = i2c_get_clientdata(i2c);

	lmp92001_device_exit(lmp92001);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id lmp92001_dt_ids[] = {
	{ .compatible = "ti,lmp92001", },
	{ },
};
MODULE_DEVICE_TABLE(of, lmp92001_dt_ids);
#endif

static const struct i2c_device_id lmp92001_i2c_ids[] = {
	{ "lmp92001" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, lmp92001_i2c_ids);

static int lmp92001_i2c_detect(struct i2c_client *i2c,
	struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = i2c->adapter;
	s32 comid, ver;

	if (!i2c_check_functionality(adapter,
					I2C_FUNC_SMBUS_BYTE_DATA |
					I2C_FUNC_SMBUS_WORD_DATA |
					I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	comid = i2c_smbus_read_byte_data(i2c, LMP92001_ID);
	ver = i2c_smbus_read_byte_data(i2c, LMP92001_VER);

	if (comid != 0x01 || ver != 0x10)
		return -ENODEV;

	return 0;
}

static struct i2c_driver lmp92001_i2c_driver = {
	.driver = {
		.name = "lmp92001",
		.of_match_table = of_match_ptr(lmp92001_dt_ids),
	},
	.probe		= lmp92001_i2c_probe,
	.remove		= lmp92001_i2c_remove,
	.id_table	= lmp92001_i2c_ids,
	.detect		= lmp92001_i2c_detect,
	.address_list	= lmp92001_i2c_addresses,
};

static int __init lmp92001_i2c_init(void)
{
	return i2c_add_driver(&lmp92001_i2c_driver);
}
subsys_initcall(lmp92001_i2c_init);

static void __exit lmp92001_i2c_exit(void)
{
	i2c_del_driver(&lmp92001_i2c_driver);
}
module_exit(lmp92001_i2c_exit);

MODULE_DESCRIPTION("i2c driver for TI LMP92001");
MODULE_AUTHOR("Abhisit Sangjan <s.abhisit@gmail.com>");
MODULE_LICENSE("GPL");
