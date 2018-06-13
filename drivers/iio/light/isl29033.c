// SPDX-License-Identifier: GPL-2.0
/*
 * isl29033.c: A iio driver for the light sensor ISL 29033.
 *
 * IIO driver for monitoring ambient light intensity in lux and infrared
 * sensing.
 *
 * Copyright (c) 2018, Axis Communication AB
 * Author: Borys Movchan <Borys.Movchan@axis.com>
 */

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define ISL29033_REG_ADD_COMMAND1	0x00
#define ISL29033_CMD1_OPMODE_SHIFT	5
#define ISL29033_CMD1_OPMODE_MASK	(7 << ISL29033_CMD1_OPMODE_SHIFT)
#define ISL29033_CMD1_OPMODE_POWER_DOWN	0
#define ISL29033_CMD1_OPMODE_ALS_CONT	5
#define ISL29033_CMD1_OPMODE_IR_CONT	6

#define ISL29033_REG_ADD_COMMAND2	0x01
#define ISL29033_CMD2_RESOLUTION_SHIFT	2
#define ISL29033_CMD2_RESOLUTION_MASK	(0x3 << ISL29033_CMD2_RESOLUTION_SHIFT)

#define ISL29033_CMD2_RANGE_SHIFT	0
#define ISL29033_CMD2_RANGE_MASK	(0x3 << ISL29033_CMD2_RANGE_SHIFT)

#define ISL29033_CMD2_SCHEME_SHIFT	7
#define ISL29033_CMD2_SCHEME_MASK	(0x1 << ISL29033_CMD2_SCHEME_SHIFT)

#define ISL29033_REG_ADD_DATA_LSB	0x02
#define ISL29033_REG_ADD_DATA_MSB	0x03

#define ISL29033_REG_TEST		0x08
#define ISL29033_TEST_SHIFT		0
#define ISL29033_TEST_MASK		(0xFF << ISL29033_TEST_SHIFT)

#define ISL29033_REF_REXT		499	/* In kOhm */

#define ISL29033_POWER_OFF_DELAY_MS	5000

#define ISL29033_MICRO			1000000

#define isl29033_int_utime(adcmax) \
	((unsigned int)(adcmax) * (ISL29033_MICRO / 1000) / 655)

static const unsigned int isl29033_int_utimes[4] = {
	isl29033_int_utime(65536),
	isl29033_int_utime(4096),
	isl29033_int_utime(256),
	isl29033_int_utime(16)
};

#define isl29033_mkscale(range, adcmax)				\
	 { (range) / (adcmax),					\
	   ((unsigned int)(range) * (ISL29033_MICRO / 10)	\
			/ (adcmax) * 10) % ISL29033_MICRO }

static const struct isl29033_scale {
	unsigned int scale;
	unsigned int uscale;
} isl29033_scales[4][4] = {
	{
	   isl29033_mkscale(125, 65536), isl29033_mkscale(500, 65536),
	   isl29033_mkscale(2000, 65536), isl29033_mkscale(8000, 65536)
	},
	{
	   isl29033_mkscale(125, 4096), isl29033_mkscale(500, 4096),
	   isl29033_mkscale(2000, 4096), isl29033_mkscale(8000, 4096)
	},
	{
	   isl29033_mkscale(125, 256), isl29033_mkscale(500, 256),
	   isl29033_mkscale(2000, 256), isl29033_mkscale(8000, 256)
	},
	{
	   isl29033_mkscale(125, 16), isl29033_mkscale(500, 16),
	   isl29033_mkscale(2000, 16), isl29033_mkscale(8000, 16)
	}
};

struct isl29033_chip {
	struct regmap		*regmap;
	struct mutex		lock;
	unsigned int		int_time;
	unsigned int		calibscale;
	unsigned int		ucalibscale;
	struct isl29033_scale	scale;
	unsigned int		rext;
	unsigned int		opmode;
};

#define isl29033_rext_normalize(chip, val) \
	((val) * ISL29033_REF_REXT / (chip)->rext)

static unsigned int isl29033_rext_normalize2(struct isl29033_chip *chip,
				     unsigned int val, unsigned int val2)
{
	val = val - isl29033_rext_normalize(chip, val)
			* chip->rext / ISL29033_REF_REXT;
	val2 = (val2 + val * ISL29033_MICRO) * ISL29033_REF_REXT / chip->rext;
	return val2;
}

