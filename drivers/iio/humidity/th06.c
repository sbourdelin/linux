/*
 * TH06 - I2C Humidity and Temperature Sensor
 *
 * Copyright (C) 2016 Cristina Moraru <cristina.moraru09@gmail.com>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * IIO driver for TH06 (7-bit I2C slave address 0x40)
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define TH06_DRV_NAME		"th06"

#define TH06_READ_RH		0xE5
#define TH06_READ_TEMP		0xE3

struct th06_data {
	struct i2c_client *client;
};

static const struct iio_chan_spec th06_channels[] = {
	{
		.type	= IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					BIT(IIO_CHAN_INFO_SCALE) |
					BIT(IIO_CHAN_INFO_OFFSET),
	},
	{
		.type	= IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					BIT(IIO_CHAN_INFO_SCALE) |
					BIT(IIO_CHAN_INFO_OFFSET),
	}
};

static int th06_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct th06_data *data = iio_priv(indio_dev);
	int ret = -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_HUMIDITYRELATIVE:
			ret = i2c_smbus_read_word_swapped(data->client,
							TH06_READ_RH);
			break;
		case IIO_TEMP:
			ret = i2c_smbus_read_word_swapped(data->client,
							TH06_READ_TEMP);
			break;
		default:
			break;
		}
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_HUMIDITYRELATIVE:
			*val = 0;
			*val2 = 1907349;
			break;
		case IIO_TEMP:
			*val = 0;
			*val2 = 2681274;
			break;
		default:
			break;
		}
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		switch (chan->type) {
		case IIO_HUMIDITYRELATIVE:
			*val = -6;
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = -46;
			*val2 = 850000;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			break;
		}
	}
	return ret;
}


static const struct iio_info th06_info = {
	.driver_module	= THIS_MODULE,
	.read_raw	= th06_read_raw,
};

static int th06_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct th06_data *data;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &th06_info;
	indio_dev->name = TH06_DRV_NAME;
	indio_dev->channels = th06_channels;
	indio_dev->num_channels = ARRAY_SIZE(th06_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id th06_id[] = {
	{"th06", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, th06_id);

static struct i2c_driver th06_driver = {
	.driver = {
		.name = TH06_DRV_NAME,
	},
	.probe		= th06_probe,
	.id_table	= th06_id,
};

module_i2c_driver(th06_driver);

MODULE_AUTHOR("Cristina Moraru <cristina.moraru09@gmail.com>");
MODULE_DESCRIPTION("TH06 Humidity and Temperature Sensor");
MODULE_LICENSE("GPL");
