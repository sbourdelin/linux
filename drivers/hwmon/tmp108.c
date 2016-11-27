/* Texas Instruments TMP108 SMBus temperature sensor driver
 *
 * Copyright (C) 2016 John Muir <john@jmuir.com>
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
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/of.h>

#define	DRIVER_NAME "tmp108"

#define	TMP108_REG_TEMP		0x00
#define	TMP108_REG_CONF		0x01
#define	TMP108_REG_TLOW		0x02
#define	TMP108_REG_THIGH	0x03

#define TMP108_TEMP_REG_COUNT	3

#define TMP108_TEMP_MIN_MC	-50000 /* Minimum millicelcius. */
#define TMP108_TEMP_MAX_MC	127937 /* Maximum millicelcius. */

/* Configuration register bits.
 * Note: these bit definitions are byte swapped.
 */
#define TMP108_CONF_M0		0x0100 /* Sensor mode. */
#define TMP108_CONF_M1		0x0200
#define TMP108_CONF_TM		0x0400 /* Thermostat mode. */
#define TMP108_CONF_FL		0x0800 /* Watchdog flag - TLOW */
#define TMP108_CONF_FH		0x1000 /* Watchdog flag - THIGH */
#define TMP108_CONF_CR0		0x2000 /* Conversion rate. */
#define TMP108_CONF_CR1		0x4000
#define TMP108_CONF_ID		0x8000
#define TMP108_CONF_HYS0	0x0010 /* Hysteresis. */
#define TMP108_CONF_HYS1	0x0020
#define TMP108_CONF_POL		0x0080 /* Polarity of alert. */

/* Defaults set by the hardware upon reset. */
#define TMP108_CONF_DEFAULTS		(TMP108_CONF_CR0 | TMP108_CONF_TM |\
					 TMP108_CONF_HYS0 | TMP108_CONF_M1)
/* These bits are read-only. */
#define TMP108_CONF_READ_ONLY		(TMP108_CONF_FL | TMP108_CONF_FH |\
					 TMP108_CONF_ID)

#define TMP108_CONF_MODE_MASK		(TMP108_CONF_M0|TMP108_CONF_M1)
#define TMP108_MODE_SHUTDOWN		0x0000
#define TMP108_MODE_ONE_SHOT		TMP108_CONF_M0
#define TMP108_MODE_CONTINUOUS		TMP108_CONF_M1		/* Default */

#define TMP108_CONF_CONVRATE_MASK	(TMP108_CONF_CR0|TMP108_CONF_CR1)
#define TMP108_CONVRATE_0P25HZ		0x0000
#define TMP108_CONVRATE_1HZ		TMP108_CONF_CR0		/* Default */
#define TMP108_CONVRATE_4HZ		TMP108_CONF_CR1
#define TMP108_CONVRATE_16HZ		(TMP108_CONF_CR0|TMP108_CONF_CR1)

#define TMP108_CONF_HYSTERESIS_MASK	(TMP108_CONF_HYS0|TMP108_CONF_HYS1)
#define TMP108_HYSTERESIS_0C		0x0000
#define TMP108_HYSTERESIS_1C		TMP108_CONF_HYS0	/* Default */
#define TMP108_HYSTERESIS_2C		TMP108_CONF_HYS1
#define TMP108_HYSTERESIS_4C		(TMP108_CONF_HYS0|TMP108_CONF_HYS1)

#define TMP108_CONVERSION_TIME_MS	30	/* in milli-seconds */

struct tmp108 {
	struct regmap *regmap;
	u16 config;
	unsigned long ready_time;
};

static const u8 tmp108_temp_reg[TMP108_TEMP_REG_COUNT] = {
	TMP108_REG_TEMP,
	TMP108_REG_TLOW,
	TMP108_REG_THIGH,
};

/* convert 12-bit TMP108 register value to milliCelsius */
static inline int tmp108_temp_reg_to_mC(s16 val)
{
	return (val & ~0x01) * 1000 / 256;
}

/* convert milliCelsius to left adjusted 12-bit TMP108 register value */
static inline u16 tmp108_mC_to_temp_reg(int val)
{
	return (val * 256) / 1000;
}