static int isl29033_set_integration_time(struct isl29033_chip *chip,
					 unsigned int utime)
{
	unsigned int i;
	int ret;
	unsigned int int_time, new_int_time;

	for (i = 0; i < ARRAY_SIZE(isl29033_int_utimes); ++i) {
		if (utime == isl29033_int_utimes[i]
				* chip->rext / ISL29033_REF_REXT) {
			new_int_time = i;
			break;
		}
	}

	if (i >= ARRAY_SIZE(isl29033_int_utimes))
		return -EINVAL;

	ret = regmap_update_bits(chip->regmap, ISL29033_REG_ADD_COMMAND2,
				 ISL29033_CMD2_RESOLUTION_MASK,
				 i << ISL29033_CMD2_RESOLUTION_SHIFT);
	if (ret < 0)
		return ret;

	/* Keep the same range when integration time changes */
	int_time = chip->int_time;
	for (i = 0; i < ARRAY_SIZE(isl29033_scales[int_time]); ++i) {
		if (chip->scale.scale == isl29033_scales[int_time][i].scale &&
		    chip->scale.uscale == isl29033_scales[int_time][i].uscale) {
			chip->scale = isl29033_scales[new_int_time][i];
			break;
		}
	}
	chip->int_time = new_int_time;

	return 0;
}

static int isl29033_set_scale(struct isl29033_chip *chip, int scale, int uscale)
{
	unsigned int i;
	int ret;
	struct isl29033_scale new_scale;

	for (i = 0; i < ARRAY_SIZE(isl29033_scales[chip->int_time]); ++i) {
		if (scale ==
			isl29033_rext_normalize(chip,
				isl29033_scales[chip->int_time][i].scale)
		    && uscale ==
			isl29033_rext_normalize2(chip,
				isl29033_scales[chip->int_time][i].scale,
				isl29033_scales[chip->int_time][i].uscale)) {
			new_scale = isl29033_scales[chip->int_time][i];
			break;
		}
	}

	if (i >= ARRAY_SIZE(isl29033_scales[chip->int_time]))
		return -EINVAL;

	ret = regmap_update_bits(chip->regmap, ISL29033_REG_ADD_COMMAND2,
				 ISL29033_CMD2_RANGE_MASK,
				 i << ISL29033_CMD2_RANGE_SHIFT);
	if (ret < 0)
		return ret;

	chip->scale = new_scale;

	return 0;
}

static int isl29033_set_mode(struct isl29033_chip *chip, int mode)
{

	int ret;
	unsigned int utime;

	if (chip->opmode == mode)
		return 0;

	ret = regmap_update_bits(chip->regmap, ISL29033_REG_ADD_COMMAND1,
				ISL29033_CMD1_OPMODE_MASK,
				mode << ISL29033_CMD1_OPMODE_SHIFT);
	if (ret < 0) {
		struct device *dev = regmap_get_device(chip->regmap);

		dev_err(dev,
			"Error in setting operating mode with err %d\n", ret);
		return ret;
	}

	utime = isl29033_int_utimes[chip->int_time] * chip->rext
			/ ISL29033_REF_REXT;

	/* Chip needs twice more time while switching between modes */
	if (chip->opmode != ISL29033_CMD1_OPMODE_POWER_DOWN)
		utime *= 2;

	if (utime < 20000)
		usleep_range(utime, utime * 2);
	else
		msleep(utime / 1000);

	chip->opmode = mode;
	return 0;
}

static int isl29033_read_sensor_input(struct isl29033_chip *chip)
{
	int ret;
	u16 val;
	struct device *dev = regmap_get_device(chip->regmap);

	ret = regmap_bulk_read(chip->regmap, ISL29033_REG_ADD_DATA_LSB,
				(u8 *)&val, 2);
	if (ret < 0) {
		dev_err(dev,
			"Data bulk read error %d\n", ret);
		return ret;
	}

	val = be16_to_cpu(val);
	dev_vdbg(dev, "Data read: %x\n", val);

	return val;
}

static int isl29033_read_lux(struct isl29033_chip *chip, int *lux, int *ulux)
{
	int ret;
	unsigned int tmpres;

	ret = isl29033_set_mode(chip, ISL29033_CMD1_OPMODE_ALS_CONT);
	if (ret)
		return ret;

	ret = isl29033_read_sensor_input(chip);
	if (ret < 0)
		return ret;

	ret++;

	tmpres = ret * isl29033_rext_normalize2(chip, chip->scale.scale,
					  chip->scale.uscale);
	*lux = ret * isl29033_rext_normalize(chip, chip->scale.scale)
				+ tmpres / ISL29033_MICRO;
	*ulux = tmpres % ISL29033_MICRO;

	*lux = *lux * chip->calibscale;
	*ulux = *ulux * chip->calibscale + *lux * chip->ucalibscale
				+ *ulux * chip->ucalibscale / ISL29033_MICRO;

	return IIO_VAL_INT_PLUS_MICRO;
}

