/*
 * Driver for Linear Technology LTC2990 power monitor
 *
 * Copyright (C) 2014 Topic Embedded Products
 * Author: Mike Looijmans <mike.looijmans@topic.nl>
 *
 * License: GPLv2
 *
 * To configure the driver, pass its desired "mode" (lower 3 bits of control
 * register) as its device name. Depending on this mode, the chip will report
 * temperature, current and/or voltage measuments. It always reports the chip's
 * internal temperature and Vcc power supply voltage.
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define LTC2990_STATUS	0x00
#define LTC2990_CONTROL	0x01
#define LTC2990_TRIGGER	0x02
#define LTC2990_TINT_MSB	0x04
#define LTC2990_V1_MSB	0x06
#define LTC2990_V2_MSB	0x08
#define LTC2990_V3_MSB	0x0A
#define LTC2990_V4_MSB	0x0C
#define LTC2990_VCC_MSB	0x0E
#define LTC2990_REGISTER_MASK	0x0F

#define LTC2990_CONTROL_KELVIN		BIT(7)
#define LTC2990_CONTROL_SINGLE		BIT(6)
#define LTC2990_CONTROL_MEASURE_ALL	(0x3 << 3)
#define LTC2990_CONTROL_MODE_DEFAULT	0x06

#define LTC2990_CONVERSION_CURRENT	0x00
#define LTC2990_CONVERSION_TEMPERATURE	0x40
#define LTC2990_CONVERSION_VOLTAGE	0x80
#define LTC2990_CONVERSION_VOLTAGE25	0xC0
#define LTC2990_CONVERSION_MASK 	0xC0

/* convert raw register value to sign-extended integer in 16-bit range */
static int ltc2990_voltage_to_int(int raw)
{
	if (raw & BIT(14))
		return -(0x4000 - (raw & 0x3FFF)) << 2;
	else
		return (raw & 0x3FFF) << 2;
}

/* Return the converted value from the given register in uV or mC */
static int ltc2990_get_value(struct i2c_client *i2c, u8 conv_reg, int *result)
{
	int val;

	val = i2c_smbus_read_word_swapped(i2c, conv_reg & LTC2990_REGISTER_MASK);
	if (unlikely(val < 0))
		return val;

	switch (conv_reg & LTC2990_CONVERSION_MASK) {
	case LTC2990_CONVERSION_CURRENT:
		 /* Current as differential Vx-Vy, 19.42uV/LSB. */
		*result = ltc2990_voltage_to_int(val) * 1942 / (4 * 100);
		break;
	case LTC2990_CONVERSION_TEMPERATURE:
		/* Temperature, 0.0625 degrees/LSB, 13-bit  */
		val = (val & 0x1FFF) << 3;
		*result = (val * 1000) >> 7;
		break;
	case LTC2990_CONVERSION_VOLTAGE:
		/* Voltage, 305.18μV/LSB */
		*result = (ltc2990_voltage_to_int(val) * 30518 /
			   (4 * 100 * 1000));
		break;
	case LTC2990_CONVERSION_VOLTAGE25:
		/* Vcc, 305.18μV/LSB, 2.5V offset */
		*result = (ltc2990_voltage_to_int(val) * 30518 /
			   (4 * 100 * 1000)) + 2500;
		break;
	default:
		return -EINVAL; /* won't happen, keep compiler happy */
	}

	return 0;
}

static ssize_t ltc2990_show_value(struct device *dev,
				  struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int value;
	int ret;

	ret = ltc2990_get_value(dev_get_drvdata(dev), attr->index, &value);
	if (unlikely(ret < 0))
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

/* Internal temperature and Vcc voltage are always present */
static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_TEMPERATURE | LTC2990_TINT_MSB);
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_VOLTAGE25 | LTC2990_VCC_MSB);
/* Current measurement requires 2 inputs per channel */
static SENSOR_DEVICE_ATTR(curr1_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_CURRENT | LTC2990_V1_MSB);
static SENSOR_DEVICE_ATTR(curr2_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_CURRENT | LTC2990_V3_MSB);
/* Voltage measurement requires 1 input per channel */
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_VOLTAGE | LTC2990_V1_MSB);
static SENSOR_DEVICE_ATTR(in2_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_VOLTAGE | LTC2990_V2_MSB);
static SENSOR_DEVICE_ATTR(in3_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_VOLTAGE | LTC2990_V3_MSB);
static SENSOR_DEVICE_ATTR(in4_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_VOLTAGE | LTC2990_V4_MSB);
/* Temperature measurement requires 2 inputs per channel */
static SENSOR_DEVICE_ATTR(temp2_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_TEMPERATURE | LTC2990_V1_MSB);
static SENSOR_DEVICE_ATTR(temp3_input, S_IRUGO, ltc2990_show_value, NULL,
			  LTC2990_CONVERSION_TEMPERATURE | LTC2990_V3_MSB);