static int tmp108_read_reg_temp(struct device *dev, int reg, int *temp)
{
	struct tmp108 *tmp108 = dev_get_drvdata(dev);
	unsigned int regval;
	int err;

	switch (reg) {
	case TMP108_REG_TEMP:
		/* Is it too early to return a conversion ? */
		if (time_before(jiffies, tmp108->ready_time)) {
			dev_dbg(dev, "%s: Conversion not ready yet..\n",
				__func__);
			return -EAGAIN;
		}
		break;
	case TMP108_REG_TLOW:
	case TMP108_REG_THIGH:
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = regmap_read(tmp108->regmap, reg, &regval);
	if (err < 0)
		return err;
	*temp = tmp108_temp_reg_to_mC(regval);

	return 0;
}

static ssize_t tmp108_show_temp(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	int temp;
	int err;

	if (sda->index >= ARRAY_SIZE(tmp108_temp_reg))
		return -EINVAL;

	err = tmp108_read_reg_temp(dev, tmp108_temp_reg[sda->index], &temp);
	if (err)
		return err;

	return snprintf(buf, PAGE_SIZE, "%d\n", temp);
}

static ssize_t tmp108_set_temp(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	struct tmp108 *tmp108 = dev_get_drvdata(dev);
	long temp;
	int err;

	if (sda->index >= ARRAY_SIZE(tmp108_temp_reg))
		return -EINVAL;

	if (kstrtol(buf, 10, &temp) < 0)
		return -EINVAL;

	temp = clamp_val(temp, TMP108_TEMP_MIN_MC, TMP108_TEMP_MAX_MC);
	err = regmap_write(tmp108->regmap, tmp108_temp_reg[sda->index],
			   tmp108_mC_to_temp_reg(temp));
	if (err)
		return err;
	return count;
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, tmp108_show_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(temp1_min, S_IWUSR | S_IRUGO, tmp108_show_temp,
			  tmp108_set_temp, 1);
static SENSOR_DEVICE_ATTR(temp1_max, S_IWUSR | S_IRUGO, tmp108_show_temp,
			  tmp108_set_temp, 2);

static struct attribute *tmp108_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_min.dev_attr.attr,
	&sensor_dev_attr_temp1_max.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(tmp108);

static int tmp108_get_temp(void *dev, int *temp)
{
	return tmp108_read_reg_temp(dev, TMP108_REG_TEMP, temp);
}

static const struct thermal_zone_of_device_ops tmp108_of_thermal_ops = {
	.get_temp = tmp108_get_temp,
};

static void tmp108_update_ready_time(struct tmp108 *tmp108)
{
	tmp108->ready_time = jiffies;
	if ((tmp108->config & TMP108_CONF_MODE_MASK)
	    == TMP108_MODE_CONTINUOUS) {
		tmp108->ready_time +=
			msecs_to_jiffies(TMP108_CONVERSION_TIME_MS);
	}
}

static void tmp108_restore_config(void *data)
{
	struct tmp108 *tmp108 = data;

	regmap_write(tmp108->regmap, TMP108_REG_CONF, tmp108->config);
	tmp108_update_ready_time(tmp108);
}

static bool tmp108_is_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg != TMP108_REG_TEMP;
}

static bool tmp108_is_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == TMP108_REG_TEMP;
}

static const struct regmap_config tmp108_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = TMP108_REG_THIGH,
	.writeable_reg = tmp108_is_writeable_reg,
	.volatile_reg = tmp108_is_volatile_reg,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_RBTREE,
	.use_single_rw = true,
};

