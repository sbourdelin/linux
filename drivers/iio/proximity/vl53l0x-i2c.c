// SPDX-License-Identifier: GPL-2.0+
/*
 *  vl53l0x-i2c.c - Support for STM VL53L0X FlightSense TOF
 *					Ranger Sensor on a i2c bus.
 *
 *  Copyright (C) 2016 STMicroelectronics Imaging Division.
 *  Copyright (C) 2018 Song Qiang <songqiang.1304521@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/iio/iio.h>

#define VL53L0X_DRV_NAME			"vl53l0x"

/* Device register map */
#define VL_REG_SYSRANGE_START					0x000
#define VL_REG_SYSRANGE_MODE_MASK				0x0F
#define VL_REG_SYSRANGE_MODE_START_STOP			0x01
#define VL_REG_SYSRANGE_MODE_SINGLESHOT			0x00
#define VL_REG_SYSRANGE_MODE_BACKTOBACK			0x02
#define VL_REG_SYSRANGE_MODE_TIMED				0x04
#define VL_REG_SYSRANGE_MODE_HISTOGRAM			0x08

#define VL_REG_SYS_THRESH_HIGH					0x000C
#define VL_REG_SYS_THRESH_LOW					0x000E

#define VL_REG_SYS_SEQUENCE_CFG					0x0001
#define VL_REG_SYS_RANGE_CFG					0x0009
#define VL_REG_SYS_INTERMEASUREMENT_PERIOD		0x0004

#define VL_REG_SYS_INT_CFG_GPIO					0x000A
#define VL_REG_SYS_INT_GPIO_DISABLED			0x00
#define VL_REG_SYS_INT_GPIO_LEVEL_LOW			0x01
#define VL_REG_SYS_INT_GPIO_LEVEL_HIGH			0x02
#define VL_REG_SYS_INT_GPIO_OUT_OF_WINDOW		0x03
#define VL_REG_SYS_INT_GPIO_NEW_SAMPLE_READY	0x04
#define VL_REG_GPIO_HV_MUX_ACTIVE_HIGH			0x0084
#define VL_REG_SYS_INT_CLEAR					0x000B

/* Result registers */
#define VL_REG_RESULT_INT_STATUS				0x0013
#define VL_REG_RESULT_RANGE_STATUS				0x0014

#define VL_REG_RESULT_CORE_PAGE  1
#define VL_REG_RESULT_CORE_AMBIENT_WINDOW_EVENTS_RTN	0x00BC
#define VL_REG_RESULT_CORE_RANGING_TOTAL_EVENTS_RTN		0x00C0
#define VL_REG_RESULT_CORE_AMBIENT_WINDOW_EVENTS_REF	0x00D0
#define VL_REG_RESULT_CORE_RANGING_TOTAL_EVENTS_REF		0x00D4
#define VL_REG_RESULT_PEAK_SIGNAL_RATE_REF				0x00B6

/* Algo register */
#define VL_REG_ALGO_PART_TO_PART_RANGE_OFFSET_MM		0x0028

#define VL_REG_I2C_SLAVE_DEVICE_ADDRESS					0x008a

/* Check Limit registers */
#define VL_REG_MSRC_CFG_CONTROL						0x0060

#define VL_REG_PRE_RANGE_CFG_MIN_SNR					0X0027
#define VL_REG_PRE_RANGE_CFG_VALID_PHASE_LOW			0x0056
#define VL_REG_PRE_RANGE_CFG_VALID_PHASE_HIGH			0x0057
#define VL_REG_PRE_RANGE_MIN_COUNT_RATE_RTN_LIMIT		0x0064

#define VL_REG_FINAL_RANGE_CFG_MIN_SNR					0X0067
#define VL_REG_FINAL_RANGE_CFG_VALID_PHASE_LOW			0x0047
#define VL_REG_FINAL_RANGE_CFG_VALID_PHASE_HIGH			0x0048
#define VL_REG_FINAL_RANGE_CFG_MIN_COUNT_RATE_RTN_LIMIT	0x0044

#define VL_REG_PRE_RANGE_CFG_SIGMA_THRESH_HI			0X0061
#define VL_REG_PRE_RANGE_CFG_SIGMA_THRESH_LO			0X0062

/* PRE RANGE registers */
#define VL_REG_PRE_RANGE_CFG_VCSEL_PERIOD				0x0050
#define VL_REG_PRE_RANGE_CFG_TIMEOUT_MACROP_HI			0x0051
#define VL_REG_PRE_RANGE_CFG_TIMEOUT_MACROP_LO			0x0052

#define VL_REG_SYS_HISTOGRAM_BIN					0x0081
#define VL_REG_HISTOGRAM_CFG_INITIAL_PHASE_SELECT		0x0033
#define VL_REG_HISTOGRAM_CFG_READOUT_CTRL				0x0055

#define VL_REG_FINAL_RANGE_CFG_VCSEL_PERIOD				0x0070
#define VL_REG_FINAL_RANGE_CFG_TIMEOUT_MACROP_HI		0x0071
#define VL_REG_FINAL_RANGE_CFG_TIMEOUT_MACROP_LO		0x0072
#define VL_REG_CROSSTALK_COMPENSATION_PEAK_RATE_MCPS	0x0020

