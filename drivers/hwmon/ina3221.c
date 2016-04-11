/*
 * INA3221 Triple Current/Voltage Monitor
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

#define INA3221_DRIVER_NAME		"ina3221"

#define INA3221_CONFIG			0x00
#define INA3221_SHUNT1			0x01
#define INA3221_BUS1			0x02
#define INA3221_SHUNT2			0x03
#define INA3221_BUS2			0x04
#define INA3221_SHUNT3			0x05
#define INA3221_BUS3			0x06
#define INA3221_CRIT1			0x07
#define INA3221_WARN1			0x08
#define INA3221_CRIT2			0x09
#define INA3221_WARN2			0x0a
#define INA3221_CRIT3			0x0b
#define INA3221_WARN3			0x0c
#define INA3221_SHUNT_SUM		0x0d
#define INA3221_SHUNT_SUM_LIMIT		0x0e
#define INA3221_MASK_ENABLE		0x0f
#define INA3221_POWERV_HLIMIT		0x10
#define INA3221_POWERV_LLIMIT		0x11

#define INA3221_CONFIG_MODE_SHUNT	BIT(1)
#define INA3221_CONFIG_MODE_BUS		BIT(2)
#define INA3221_CONFIG_MODE_CONTINUOUS	BIT(3)

enum ina3221_fields {
	/* Configuration */
	F_MODE, F_SHUNT_CT, F_BUS_CT, F_AVG,
	F_CHAN3_EN, F_CHAN2_EN, F_CHAN1_EN, F_RST,

	/* sentinel */
	F_MAX_FIELDS
};

static const struct reg_field ina3221_reg_fields[] = {
	[F_MODE]	= REG_FIELD(INA3221_CONFIG, 0, 2),
	[F_SHUNT_CT]	= REG_FIELD(INA3221_CONFIG, 3, 5),
	[F_BUS_CT]	= REG_FIELD(INA3221_CONFIG, 6, 8),
	[F_AVG]		= REG_FIELD(INA3221_CONFIG, 9, 11),
	[F_CHAN3_EN]	= REG_FIELD(INA3221_CONFIG, 12, 12),
	[F_CHAN2_EN]	= REG_FIELD(INA3221_CONFIG, 13, 13),
	[F_CHAN1_EN]	= REG_FIELD(INA3221_CONFIG, 14, 14),
	[F_RST]		= REG_FIELD(INA3221_CONFIG, 15, 15),
};

enum ina3221_channels {
	INA3221_CHANNEL1,
	INA3221_CHANNEL2,
	INA3221_CHANNEL3,
	INA3221_NUM_CHANNELS
};

static const int shunt_registers[] = {
	[INA3221_CHANNEL1] = INA3221_SHUNT1,
	[INA3221_CHANNEL2] = INA3221_SHUNT2,
	[INA3221_CHANNEL3] = INA3221_SHUNT3,
};

static const int bus_registers[] = {
	[INA3221_CHANNEL1] = INA3221_BUS1,
	[INA3221_CHANNEL2] = INA3221_BUS2,
	[INA3221_CHANNEL3] = INA3221_BUS3,
};

/**
 * struct ina3221_data - device specific information
 * @dev: Device structure
 * @regmap: Register map of the device
 * @fields: Register fields of the device
 */
struct ina3221_data {
	struct device *dev;
	struct regmap *regmap;
	struct regmap_field *fields[F_MAX_FIELDS];
	unsigned int shunt_resistors[INA3221_NUM_CHANNELS];
};

static int ina3221_read_value(struct ina3221_data *ina, unsigned int reg,
			      unsigned int *val)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(ina->regmap, reg, &regval);
	if (ret)
		return ret;

	*val = sign_extend32(regval >> 3, 12);

	return 0;
}

static ssize_t ina3221_show_voltage(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int reg = sd_attr->index;
	int val, voltage_mv, ret;

	ret = ina3221_read_value(ina, reg, &val);
	if (ret)
		return ret;

	if (reg == INA3221_BUS1 ||
	    reg == INA3221_BUS2 ||
	    reg == INA3221_BUS3)
		voltage_mv = val * 8;
	else
		voltage_mv = DIV_ROUND_CLOSEST(val * 40, 1000);

	return snprintf(buf, PAGE_SIZE, "%d\n", voltage_mv);
}

