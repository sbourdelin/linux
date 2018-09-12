// SPDX-License-Identifier: GPL-2.0+
/*
 *  Support for ST's VL53L0X FlightSense ToF Ranger Sensor on a i2c bus.
 *
 *  Copyright (C) 2016 STMicroelectronics Imaging Division.
 *  Copyright (C) 2018 Song Qiang <songqiang.1304521@gmail.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#define VL53L0X_DRV_NAME				"vl53l0x-i2c"

#define VL_REG_SYSRANGE_MODE_MASK			GENMASK(3, 0)
#define VL_REG_SYSRANGE_START				0x00
#define VL_REG_SYSRANGE_MODE_SINGLESHOT			0x00
#define VL_REG_SYSRANGE_MODE_START_STOP			BIT(0)
#define VL_REG_SYSRANGE_MODE_BACKTOBACK			BIT(1)
#define VL_REG_SYSRANGE_MODE_TIMED			BIT(2)
#define VL_REG_SYSRANGE_MODE_HISTOGRAM			BIT(3)

#define VL_REG_SYS_SEQUENCE_CFG				BIT(0)
#define VL_REG_SYS_INTERMEASUREMENT_PERIOD		BIT(2)
#define VL_REG_SYS_RANGE_CFG				0x09

#define VL_REG_SYS_INT_GPIO_DISABLED			0x00
#define VL_REG_SYS_INT_GPIO_LEVEL_LOW			BIT(0)
#define VL_REG_SYS_INT_GPIO_LEVEL_HIGH			BIT(1)
#define VL_REG_SYS_INT_GPIO_OUT_OF_WINDOW		0x03
#define VL_REG_SYS_INT_GPIO_NEW_SAMPLE_READY		BIT(2)
#define VL_REG_SYS_INT_CFG_GPIO				0x0A
#define VL_REG_SYS_INT_CLEAR				0x0B
#define VL_REG_GPIO_HV_MUX_ACTIVE_HIGH			0x84

#define VL_REG_RESULT_INT_STATUS			0x13
#define VL_REG_RESULT_RANGE_STATUS			0x14
#define VL_REG_RESULT_RANGE_SATTUS_COMPLETE		BIT(0)

#define VL_REG_I2C_SLAVE_DEVICE_ADDRESS			0x8a

#define VL_REG_IDENTIFICATION_MODEL_ID			0xc0
#define VL_REG_IDENTIFICATION_REVISION_ID		0xc2

struct vl53l0x_data {
	struct i2c_client *client;
};

static int vl53l0x_read_proximity(struct vl53l0x_data *data,
				  const struct iio_chan_spec *chan,
				  int *val)
{
	u8 write_command = VL_REG_RESULT_RANGE_STATUS;
	struct i2c_client *client = data->client;
	unsigned int tries = 20;
	struct i2c_msg msg[2];
	u8 buffer[12];
	int ret;

	ret = i2c_smbus_write_byte_data(data->client,
					VL_REG_SYSRANGE_START, 1);
	if (ret < 0)
		return ret;

	do {
		ret = i2c_smbus_read_byte_data(data->client,
					       VL_REG_RESULT_RANGE_STATUS);
		if (ret < 0)
			return ret;

		if (ret & VL_REG_RESULT_RANGE_SATTUS_COMPLETE)
			break;

		usleep_range(1000, 5000);
	} while (tries--);
	if (!tries)
		return -ETIMEDOUT;

	msg[0].addr = client->addr;
	msg[0].buf = &write_command;
	msg[0].len = 1;
	msg[0].flags = client->flags | I2C_M_STOP;

	msg[1].addr = client->addr;
	msg[1].buf = buffer;
	msg[1].len = 12;
	msg[1].flags = client->flags | I2C_M_RD;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0)
		return ret;
	else if (ret != 2)
		dev_err(&data->client->dev, "vl53l0x: consecutively read error. ");

	*val = le16_to_cpu((buffer[10] << 8) + buffer[11]);

	return 0;
}

static const struct iio_chan_spec vl53l0x_channels[] = {
	{
		.type = IIO_DISTANCE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
	},
	IIO_CHAN_SOFT_TIMESTAMP(1),
};

static int vl53l0x_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct vl53l0x_data *data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_DISTANCE) {
		dev_err(&data->client->dev, "vl53l0x: iio type error");
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = vl53l0x_read_proximity(data, chan, val);
		if (ret < 0)
			dev_err(&data->client->dev,
				"vl53l0x: raw value read error with %d", ret);

		return IIO_VAL_INT;
	default:
		dev_err(&data->client->dev, "vl53l0x: IIO_CHAN_* not recognzed.");
		return -EINVAL;
	}
}

static const struct iio_info vl53l0x_info = {
	.read_raw = vl53l0x_read_raw,
};

static int vl53l0x_probe(struct i2c_client *client)
{
	struct vl53l0x_data *data;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	i2c_set_clientdata(client, indio_dev);

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = VL53L0X_DRV_NAME;
	indio_dev->info = &vl53l0x_info;
	indio_dev->channels = vl53l0x_channels;
	indio_dev->num_channels = ARRAY_SIZE(vl53l0x_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id st_vl53l0x_dt_match[] = {
	{ .compatible = "st,vl53l0x-i2c", },
	{ }
};

static struct i2c_driver vl53l0x_driver = {
	.driver = {
		.name = VL53L0X_DRV_NAME,
		.of_match_table = st_vl53l0x_dt_match,
	},
	.probe_new = vl53l0x_probe,
};
module_i2c_driver(vl53l0x_driver);

MODULE_AUTHOR("Song Qiang <songqiang.1304521@gmail.com>");
MODULE_DESCRIPTION("ST vl53l0x ToF ranging sensor");
MODULE_LICENSE("GPL");