/* Common attributes regardless of mode */
static struct attribute *ltc2990_common_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_in0_input.dev_attr.attr,
	NULL,
};
static const struct attribute_group ltc2990_common_group = {
	.attrs = ltc2990_common_attrs,
};

/* Attribute to mode mapping, as per the CONTROL register bits [2:0] */

/* 000 = V1 V2 TR2 */
static struct attribute *ltc2990_000_attrs[] = {
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	NULL,
};
/* 001 = V1-V2 TR2 */
static struct attribute *ltc2990_001_attrs[] = {
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	NULL,
};
/* 010 = V1-V2 V3 V4 */
static struct attribute *ltc2990_010_attrs[] = {
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	NULL,
};
/* 011 = TR1 V3 V4 */
static struct attribute *ltc2990_011_attrs[] = {
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	NULL,
};
/* 100 = TR1 V3-V4 */
static struct attribute *ltc2990_100_attrs[] = {
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_curr2_input.dev_attr.attr,
	NULL,
};
/* 101 = TR1 TR2 */
static struct attribute *ltc2990_101_attrs[] = {
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	NULL,
};
/* 110 = V1-V2 V3-V4 */
static struct attribute *ltc2990_110_attrs[] = {
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_curr2_input.dev_attr.attr,
	NULL,
};
/* 111 = V1 V2 V3 V4 */
static struct attribute *ltc2990_111_attrs[] = {
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	NULL,
};
/* Decoder array for mode index */
static struct attribute **ltc2990_attr_list[] = {
	ltc2990_000_attrs,
	ltc2990_001_attrs,
	ltc2990_010_attrs,
	ltc2990_011_attrs,
	ltc2990_100_attrs,
	ltc2990_101_attrs,
	ltc2990_110_attrs,
	ltc2990_111_attrs,
};

struct ltc2990_driver_data {
	struct attribute_group ltc2990_extra_group;
	const struct attribute_group *ltc2990_groups[3];
};

static int ltc2990_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	int ret;
	struct device *hwmon_dev;
	struct ltc2990_driver_data *data;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	/* Setup continuous monitoring mode */
	ret = i2c_smbus_write_byte_data(i2c, LTC2990_CONTROL,
					LTC2990_CONTROL_MEASURE_ALL |
					id->driver_data);
	if (ret < 0) {
		dev_err(&i2c->dev, "Error: Failed to set control mode.\n");
		return ret;
	}
	/* Trigger once to start continuous conversion */
	ret = i2c_smbus_write_byte_data(i2c, LTC2990_TRIGGER, 1);
	if (ret < 0) {
		dev_err(&i2c->dev, "Error: Failed to start acquisition.\n");
		return ret;
	}

	/* Define attributes based on chip mode */
	data = devm_kzalloc(&i2c->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->ltc2990_extra_group.attrs = ltc2990_attr_list[id->driver_data];
	data->ltc2990_groups[0] = &ltc2990_common_group;
	data->ltc2990_groups[1] = &data->ltc2990_extra_group;

	hwmon_dev = devm_hwmon_device_register_with_groups(&i2c->dev,
							   i2c->name,
							   i2c,
							   data->ltc2990_groups);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id ltc2990_i2c_id[] = {
	{ "ltc2990",   LTC2990_CONTROL_MODE_DEFAULT},
	{ "ltc29900", 0},
	{ "ltc29901", 1},
	{ "ltc29902", 2},
	{ "ltc29903", 3},
	{ "ltc29904", 4},
	{ "ltc29905", 5},
	{ "ltc29906", 6},
	{ "ltc29907", 7},
	{}
};
MODULE_DEVICE_TABLE(i2c, ltc2990_i2c_id);

static struct i2c_driver ltc2990_i2c_driver = {
	.driver = {
		.name = "ltc2990",
	},
	.probe    = ltc2990_i2c_probe,
	.id_table = ltc2990_i2c_id,
};

module_i2c_driver(ltc2990_i2c_driver);

MODULE_DESCRIPTION("LTC2990 Sensor Driver");
MODULE_AUTHOR("Topic Embedded Products");
MODULE_LICENSE("GPL v2");