static ssize_t ina3221_set_voltage(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int reg = sd_attr->index;
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	val = DIV_ROUND_CLOSEST(val, 40000) << 3;

	ret = regmap_write(ina->regmap, reg, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t ina3221_show_current(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	unsigned int shunt_reg, resistance_uo;
	int val, current_ma, shunt_voltage_mv, ret;

	shunt_reg = shunt_registers[channel];
	resistance_uo = ina->shunt_resistors[channel];
	if (resistance_uo == 0) {
		current_ma = 0;
		goto resistance_is_futile;
	}

	ret = ina3221_read_value(ina, shunt_reg, &val);
	if (ret)
		return ret;
	shunt_voltage_mv = val * 40000;

	current_ma = DIV_ROUND_CLOSEST(shunt_voltage_mv, resistance_uo);
resistance_is_futile:
	return snprintf(buf, PAGE_SIZE, "%d\n", current_ma);
}

static ssize_t ina3221_show_power(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	unsigned int shunt_reg, bus_reg, resistance_uo;
	int val, power_uw, current_ma, shunt_voltage_mv, bus_voltage_mv, ret;

	shunt_reg = shunt_registers[channel];
	bus_reg = bus_registers[channel];
	resistance_uo = ina->shunt_resistors[channel];
	if (resistance_uo == 0) {
		power_uw = 0;
		goto sorry_i_couldnt_resist;
	}

	ret = ina3221_read_value(ina, shunt_reg, &val);
	if (ret)
		return ret;
	shunt_voltage_mv = val * 40000;

	current_ma = DIV_ROUND_CLOSEST(shunt_voltage_mv, resistance_uo);

	ret = ina3221_read_value(ina, bus_reg, &val);
	if (ret)
		return ret;
	bus_voltage_mv = val * 8;

	power_uw = current_ma * bus_voltage_mv;
sorry_i_couldnt_resist:
	return snprintf(buf, PAGE_SIZE, "%d\n", power_uw);
}

static ssize_t ina3221_show_shunt(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	unsigned int resistance_uo;

	resistance_uo = ina->shunt_resistors[channel];

	return snprintf(buf, PAGE_SIZE, "%d\n", resistance_uo);
}

static ssize_t ina3221_set_shunt(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sensor_device_attribute *sd_attr = to_sensor_dev_attr(attr);
	struct ina3221_data *ina = dev_get_drvdata(dev);
	unsigned int channel = sd_attr->index;
	int ret;

	ret = kstrtouint(buf, 10, &ina->shunt_resistors[channel]);
	if (ret)
		return ret;

	return count;
}

/* bus voltages */
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_BUS1);
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_BUS2);
static SENSOR_DEVICE_ATTR(in2_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_BUS3);

/* shunt voltages */
static SENSOR_DEVICE_ATTR(in3_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_SHUNT1);
static SENSOR_DEVICE_ATTR(in4_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_SHUNT2);
static SENSOR_DEVICE_ATTR(in5_input, S_IRUGO, ina3221_show_voltage, NULL, INA3221_SHUNT3);

/* calculated current */
static SENSOR_DEVICE_ATTR(curr0_input, S_IRUGO, ina3221_show_current, NULL, INA3221_CHANNEL1);
static SENSOR_DEVICE_ATTR(curr1_input, S_IRUGO, ina3221_show_current, NULL, INA3221_CHANNEL2);
static SENSOR_DEVICE_ATTR(curr2_input, S_IRUGO, ina3221_show_current, NULL, INA3221_CHANNEL3);

/* calculated power */
static SENSOR_DEVICE_ATTR(power0_input, S_IRUGO, ina3221_show_power, NULL, INA3221_CHANNEL1);
static SENSOR_DEVICE_ATTR(power1_input, S_IRUGO, ina3221_show_power, NULL, INA3221_CHANNEL2);
static SENSOR_DEVICE_ATTR(power2_input, S_IRUGO, ina3221_show_power, NULL, INA3221_CHANNEL3);

/* shunt resistance */
static SENSOR_DEVICE_ATTR(shunt0_resistor, (S_IRUGO | S_IWUSR), ina3221_show_shunt, ina3221_set_shunt, INA3221_CHANNEL1);
static SENSOR_DEVICE_ATTR(shunt1_resistor, (S_IRUGO | S_IWUSR), ina3221_show_shunt, ina3221_set_shunt, INA3221_CHANNEL2);
static SENSOR_DEVICE_ATTR(shunt2_resistor, (S_IRUGO | S_IWUSR), ina3221_show_shunt, ina3221_set_shunt, INA3221_CHANNEL3);

