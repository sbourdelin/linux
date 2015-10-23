/*
 * Driver for Texas Instruments INA219, INA226 power monitor chips
 *
 * INA219:
 * Zero Drift Bi-Directional Current/Power Monitor with I2C Interface
 * Datasheet: http://www.ti.com/product/ina219
 *
 * INA220:
 * Bi-Directional Current/Power Monitor with I2C Interface
 * Datasheet: http://www.ti.com/product/ina220
 *
 * INA226:
 * Bi-Directional Current/Power Monitor with I2C Interface
 * Datasheet: http://www.ti.com/product/ina226
 *
 * INA230:
 * Bi-directional Current/Power Monitor with I2C Interface
 * Datasheet: http://www.ti.com/product/ina230
 *
 * Copyright (C) 2012 Lothar Felten <l-felten@ti.com>
 * Thanks to Jan Volkering
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/util_macros.h>
#include <linux/regmap.h>

#include <linux/platform_data/ina2xx.h>

/* common register definitions */
#define INA2XX_CONFIG			0x00
#define INA2XX_SHUNT_VOLTAGE		0x01 /* readonly */
#define INA2XX_BUS_VOLTAGE		0x02 /* readonly */
#define INA2XX_POWER			0x03 /* readonly */
#define INA2XX_CURRENT			0x04 /* readonly */
#define INA2XX_CALIBRATION		0x05

/* CONFIG register fields */
#define INA2XX_AVG_MASK			0x0E00
#define INA2XX_AVG_SHFT			9

/* settings - depend on use case */
#define INA219_CONFIG_DEFAULT		0x399F	/* PGA=8 */
#define INA226_CONFIG_DEFAULT		0x4527	/* averages=16 */

/* worst case is 68.10 ms (~14.6Hz, ina219) */
#define INA2XX_MAX_DELAY		69 /* worst case delay in ms */

#define INA2XX_RSHUNT_DEFAULT		10000

/* Currently only handling common register set */
static const struct regmap_config INA2XX_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = INA2XX_CALIBRATION,
};

/* common attrs, ina226 attrs and NULL */
#define INA2XX_MAX_ATTRIBUTE_GROUPS	3

/*
 * Both bus voltage and shunt voltage conversion times for ina226 are set
 * to 0b0100 on POR, which translates to 2200 microseconds in total.
 */
#define INA226_TOTAL_CONV_TIME_DEFAULT	2200

enum ina2xx_ids { ina219, ina226 };

struct ina2xx_config {
	u16 config_default;
	int calibration_factor;
	int shunt_div;
	int bus_voltage_shift;
	int bus_voltage_lsb;	/* uV */
	int power_lsb;		/* uW */
};

struct ina2xx_data {
	struct i2c_client *client;
	const struct ina2xx_config *config;
	struct regmap *regmap;
	long rshunt;
	int valid;
	const struct attribute_group *groups[INA2XX_MAX_ATTRIBUTE_GROUPS];
};

static const struct ina2xx_config ina2xx_config[] = {
	[ina219] = {
		.config_default = INA219_CONFIG_DEFAULT,
		.calibration_factor = 40960000,
		.shunt_div = 100,
		.bus_voltage_shift = 3,
		.bus_voltage_lsb = 4000,
		.power_lsb = 20000,
	},
	[ina226] = {
		.config_default = INA226_CONFIG_DEFAULT,
		.calibration_factor = 5120000,
		.shunt_div = 400,
		.bus_voltage_shift = 0,
		.bus_voltage_lsb = 1250,
		.power_lsb = 25000,
	},
};

/*
 * Available averaging rates for ina226. The indices correspond with
 * the bit values expected by the chip (according to the ina226 datasheet,
 * table 3 AVG bit settings, found at
 * http://www.ti.com/lit/ds/symlink/ina226.pdf.
 */
static const int ina226_avg_tab[] = { 1, 4, 16, 64, 128, 256, 512, 1024 };

static int ina226_field_to_interval(int field)
{
	int avg = ina226_avg_tab[field];

	/*
	 * Multiply the total conversion time by the number of averages.
	 * Return the result in milliseconds.
	 */
	return DIV_ROUND_CLOSEST(avg * INA226_TOTAL_CONV_TIME_DEFAULT, 1000);
}

static int ina226_interval_to_field(int interval)
{
	int avg = DIV_ROUND_CLOSEST(interval * 1000,
				    INA226_TOTAL_CONV_TIME_DEFAULT);
	return find_closest(avg, ina226_avg_tab, ARRAY_SIZE(ina226_avg_tab));
}