#define VL_REG_MSRC_CFG_TIMEOUT_MACROP					0x0046

#define VL_REG_SOFT_RESET_GO2_SOFT_RESET_N				0x00bf
#define VL_REG_IDENTIFICATION_MODEL_ID					0x00c0
#define VL_REG_IDENTIFICATION_REVISION_ID				0x00c2

#define VL_REG_OSC_CALIBRATE_VAL					0x00f8

#define VL_SIGMA_ESTIMATE_MAX_VALUE					65535
/* equivalent to a range sigma of 655.35mm */

#define VL_REG_GLOBAL_CFG_VCSEL_WIDTH					0x032
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_0			0x0B0
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_1			0x0B1
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_2			0x0B2
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_3			0x0B3
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_4			0x0B4
#define VL_REG_GLOBAL_CFG_SPAD_ENABLES_REF_5			0x0B5
#define VL_REG_GLOBAL_CFG_REF_EN_START_SELECT			0xB6
#define VL_REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD		0x4E /* 0x14E */
#define VL_REG_DYNAMIC_SPAD_REF_EN_START_OFFSET			0x4F /* 0x14F */
#define VL_REG_POWER_MANAGEMENT_GO1_POWER_FORCE			0x80

/*
 * Speed of light in um per 1E-10 Seconds
 */
#define VL_SPEED_OF_LIGHT_IN_AIR					2997
#define VL_REG_VHV_CFG_PAD_SCL_SDA__EXTSUP_HV			0x0089
#define VL_REG_ALGO_PHASECAL_LIM			0x0030 /* 0x130 */
#define VL_REG_ALGO_PHASECAL_CFG_TIMEOUT				0x0030

struct vl53l0x_data {
	struct i2c_client *client;
	struct mutex lock;
	int	useLongRange;
};

static int vl53l0x_read_proximity(struct vl53l0x_data *data,
				const struct iio_chan_spec *chan,
				int *val)
{
	int ret;
	struct i2c_client *client = data->client;
	int tries = 20;
	u8 buffer[12];
	struct i2c_msg msg[2];
	u8 write_command = VL_REG_RESULT_RANGE_STATUS;

	ret = i2c_smbus_write_byte_data(data->client,
					VL_REG_SYSRANGE_START, 1);
	if (ret < 0)
		return ret;

	while (tries-- > 0) {
		ret = i2c_smbus_read_byte_data(data->client,
						VL_REG_RESULT_RANGE_STATUS);
		if (ret < 0)
			return ret;

		if (ret & 0x01)
			break;
		usleep_range(1000, 5000);
	}

	if (tries < 0)
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

	if (ret != 2) {
		pr_err("vl53l0x: consecutively read error. ");
		return ret;
	}

	*val = __le16_to_cpu((buffer[10] << 8) + buffer[11]);

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
		pr_err("vl53l0x: iio type error");
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;
		ret = vl53l0x_read_proximity(data, chan, val);
		if (ret < 0)
			pr_err("vl53l0x: raw value read error with %d", ret);

		ret = IIO_VAL_INT;
		iio_device_release_direct_mode(indio_dev);
		return ret;
	default:
		pr_err("vl53l0x: IIO_CHAN_* not recognzed.");
		return -EINVAL;
	}
}

static const struct iio_info vl53l0x_info = {
	.read_raw = vl53l0x_read_raw,
};

static int vl53l0x_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	int ret;
	struct vl53l0x_data *data;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	i2c_set_clientdata(client, indio_dev);
	mutex_init(&data->lock);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE))
		return -EOPNOTSUPP;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = VL53L0X_DRV_NAME;
	indio_dev->info = &vl53l0x_info;
	indio_dev->channels = vl53l0x_channels;
	indio_dev->num_channels = ARRAY_SIZE(vl53l0x_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	//probe 0xc0 if the value is 0xEE.

	ret = iio_device_register(indio_dev);
	if (ret)
		return ret;

	dev_set_drvdata(&client->dev, data);

	return 0;
}

static int vl53l0x_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct vl53l0x_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	kfree(data);

	return 0;
}

static const struct i2c_device_id vl53l0x_id[] = {
	{ VL53L0X_DRV_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, vl53l0x_id);

static const struct of_device_id st_vl53l0x_dt_match[] = {
	{ .compatible = "st,vl53l0x-i2c", },
	{ },
};

static struct i2c_driver vl53l0x_driver = {
	.driver = {
		.name	= VL53L0X_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = st_vl53l0x_dt_match,
	},
	.probe	= vl53l0x_probe,
	.remove	= vl53l0x_remove,
	.id_table = vl53l0x_id,
};
module_i2c_driver(vl53l0x_driver);

MODULE_AUTHOR("Song Qiang <songqiang.1304521@gmail.com>");
MODULE_DESCRIPTION("ST vl53l0x ToF ranging sensor");
MODULE_LICENSE("GPL");