static int tmp108_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct thermal_zone_device *tz;
	struct tmp108 *tmp108;
	unsigned int regval;
	int err;
	u16 config;
	u32 convrate = 100;
	u32 hysteresis = 1;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(dev,
			"adapter doesn't support SMBus word transactions\n");
		return -ENODEV;
	}

	tmp108 = devm_kzalloc(dev, sizeof(*tmp108), GFP_KERNEL);
	if (!tmp108)
		return -ENOMEM;

	i2c_set_clientdata(client, tmp108);

	tmp108->regmap = devm_regmap_init_i2c(client, &tmp108_regmap_config);
	if (IS_ERR(tmp108->regmap))
		return PTR_ERR(tmp108->regmap);

	err = regmap_read(tmp108->regmap, TMP108_REG_CONF, &regval);
	if (err < 0) {
		dev_err(dev, "error reading config register\n");
		return err;
	}
	tmp108->config = regval;
	config = regval;

	/* At this time, only continuous mode is supported. */
	config &= ~TMP108_CONF_MODE_MASK;
	config |= TMP108_MODE_CONTINUOUS;

	if (device_property_read_bool(dev, "ti,thermostat-mode-comparitor"))
		config &= ~TMP108_CONF_TM;
	else
		config |= TMP108_CONF_TM;

	if (device_property_read_bool(dev, "ti,alert-active-high"))
		config |= TMP108_CONF_POL;
	else
		config &= ~TMP108_CONF_POL;

	if (device_property_read_u32(dev, "ti,conversion-rate-cHz", &convrate)
	    >= 0) {
		config &= ~TMP108_CONF_CONVRATE_MASK;
		switch (convrate) {
		case 25:
			config |= TMP108_CONVRATE_0P25HZ;
			break;
		case 100:
			config |= TMP108_CONVRATE_1HZ;
			break;
		case 400:
			config |= TMP108_CONVRATE_4HZ;
			break;
		case 1600:
			config |= TMP108_CONVRATE_16HZ;
			break;
		default:
			dev_err(dev, "conversion rate %u invalid: defaulting to 1Hz.\n",
				convrate);
			convrate = 100;
			config |= TMP108_CONVRATE_1HZ;
			break;
		}
	}

	if (device_property_read_u32(dev, "ti,hysteresis", &hysteresis) >= 0) {
		config &= ~TMP108_CONF_HYSTERESIS_MASK;
		switch (hysteresis) {
		case 0:
			config |= TMP108_HYSTERESIS_0C;
			break;
		case 1:
			config |= TMP108_HYSTERESIS_1C;
			break;
		case 2:
			config |= TMP108_HYSTERESIS_2C;
			break;
		case 4:
			config |= TMP108_HYSTERESIS_4C;
			break;
		default:
			dev_err(dev, "hysteresis value %u invalid: defaulting to 1C.\n",
				hysteresis);
			hysteresis = 1;
			config |= TMP108_HYSTERESIS_1C;
			break;
		}
	}

	err = regmap_write(tmp108->regmap, TMP108_REG_CONF, config);
	if (err < 0) {
		dev_err(dev, "error writing config register\n");
		return err;
	}

	tmp108->config = config;
	tmp108_update_ready_time(tmp108);

	err = devm_add_action_or_reset(dev, tmp108_restore_config, tmp108);
	if (err)
		return err;

	hwmon_dev = devm_hwmon_device_register_with_groups(dev, client->name,
							   tmp108,
							   tmp108_groups);
	if (IS_ERR(hwmon_dev)) {
		dev_dbg(dev, "unable to register hwmon device\n");
		return PTR_ERR(hwmon_dev);
	}

	tz = devm_thermal_zone_of_sensor_register(hwmon_dev, 0, hwmon_dev,
						  &tmp108_of_thermal_ops);
	if (IS_ERR(tz))
		return PTR_ERR(tz);

	dev_info(dev, "%s, alert: active %s, hyst: %uC, conv: %ucHz\n",
		 (tmp108->config & TMP108_CONF_TM) != 0 ?
			"interrupt" : "comparator",
		 (tmp108->config & TMP108_CONF_POL) != 0 ? "high" : "low",
		 hysteresis, convrate);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int tmp108_suspend(struct device *dev)
{
	struct tmp108 *tmp108 = dev_get_drvdata(dev);

	return regmap_update_bits(tmp108->regmap, TMP108_REG_CONF,
				  TMP108_CONF_MODE_MASK, TMP108_MODE_SHUTDOWN);
}

static int tmp108_resume(struct device *dev)
{
	struct tmp108 *tmp108 = dev_get_drvdata(dev);
	int err;

	err = regmap_write(tmp108->regmap, TMP108_REG_CONF, tmp108->config);

	tmp108_update_ready_time(tmp108);

	return err;
}
#endif /* CONFIG_PM */

static SIMPLE_DEV_PM_OPS(tmp108_dev_pm_ops, tmp108_suspend, tmp108_resume);

static const struct i2c_device_id tmp108_id[] = {
	{ "tmp108", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tmp108_id);

static struct i2c_driver tmp108_driver = {
	.driver.name	= DRIVER_NAME,
	.driver.pm	= &tmp108_dev_pm_ops,
	.probe		= tmp108_probe,
	.id_table	= tmp108_id,
};

module_i2c_driver(tmp108_driver);

MODULE_AUTHOR("John Muir <john@jmuir.com>");
MODULE_DESCRIPTION("Texas Instruments TMP108 temperature sensor driver");
MODULE_LICENSE("GPL");