static int ina2xx_calibrate(struct ina2xx_data *data)
{
	u16 val = DIV_ROUND_CLOSEST(data->config->calibration_factor,
				    data->rshunt);

	return regmap_write(data->regmap, INA2XX_CALIBRATION, val);
}

/*
 * Initialize the configuration and calibration registers.
 */
static int ina2xx_init(struct ina2xx_data *data)
{
	int ret = regmap_write(data->regmap, INA2XX_CONFIG,
			       data->config->config_default);
	if (ret)
		return ret;
	/*
	 * Set current LSB to 1mA, shunt is in uOhms
	 * (equation 13 in datasheet).
	 */
	return ina2xx_calibrate(data);
}

static int ina2xx_show_common(struct device *dev, int index, int *val)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int err, retry;

	if (unlikely(!val))
		return -EINVAL;

	for (retry = 5; retry; retry--) {

		/* Check for remaining registers in mask. */
		err = regmap_read(data->regmap, index, val);
		if (err)
			return err;

		dev_dbg(dev, "read %d, val = 0x%04x\n", index, *val);

		/*
		 * If the current value in the calibration register is 0, the
		 * power and current registers will also remain at 0. In case
		 * the chip has been reset let's check the calibration
		 * register and reinitialize if needed.
		 */
		if (!data->valid) {
			dev_warn(dev,
				 "chip needs calibration, reinitializing\n");

			err = ina2xx_calibrate(data);
			if (err)
				return err;
			/*
			 * Let's make sure the power and current registers
			 * have been updated before trying again.
			 */
			msleep(INA2XX_MAX_DELAY);

			/* data valid once we have a cal value. */
			regmap_read(data->regmap, INA2XX_CALIBRATION,
				    &data->valid);
			continue;
		}
		return 0;
	}

	/*
	 * If we're here then although all write operations succeeded, the
	 * chip still returns 0 in the calibration register. Nothing more we
	 * can do here.
	 */
	dev_err(dev, "unable to reinitialize the chip\n");
	return -ENODEV;
}

static ssize_t ina2xx_show_shunt(struct device *dev,
				 struct device_attribute *da, char *buf)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int val, err;

	err = ina2xx_show_common(dev, INA2XX_SHUNT_VOLTAGE, &val);
	if (err)
		return err;

	val = DIV_ROUND_CLOSEST((s16) val, data->config->shunt_div);
	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ina2xx_show_bus(struct device *dev,
			       struct device_attribute *da, char *buf)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int val, err;

	err = ina2xx_show_common(dev, INA2XX_BUS_VOLTAGE, &val);
	if (err)
		return err;

	val = (val >> data->config->bus_voltage_shift)
	    * data->config->bus_voltage_lsb;
	val = DIV_ROUND_CLOSEST(val, 1000);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ina2xx_show_pow(struct device *dev,
			       struct device_attribute *da, char *buf)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int val, err;

	err = ina2xx_show_common(dev, INA2XX_POWER, &val);
	if (err)
		return err;

	val *= data->config->power_lsb;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ina2xx_show_curr(struct device *dev,
				struct device_attribute *da, char *buf)
{
	int val, err;

	err = ina2xx_show_common(dev, INA2XX_CURRENT, &val);
	if (err)
		return err;

	/* signed register, LSB=1mA (selected), in mA */
	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ina2xx_show_cal(struct device *dev,
			       struct device_attribute *da, char *buf)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int val, err;

	err = ina2xx_show_common(dev, INA2XX_CALIBRATION, &val);
	if (err)
		return err;

	val = DIV_ROUND_CLOSEST(data->config->calibration_factor, val);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t ina2xx_set_shunt(struct device *dev,
				struct device_attribute *da,
				const char *buf, size_t count)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);

	unsigned long val;
	int status;

	status = kstrtoul(buf, 10, &val);
	if (status < 0)
		return status;

	if (val == 0 ||
	    /* Values greater than the calibration factor make no sense. */
	    val > data->config->calibration_factor)
		return -EINVAL;

	data->rshunt = val;
	data->valid = 0;

	return count;
}

static ssize_t ina226_set_interval(struct device *dev,
				   struct device_attribute *da,
				   const char *buf, size_t count)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int status;

	status = kstrtoul(buf, 10, &val);
	if (status < 0)
		return status;

	if (val > INT_MAX || val == 0)
		return -EINVAL;

	val = ina226_interval_to_field(val);

	status = regmap_update_bits(data->regmap, INA2XX_CONFIG,
				    INA2XX_AVG_MASK, (val << INA2XX_AVG_SHFT));
	if (status < 0)
		return status;

	data->valid = 0;

	return count;
}