static int isl29033_read_ir(struct isl29033_chip *chip, int *ir)
{
	int ret;

	ret = isl29033_set_mode(chip, ISL29033_CMD1_OPMODE_IR_CONT);
	if (ret)
		return ret;

	ret = isl29033_read_sensor_input(chip);
	if (ret < 0)
		return ret;

	*ir = ret;

	return IIO_VAL_INT;
}

static ssize_t isl29033_in_illuminance_scale_available
			(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct isl29033_chip *chip = iio_priv(indio_dev);
	unsigned int i;
	int len = 0;

	mutex_lock(&chip->lock);
	for (i = 0; i < ARRAY_SIZE(isl29033_scales[chip->int_time]); ++i)
		len += sprintf(buf + len, "%d.%06d ",
			isl29033_rext_normalize(chip,
				isl29033_scales[chip->int_time][i].scale),
			isl29033_rext_normalize2(chip,
				isl29033_scales[chip->int_time][i].scale,
				isl29033_scales[chip->int_time][i].uscale));
	mutex_unlock(&chip->lock);

	buf[len - 1] = '\n';

	return len;
}

static ssize_t isl29033_in_illuminance_integration_time_available
			(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct isl29033_chip *chip = iio_priv(indio_dev);
	unsigned int i;
	int len = 0;

	for (i = 0; i < ARRAY_SIZE(isl29033_int_utimes); ++i)
		len += sprintf(buf + len, "0.%06d ",
				   isl29033_int_utimes[i]
				   * chip->rext / ISL29033_REF_REXT);

	buf[len - 1] = '\n';

	return len;
}

static int isl29033_runtime_pm_get(struct isl29033_chip *chip)
{
	int ret;
	struct device *dev = regmap_get_device(chip->regmap);

	ret = pm_runtime_get(dev);
	if (ret < 0)
		pm_runtime_put_noidle(dev);

	return ret;
}

static void isl29033_runtime_pm_put(struct isl29033_chip *chip)
{
	struct device *dev = regmap_get_device(chip->regmap);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}


static int isl29033_write_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  int val,
				  int val2,
				  long mask)
{
	int ret;
	struct isl29033_chip *chip = iio_priv(indio_dev);

	ret = isl29033_runtime_pm_get(chip);
	if (ret < 0)
		return ret;

	ret = -EINVAL;

	mutex_lock(&chip->lock);
	switch (mask) {
	case IIO_CHAN_INFO_CALIBSCALE:
		if (chan->type == IIO_LIGHT) {
			chip->calibscale = val;
			chip->ucalibscale = val2;
			ret = 0;
		}
		break;
	case IIO_CHAN_INFO_INT_TIME:
		if (chan->type == IIO_LIGHT && !val)
			ret = isl29033_set_integration_time(chip, val2);
		break;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_LIGHT)
			ret = isl29033_set_scale(chip, val, val2);
		break;
	default:
		break;
	}

	mutex_unlock(&chip->lock);
	isl29033_runtime_pm_put(chip);

	return ret;
}

static int isl29033_read_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int *val,
				 int *val2,
				 long mask)
{
	int ret;
	struct isl29033_chip *chip = iio_priv(indio_dev);

	ret = isl29033_runtime_pm_get(chip);
	if (ret < 0)
		return ret;

	ret = -EINVAL;

	mutex_lock(&chip->lock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_INTENSITY)
			ret = isl29033_read_ir(chip, val);
		break;
	case IIO_CHAN_INFO_PROCESSED:
		if (chan->type == IIO_LIGHT)
			ret = isl29033_read_lux(chip, val, val2);
		break;
	case IIO_CHAN_INFO_INT_TIME:
		if (chan->type == IIO_LIGHT) {
			*val = 0;
			*val2 = isl29033_int_utimes[chip->int_time]
					* chip->rext / ISL29033_REF_REXT;
			ret = IIO_VAL_INT_PLUS_MICRO;
		}
		break;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_LIGHT) {
			*val = isl29033_rext_normalize(chip, chip->scale.scale);
			*val2 = isl29033_rext_normalize2(chip,
					chip->scale.scale, chip->scale.uscale);
			ret = IIO_VAL_INT_PLUS_MICRO;
		}
		break;
	case IIO_CHAN_INFO_CALIBSCALE:
		if (chan->type == IIO_LIGHT) {
			*val = chip->calibscale;
			*val2 = chip->ucalibscale;
			ret = IIO_VAL_INT_PLUS_MICRO;
		}
		break;
	default:
		break;
	}

	mutex_unlock(&chip->lock);
	isl29033_runtime_pm_put(chip);

	return ret;
}

