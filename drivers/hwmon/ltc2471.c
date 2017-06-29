/*
 * Driver for Linear Technology LTC2471 and LTC2473 voltage monitors
 * The LTC2473 is identical to the 2471, but reports a differential signal.
 *
 * Copyright (C) 2017 Topic Embedded Products
 * Author: Mike Looijmans <mike.looijmans@topic.nl>
 *
 * License: GPLv2
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

enum chips {
	ltc2471,
	ltc2473
};

struct ltc2471_data {
	struct i2c_client *i2c;
	bool differential;
};

/* Reference voltage is 1.25V */
#define LTC2471_VREF 1250

/* Read two bytes from the I2C bus to obtain the ADC result */
static int ltc2471_get_value(struct i2c_client *i2c)
{
	int ret;
	__be16 buf;

	ret = i2c_master_recv(i2c, (char *)&buf, 2);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	/* MSB first */
	return be16_to_cpu(buf);
}

static ssize_t ltc2471_show_value(struct device *dev,
				  struct device_attribute *da, char *buf)
{
	struct ltc2471_data *data = dev_get_drvdata(dev);
	int value;

	value = ltc2471_get_value(data->i2c);
	if (unlikely(value < 0))
		return value;

	if (data->differential)
		/* Ranges from -VREF to +VREF with "0" at 0x8000 */
		value = ((s32)LTC2471_VREF * (s32)(value - 0x8000)) >> 15;
	else
		/* Ranges from 0 to +VREF */
		value = ((u32)LTC2471_VREF * (u32)value) >> 16;

	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ltc2471_show_value, NULL, 0);

static struct attribute *ltc2471_attrs[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	NULL
};

ATTRIBUTE_GROUPS(ltc2471);

static int ltc2471_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	int ret;
	struct device *hwmon_dev;
	struct ltc2471_data *data;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	data = devm_kzalloc(&i2c->dev, sizeof(struct ltc2471_data), GFP_KERNEL);
	if (unlikely(!data))
		return -ENOMEM;

	data->i2c = i2c;
	data->differential = (id->driver_data == ltc2473);

	/* Trigger once to start conversion and check if chip is there */
	ret = ltc2471_get_value(i2c);
	if (ret < 0) {
		dev_err(&i2c->dev, "Cannot read from device.\n");
		return ret;
	}

	hwmon_dev = devm_hwmon_device_register_with_groups(&i2c->dev,
							   i2c->name,
							   data,
							   ltc2471_groups);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id ltc2471_i2c_id[] = {
	{ "ltc2471", ltc2471 },
	{ "ltc2473", ltc2473 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ltc2471_i2c_id);

static struct i2c_driver ltc2471_i2c_driver = {
	.driver = {
		.name = "ltc2471",
	},
	.probe    = ltc2471_i2c_probe,
	.id_table = ltc2471_i2c_id,
};

module_i2c_driver(ltc2471_i2c_driver);

MODULE_DESCRIPTION("LTC2471/LTC2473 Sensor Driver");
MODULE_AUTHOR("Topic Embedded Products");
MODULE_LICENSE("GPL v2");