/* critical alert */
static SENSOR_DEVICE_ATTR(critical0_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_CRIT1);
static SENSOR_DEVICE_ATTR(critical1_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_CRIT2);
static SENSOR_DEVICE_ATTR(critical2_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_CRIT3);

/* warning alert */
static SENSOR_DEVICE_ATTR(warning0_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_WARN1);
static SENSOR_DEVICE_ATTR(warning1_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_WARN2);
static SENSOR_DEVICE_ATTR(warning2_alarm, (S_IRUGO | S_IWUSR), ina3221_show_voltage, ina3221_set_voltage, INA3221_WARN3);

static struct attribute *ina3221_attrs[] = {
	/* channel 1 */
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_curr0_input.dev_attr.attr,
	&sensor_dev_attr_power0_input.dev_attr.attr,
	&sensor_dev_attr_shunt0_resistor.dev_attr.attr,
	&sensor_dev_attr_critical0_alarm.dev_attr.attr,
	&sensor_dev_attr_warning0_alarm.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,

	/* channel 2 */
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_power1_input.dev_attr.attr,
	&sensor_dev_attr_shunt1_resistor.dev_attr.attr,
	&sensor_dev_attr_critical1_alarm.dev_attr.attr,
	&sensor_dev_attr_warning1_alarm.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,

	/* channel 3 */
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_curr2_input.dev_attr.attr,
	&sensor_dev_attr_power2_input.dev_attr.attr,
	&sensor_dev_attr_shunt2_resistor.dev_attr.attr,
	&sensor_dev_attr_critical2_alarm.dev_attr.attr,
	&sensor_dev_attr_warning2_alarm.dev_attr.attr,
	&sensor_dev_attr_in5_input.dev_attr.attr,

	NULL,
};
ATTRIBUTE_GROUPS(ina3221);

static const struct regmap_range ina3221_yes_ranges[] = {
	regmap_reg_range(INA3221_SHUNT1, INA3221_BUS3),
	regmap_reg_range(INA3221_MASK_ENABLE, INA3221_MASK_ENABLE),
};

static const struct regmap_access_table ina3221_volatile_table = {
	.yes_ranges = ina3221_yes_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina3221_yes_ranges),
};

static const struct regmap_config ina3221_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,

	.cache_type = REGCACHE_RBTREE,
	.volatile_table = &ina3221_volatile_table,
};

static int ina3221_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct ina3221_data *ina;
	struct device *hwmon_dev;
	int i, ret;

	ina = devm_kzalloc(&client->dev, sizeof(*ina), GFP_KERNEL);
	if (!ina)
		return -ENOMEM;
	i2c_set_clientdata(client, ina);

	ina->dev = &client->dev;

	ina->regmap = devm_regmap_init_i2c(client, &ina3221_regmap_config);
	if (IS_ERR(ina->regmap)) {
		dev_err(ina->dev, "Unable to allocate register map\n");
		return PTR_ERR(ina->regmap);
	}

	for (i = 0; i < F_MAX_FIELDS; i++) {
		ina->fields[i] = devm_regmap_field_alloc(ina->dev,
							 ina->regmap,
							 ina3221_reg_fields[i]);
		if (IS_ERR(ina->fields[i])) {
			dev_err(ina->dev, "Unable to allocate regmap fields\n");
			return PTR_ERR(ina->fields[i]);
		}
	}

	ret = regmap_field_write(ina->fields[F_RST], true);
	if (ret) {
		dev_err(ina->dev, "Unable to reset device\n");
		return ret;
	}

	hwmon_dev = devm_hwmon_device_register_with_groups(ina->dev,
							   client->name,
							   ina, ina3221_groups);
	if (IS_ERR(hwmon_dev)) {
		dev_err(ina->dev, "Unable register hwmon device\n");
		return PTR_ERR(hwmon_dev);
	}

	return 0;
}

static const struct i2c_device_id ina3221_ids[] = {
	{ "ina3221", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ina3221_ids);

static struct i2c_driver ina3221_i2c_driver = {
	.driver = {
		.name = INA3221_DRIVER_NAME,
	},
	.probe = ina3221_probe,
	.id_table = ina3221_ids,
};
module_i2c_driver(ina3221_i2c_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("Texas Instruments INA3221 HWMon Driver");
MODULE_LICENSE("GPL v2");