#define ISL29033_LIGHT_CHANNEL {					\
	.type = IIO_LIGHT,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |		\
	BIT(IIO_CHAN_INFO_CALIBSCALE) |					\
	BIT(IIO_CHAN_INFO_SCALE) |					\
	BIT(IIO_CHAN_INFO_INT_TIME),					\
}

#define ISL29033_IR_CHANNEL {						\
	.type = IIO_INTENSITY,						\
	.modified = 1,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.channel2 = IIO_MOD_LIGHT_IR,					\
}

static const struct iio_chan_spec isl29033_channels[] = {
	ISL29033_LIGHT_CHANNEL,
	ISL29033_IR_CHANNEL,
};

static IIO_DEVICE_ATTR(in_illuminance_integration_time_available, 0444,
		isl29033_in_illuminance_integration_time_available, NULL, 0);
static IIO_DEVICE_ATTR(in_illuminance_scale_available, 0444,
		isl29033_in_illuminance_scale_available, NULL, 0);

#define ISL29033_DEV_ATTR(name) (&iio_dev_attr_##name.dev_attr.attr)

static struct attribute *isl29033_attributes[] = {
	ISL29033_DEV_ATTR(in_illuminance_scale_available),
	ISL29033_DEV_ATTR(in_illuminance_integration_time_available),
	NULL
};

static const struct attribute_group isl29033_group = {
	.attrs = isl29033_attributes,
};

static int isl29033_chip_init(struct isl29033_chip *chip)
{
	int ret;
	struct device *dev = regmap_get_device(chip->regmap);

	/*
	 * Code added per Intersil Application Note 1534:
	 *	 When VDD sinks to approximately 1.8V or below, some of
	 * the part's registers may change their state. When VDD
	 * recovers to 2.25V (or greater), the part may thus be in an
	 * unknown mode of operation. The user can return the part to
	 * a known mode of operation either by (a) setting VDD = 0V for
	 * 1 second or more and then powering back up with a slew rate
	 * of 0.5V/ms or greater, or (b) via I2C disable all ALS/PROX
	 * conversions, clear the test registers, and then rewrite all
	 * registers to the desired values.
	 * ...
	 * For ISL29033, etc.
	 * 1. Write 0x00 to register 0x08 (TEST)
	 * 2. Write 0x00 to register 0x00 (CMD1)
	 * 3. Rewrite all registers to the desired values
	 */
	ret = regmap_write(chip->regmap, ISL29033_REG_TEST, 0x0);
	if (ret < 0) {
		dev_err(dev, "Failed to clear isl29033 TEST reg with err %d\n",
			ret);
		return ret;
	}

	/*
	 * See Intersil AN1534 comments above.
	 * "Operating Mode" (COMMAND1) register is reprogrammed when
	 * data is read from the device.
	 */
	ret = regmap_write(chip->regmap, ISL29033_REG_ADD_COMMAND1, 0);
	if (ret < 0) {
		dev_err(dev, "Failed to clear isl29033 CMD1 reg with err %d\n",
			ret);
		return ret;
	}

	usleep_range(1000, 2000);	/* per data sheet, page 10 */

	/* Set defaults */
	ret = isl29033_set_scale(chip,
			isl29033_rext_normalize(chip, chip->scale.scale),
			isl29033_rext_normalize2(chip, chip->scale.scale,
					chip->scale.uscale));
	if (ret) {
		dev_err(dev,
			"Init of isl29033 fails (scale) with err %d\n", ret);
		return ret;
	}

	ret = isl29033_set_integration_time(chip,
			isl29033_int_utimes[chip->int_time]
			* chip->rext / ISL29033_REF_REXT);
	if (ret) {
		dev_err(dev,
			"Init of isl29033 fails (integration) with err %d\n",
			ret);
		return ret;
	}

	ret = isl29033_set_mode(chip, chip->opmode);
	if (ret)
		dev_err(dev,
			"Init of isl29033 fails (opmode) with err %d\n", ret);

	return ret;
}

