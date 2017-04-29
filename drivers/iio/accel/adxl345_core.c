/*
 * ADXL345 3-Axis Digital Accelerometer IIO core driver
 *
 * Copyright (c) 2017 Eva Rachel Retuya <eraretuya@gmail.com>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License. See the file COPYING in the main
 * directory of this archive for more details.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>

#include "adxl345.h"

#define ADXL345_REG_DEVID		0x00
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_REG_INT_ENABLE		0x2E
#define ADXL345_REG_INT_MAP		0x2F
#define ADXL345_REG_INT_SOURCE		0x30
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_REG_DATAX0		0x32
#define ADXL345_REG_DATAY0		0x34
#define ADXL345_REG_DATAZ0		0x36

#define ADXL345_POWER_CTL_MEASURE	BIT(3)
#define ADXL345_POWER_CTL_STANDBY	0x00

/* INT_ENABLE/INT_MAP/INT_SOURCE bits */
#define ADXL345_INT_DATA_READY		BIT(7)
#define ADXL345_INT_OVERRUN		0

#define ADXL345_DATA_FORMAT_FULL_RES	BIT(3) /* Up to 13-bits resolution */
#define ADXL345_DATA_FORMAT_2G		0
#define ADXL345_DATA_FORMAT_4G		1
#define ADXL345_DATA_FORMAT_8G		2
#define ADXL345_DATA_FORMAT_16G		3

#define ADXL345_DEVID			0xE5

#define ADXL345_IRQ_NAME		"adxl345_event"

/*
 * In full-resolution mode, scale factor is maintained at ~4 mg/LSB
 * in all g ranges.
 *
 * At +/- 16g with 13-bit resolution, scale is computed as:
 * (16 + 16) * 9.81 / (2^13 - 1) = 0.0383
 */
static const int adxl345_uscale = 38300;

struct adxl345_data {
	struct iio_trigger *data_ready_trig;
	bool data_ready_trig_on;
	struct regmap *regmap;
	struct mutex lock; /* protect this data structure */
	u8 data_range;
};

static int adxl345_set_mode(struct adxl345_data *data, u8 mode)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;

	ret = regmap_write(data->regmap, ADXL345_REG_POWER_CTL, mode);
	if (ret < 0) {
		dev_err(dev, "Failed to set power mode, %d\n", ret);
		return ret;
	}

	return 0;
}

static int adxl345_data_ready(struct adxl345_data *data)
{
	struct device *dev = regmap_get_device(data->regmap);
	int tries = 5;
	u32 val;
	int ret;

	do {
		/*
		 * 1/ODR + 1.1ms; 11.1ms at ODR of 0.10 Hz
		 * Sensor currently operates at default ODR of 100 Hz
		 */
		usleep_range(1100, 11100);

		ret = regmap_read(data->regmap, ADXL345_REG_INT_SOURCE, &val);
		if (ret < 0)
			return ret;
		if ((val & ADXL345_INT_DATA_READY) == ADXL345_INT_DATA_READY)
			return 0;
	} while (--tries);
	dev_err(dev, "Data is not yet ready, try again.\n");

	return -EAGAIN;
}

#define ADXL345_CHANNEL(reg, axis) {					\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.address = reg,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec adxl345_channels[] = {
	ADXL345_CHANNEL(ADXL345_REG_DATAX0, X),
	ADXL345_CHANNEL(ADXL345_REG_DATAY0, Y),
	ADXL345_CHANNEL(ADXL345_REG_DATAZ0, Z),
};

static int adxl345_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct adxl345_data *data = iio_priv(indio_dev);
	__le16 regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		ret = adxl345_set_mode(data, ADXL345_POWER_CTL_MEASURE);
		if (ret < 0) {
			mutex_unlock(&data->lock);
			return ret;
		}

		ret = adxl345_data_ready(data);
		if (ret < 0) {
			adxl345_set_mode(data, ADXL345_POWER_CTL_STANDBY);
			mutex_unlock(&data->lock);
			return ret;
		}
		/*
		 * Data is stored in adjacent registers:
		 * ADXL345_REG_DATA(X0/Y0/Z0) contain the least significant byte
		 * and ADXL345_REG_DATA(X0/Y0/Z0) + 1 the most significant byte
		 */
		ret = regmap_bulk_read(data->regmap, chan->address, &regval,
				       sizeof(regval));
		mutex_unlock(&data->lock);
		if (ret < 0) {
			adxl345_set_mode(data, ADXL345_POWER_CTL_STANDBY);
			return ret;
		}

		*val = sign_extend32(le16_to_cpu(regval), 12);
		adxl345_set_mode(data, ADXL345_POWER_CTL_STANDBY);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = adxl345_uscale;

		return IIO_VAL_INT_PLUS_MICRO;
	}

	return -EINVAL;
}