static ssize_t ina226_show_interval(struct device *dev,
				    struct device_attribute *da, char *buf)
{
	struct ina2xx_data *data = dev_get_drvdata(dev);
	int status, val;

	status = regmap_read(data->regmap, INA2XX_CONFIG, &val);
	if (status)
		return status;

	val = (val & INA2XX_AVG_MASK) >> INA2XX_AVG_SHFT;
	val = ina226_field_to_interval(val);

	/*
	 * We want to display the actual interval used by the chip.
	 */
	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

/* shunt voltage */
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ina2xx_show_shunt, NULL,
			  INA2XX_SHUNT_VOLTAGE);

/* bus voltage */
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, ina2xx_show_bus, NULL,
			  INA2XX_BUS_VOLTAGE);

/* calculated current */
static SENSOR_DEVICE_ATTR(curr1_input, S_IRUGO, ina2xx_show_curr, NULL,
			  INA2XX_CURRENT);

/* calculated power */
static SENSOR_DEVICE_ATTR(power1_input, S_IRUGO, ina2xx_show_pow, NULL,
			  INA2XX_POWER);

/* shunt resistance */
static SENSOR_DEVICE_ATTR(shunt_resistor, S_IRUGO | S_IWUSR,
			  ina2xx_show_cal, ina2xx_set_shunt,
			  INA2XX_CALIBRATION);

/* pointers to created device attributes */
static struct attribute *ina2xx_attrs[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_power1_input.dev_attr.attr,
	&sensor_dev_attr_shunt_resistor.dev_attr.attr,
	NULL,
};

static const struct attribute_group ina2xx_group = {
	.attrs = ina2xx_attrs,
};

/* update interval (ina226 only) */
static SENSOR_DEVICE_ATTR(update_interval, S_IRUGO | S_IWUSR,
			  ina226_show_interval, ina226_set_interval, 0);

static struct attribute *ina226_attrs[] = {
	&sensor_dev_attr_update_interval.dev_attr.attr,
	NULL,
};

static const struct attribute_group ina226_group = {
	.attrs = ina226_attrs,
};


static int ina2xx_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ina2xx_platform_data *pdata;
	struct device *dev = &client->dev;
	struct ina2xx_data *data;
	struct device *hwmon_dev;
	struct regmap *regmap;
	u32 val;
	int ret, group = 0;

	/* Register regmap */
	regmap = devm_regmap_init_i2c(client, &INA2XX_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "failed to allocate register map\n");
		return PTR_ERR(regmap);
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = regmap;

	/* Set config according to device type. */
	data->config = &ina2xx_config[id->driver_data];
	data->client = client;

	/* Check for shunt resistor value.
	 * Give precedence to device tree over must-recompile.
	 */
	if (of_property_read_u32(dev->of_node, "shunt-resistor", &val) < 0) {
		pdata = dev_get_platdata(dev);
		if (pdata)
			val = pdata->shunt_uohms;
		else
			val = INA2XX_RSHUNT_DEFAULT;
	}

	if (val <= 0 || val > data->config->calibration_factor) {
		dev_err(dev, "Invalid shunt resistor value %li", val);
		return -ENODEV;
	}
	data->rshunt = val;

	/* Write config to chip, and calibrate */
	ret = ina2xx_init(data);
	if (ret) {
		dev_err(dev, "error configuring the device.\n");
		return ret;
	}

	/* Set sensor group according to device type. */
	data->groups[group++] = &ina2xx_group;
	if (ina226 == id->driver_data)
		data->groups[group++] = &ina226_group;

	/* register to hwmon */
	hwmon_dev = devm_hwmon_device_register_with_groups(dev, client->name,
							   data, data->groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "power monitor %s (Rshunt = %li uOhm)\n",
		 id->name, data->rshunt);

	return 0;
}

static const struct i2c_device_id ina2xx_id[] = {
	{ "ina219", ina219 },
	{ "ina220", ina219 },
	{ "ina226", ina226 },
	{ "ina230", ina226 },
	{ "ina231", ina226 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ina2xx_id);

static struct i2c_driver ina2xx_driver = {
	.driver = {
		.name	= "ina2xx",
	},
	.probe		= ina2xx_probe,
	.id_table	= ina2xx_id,
};

module_i2c_driver(ina2xx_driver);
MODULE_AUTHOR("Lothar Felten <l-felten@ti.com>");
MODULE_DESCRIPTION("ina2xx driver");
MODULE_LICENSE("GPL");