static const struct iio_info isl29033_info = {
	.attrs = &isl29033_group,
	.read_raw = isl29033_read_raw,
	.write_raw = isl29033_write_raw,
};

static bool isl29033_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ISL29033_REG_ADD_DATA_LSB:
	case ISL29033_REG_ADD_DATA_MSB:
	case ISL29033_REG_ADD_COMMAND1:
	case ISL29033_REG_ADD_COMMAND2:
	case ISL29033_REG_TEST:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config isl29033_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_reg = isl29033_is_volatile_reg,
	.max_register = ISL29033_REG_TEST,
	.num_reg_defaults_raw = ISL29033_REG_TEST + 1,
	.cache_type = REGCACHE_RBTREE,
};

static const char *isl29033_match_acpi_device(struct device *dev, int *data)
{
	const struct acpi_device_id *id;

	id = acpi_match_device(dev->driver->acpi_match_table, dev);

	if (!id)
		return NULL;

	*data = (int)id->driver_data;

	return dev_name(dev);
}

static int isl29033_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct isl29033_chip *chip;
	struct iio_dev *indio_dev;
	int ret;
	const char *name = NULL;
	int dev_id = 0;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);

	i2c_set_clientdata(client, indio_dev);

	if (id) {
		name = id->name;
		dev_id = id->driver_data;
	}

	if (ACPI_HANDLE(&client->dev))
		name = isl29033_match_acpi_device(&client->dev, &dev_id);

	mutex_init(&chip->lock);

	chip->calibscale = 1;
	chip->ucalibscale = 0;
	chip->int_time = 0;
	chip->scale = isl29033_scales[chip->int_time][0];

#ifdef CONFIG_OF
	ret = device_property_read_u32(&client->dev, "isil,rext-kohms",
					&chip->rext);
	if (ret != 0)
		chip->rext =  ISL29033_REF_REXT;
#else
	chip->rext = ISL29033_REF_REXT;
#endif /* CONFIG_OF */

	chip->regmap = devm_regmap_init_i2c(client, &isl29033_regmap_config);

	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev,
			"regmap initialization fails with err %d\n", ret);
		return ret;
	}

	ret = isl29033_chip_init(chip);
	if (ret)
		return ret;

	indio_dev->info = &isl29033_info;
	indio_dev->channels = isl29033_channels;
	indio_dev->num_channels = ARRAY_SIZE(isl29033_channels);
	indio_dev->name = name;
	indio_dev->dev.parent = &client->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;

	pm_runtime_enable(&client->dev);
	pm_runtime_set_autosuspend_delay(&client->dev,
			ISL29033_POWER_OFF_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);


	return iio_device_register(indio_dev);
}

static int isl29033_suspend(struct device *dev)
{
	struct isl29033_chip *chip = iio_priv(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&chip->lock);

	ret = isl29033_set_mode(chip, ISL29033_CMD1_OPMODE_POWER_DOWN);

	mutex_unlock(&chip->lock);

	return ret;
}

static int __maybe_unused isl29033_resume(struct device *dev)
{
	struct isl29033_chip *chip = iio_priv(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&chip->lock);

	ret = isl29033_chip_init(chip);

	mutex_unlock(&chip->lock);

	return ret;
}

static int isl29033_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);

	isl29033_suspend(&client->dev);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	devm_iio_device_free(&client->dev, indio_dev);

	return 0;
}

static const struct dev_pm_ops isl29033_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
			pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(isl29033_suspend, isl29033_resume, NULL)
};

static const struct acpi_device_id isl29033_acpi_match[] = {
	{"ISL29033", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, isl29033_acpi_match);

static const struct i2c_device_id isl29033_id[] = {
	{"isl29033", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, isl29033_id);

static const struct of_device_id isl29033_of_match[] = {
	{ .compatible = "isil,isl29033", },
	{ },
};
MODULE_DEVICE_TABLE(of, isl29033_of_match);

static struct i2c_driver isl29033_driver = {
	.driver	 = {
		.name = "isl29033",
		.acpi_match_table = ACPI_PTR(isl29033_acpi_match),
		.pm = &isl29033_dev_pm_ops,
		.of_match_table = isl29033_of_match,
	},
	.probe	 = isl29033_probe,
	.remove  = isl29033_remove,
	.id_table = isl29033_id,
};
module_i2c_driver(isl29033_driver);

MODULE_DESCRIPTION("ISL29033 Ambient Light Sensor driver");
MODULE_LICENSE("GPL");