static irqreturn_t adxl345_irq(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct adxl345_data *data = iio_priv(indio_dev);
	int ret;
	u32 int_stat;

	ret = regmap_read(data->regmap, ADXL345_REG_INT_SOURCE, &int_stat);
	if (ret < 0)
		return IRQ_HANDLED;

	if (int_stat & ADXL345_INT_DATA_READY) {
		iio_trigger_poll_chained(data->data_ready_trig);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int adxl345_drdy_trigger_set_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct adxl345_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->regmap);
	int ret;

	ret = regmap_update_bits(data->regmap,
				 ADXL345_REG_INT_ENABLE,
				 ADXL345_INT_DATA_READY,
				 state ? ADXL345_INT_DATA_READY : 0);
	if (ret < 0) {
		dev_err(dev, "Failed to update INT_ENABLE bits\n");
		return ret;
	}
	data->data_ready_trig_on = state;

	return ret;
}

static const struct iio_trigger_ops adxl345_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = adxl345_drdy_trigger_set_state,
};

static const struct iio_info adxl345_info = {
	.driver_module	= THIS_MODULE,
	.read_raw	= adxl345_read_raw,
};

int adxl345_core_probe(struct device *dev, struct regmap *regmap, int irq,
		       const char *name)
{
	struct adxl345_data *data;
	struct iio_dev *indio_dev;
	u32 regval;
	int of_irq;
	int ret;

	ret = regmap_read(regmap, ADXL345_REG_DEVID, &regval);
	if (ret < 0) {
		dev_err(dev, "Error reading device ID: %d\n", ret);
		return ret;
	}

	if (regval != ADXL345_DEVID) {
		dev_err(dev, "Invalid device ID: %x, expected %x\n",
			regval, ADXL345_DEVID);
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	dev_set_drvdata(dev, indio_dev);
	data->regmap = regmap;
	/* Enable full-resolution mode */
	data->data_range = ADXL345_DATA_FORMAT_FULL_RES;

	ret = regmap_write(data->regmap, ADXL345_REG_DATA_FORMAT,
			   data->data_range);
	if (ret < 0) {
		dev_err(dev, "Failed to set data range: %d\n", ret);
		return ret;
	}
	/*
	 * Any bits set to 0 send their respective interrupts to the INT1 pin,
	 * whereas bits set to 1 send their respective interrupts to the INT2
	 * pin. Map all interrupts to the specified pin.
	 */
	of_irq = of_irq_get_byname(dev->of_node, "INT2");
	if (of_irq == irq)
		regval = 0xFF;
	else
		regval = 0x00;

	ret = regmap_write(data->regmap, ADXL345_REG_INT_MAP, regval);
	if (ret < 0) {
		dev_err(dev, "Failed to set up interrupts: %d\n", ret);
		return ret;
	}

	mutex_init(&data->lock);

	indio_dev->dev.parent = dev;
	indio_dev->name = name;
	indio_dev->info = &adxl345_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adxl345_channels;
	indio_dev->num_channels = ARRAY_SIZE(adxl345_channels);

	if (irq > 0) {
		ret = devm_request_threaded_irq(dev,
						irq,
						NULL,
						adxl345_irq,
						IRQF_TRIGGER_HIGH |
						IRQF_ONESHOT,
						ADXL345_IRQ_NAME,
						indio_dev);
		if (ret < 0) {
			dev_err(dev, "Failed to request irq: %d\n", irq);
			return ret;
		}

		data->data_ready_trig = devm_iio_trigger_alloc(dev,
							       "%s-dev%d",
							       indio_dev->name,
							       indio_dev->id);
		if (!data->data_ready_trig)
			return -ENOMEM;

		data->data_ready_trig->dev.parent = dev;
		data->data_ready_trig->ops = &adxl345_trigger_ops;
		iio_trigger_set_drvdata(data->data_ready_trig, indio_dev);

		ret = devm_iio_trigger_register(dev, data->data_ready_trig);
		if (ret) {
			dev_err(dev, "Failed to register trigger: %d\n", ret);
			return ret;
		}
	}

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		dev_err(dev, "iio_device_register failed: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(adxl345_core_probe);

int adxl345_core_remove(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct adxl345_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	return adxl345_set_mode(data, ADXL345_POWER_CTL_STANDBY);
}
EXPORT_SYMBOL_GPL(adxl345_core_remove);

MODULE_AUTHOR("Eva Rachel Retuya <eraretuya@gmail.com>");
MODULE_DESCRIPTION("ADXL345 3-Axis Digital Accelerometer core driver");
MODULE_LICENSE("GPL v2");
