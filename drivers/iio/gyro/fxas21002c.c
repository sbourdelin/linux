// SPDX-License-Identifier: GPL-2.0
/*
 * FXAS21002C - Digital Angular Rate Gyroscope driver
 *
 * Copyright (c) 2018, Afonso Bordado <afonsobordado@az8.co>
 *
 * IIO driver for FXAS21002C (7-bit I2C slave address 0x20 or 0x21).
 * Datasheet: https://www.nxp.com/docs/en/data-sheet/FXAS21002.pdf
 * TODO:
 *        Scale Boost Mode
 *        Power management
 *        GPIO Reset
 *        Power supplies
 *        Mount Matrix
 *        LowPass/HighPass Filters
 *        Buffers
 *        Interrupts
 *        Alarms
 *        SPI Support
 */

#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define FXAS21002C_DRV_NAME "fxas21002c"

#define FXAS21002C_MAX_TRANSITION_TIME_MS 61

#define FXAS21002C_CHIP_ID             0xD7

#define FXAS21002C_REG_STATUS          0x00
#define FXAS21002C_REG_OUT_X_MSB       0x01
#define FXAS21002C_REG_OUT_X_LSB       0x02
#define FXAS21002C_REG_OUT_Y_MSB       0x03
#define FXAS21002C_REG_OUT_Y_LSB       0x04
#define FXAS21002C_REG_OUT_Z_MSB       0x05
#define FXAS21002C_REG_OUT_Z_LSB       0x06
#define FXAS21002C_REG_DR_STATUS       0x07
#define FXAS21002C_REG_F_STATUS        0x08
#define FXAS21002C_REG_F_SETUP         0x09
#define FXAS21002C_REG_F_EVENT         0x0A
#define FXAS21002C_REG_INT_SRC_FLAG    0x0B
#define FXAS21002C_REG_WHO_AM_I        0x0C

#define FXAS21002C_REG_CTRL_REG0       0x0D
#define FXAS21002C_SCALE_MASK          GENMASK(1, 0)

#define FXAS21002C_REG_RT_CFG          0x0E
#define FXAS21002C_REG_RT_SRC          0x0F
#define FXAS21002C_REG_RT_THS          0x10
#define FXAS21002C_REG_RT_COUNT        0x11
#define FXAS21002C_REG_TEMP            0x12

#define FXAS21002C_REG_CTRL_REG1       0x13
#define FXAS21002C_RST_BIT             BIT(6)
#define FXAS21002C_ACTIVE_BIT          BIT(1)
#define FXAS21002C_READY_BIT           BIT(0)

#define FXAS21002C_ODR_SHIFT           2
#define FXAS21002C_ODR_MASK            GENMASK(4, 2)

#define FXAS21002C_REG_CTRL_REG2       0x14
#define FXAS21002C_REG_CTRL_REG3       0x15

#define FXAS21002C_TEMP_SCALE          1000


#define FXAS21002C_SCALE(scale) (IIO_DEGREE_TO_RAD(62500 >> (scale)))

#define FXAS21002C_SAMPLE_FREQ(odr) (800 >> (odr))
#define FXAS21002C_SAMPLE_FREQ_MICRO(odr) ( \
		((odr) == FXAS21002C_ODR_12_5) ? 500000 : 0)

enum {
	ID_FXAS21002C,
};

enum fxas21002c_operating_mode {
	FXAS21002C_OM_BOOT,
	FXAS21002C_OM_STANDBY,
	FXAS21002C_OM_READY,
	FXAS21002C_OM_ACTIVE,
};

struct fxas21002c_data {
	struct i2c_client *client;
	struct regmap *regmap;
};

enum fxas21002c_scale {
	FXAS21002C_SCALE_62MDPS,
	FXAS21002C_SCALE_31MDPS,
	FXAS21002C_SCALE_15MDPS,
	FXAS21002C_SCALE_7MDPS,
	__FXAS21002C_SCALE_MAX,
};

enum fxas21002c_odr {
	FXAS21002C_ODR_800,
	FXAS21002C_ODR_400,
	FXAS21002C_ODR_200,
	FXAS21002C_ODR_100,
	FXAS21002C_ODR_50,
	FXAS21002C_ODR_25,
	FXAS21002C_ODR_12_5,
	__FXAS21002C_ODR_MAX,
};

static const struct regmap_range fxas21002c_writable_ranges[] = {
	regmap_reg_range(FXAS21002C_REG_F_SETUP, FXAS21002C_REG_F_SETUP),
	regmap_reg_range(FXAS21002C_REG_CTRL_REG0, FXAS21002C_REG_RT_CFG),
	regmap_reg_range(FXAS21002C_REG_RT_THS, FXAS21002C_REG_RT_COUNT),
	regmap_reg_range(FXAS21002C_REG_CTRL_REG1, FXAS21002C_REG_CTRL_REG3),
};

static const struct regmap_access_table fxas21002c_writable_table = {
	.yes_ranges = fxas21002c_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(fxas21002c_writable_ranges),
};

static const struct regmap_range fxas21002c_volatile_ranges[] = {
	regmap_reg_range(FXAS21002C_REG_STATUS, FXAS21002C_REG_F_STATUS),
	regmap_reg_range(FXAS21002C_REG_F_EVENT, FXAS21002C_REG_INT_SRC_FLAG),
	regmap_reg_range(FXAS21002C_REG_RT_COUNT, FXAS21002C_REG_CTRL_REG1),
};

static const struct regmap_access_table fxas21002c_volatile_table = {
	.yes_ranges = fxas21002c_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(fxas21002c_volatile_ranges),
};

const struct regmap_config fxas21002c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = FXAS21002C_REG_CTRL_REG3,
	/* We don't specify a .rd_table because everything is readable */
	.wr_table = &fxas21002c_writable_table,
	.volatile_table = &fxas21002c_volatile_table,
};

#define FXAS21002C_GYRO_CHAN(_axis) {					\
	.type = IIO_ANGL_VEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_ ## _axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.address = FXAS21002C_REG_OUT_ ## _axis ## _MSB,		\
}

static const struct iio_chan_spec fxas21002c_channels[] = {
	{
		.type = IIO_TEMP,
		.address = FXAS21002C_REG_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	FXAS21002C_GYRO_CHAN(X),
	FXAS21002C_GYRO_CHAN(Y),
	FXAS21002C_GYRO_CHAN(Z),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static int fxas21002c_set_operating_mode(struct fxas21002c_data *data,
					 enum fxas21002c_operating_mode om)
{
	int ret;
	int mask;

	switch (om) {
	case FXAS21002C_OM_STANDBY:
		mask = 0;
		break;
	case FXAS21002C_OM_READY:
		mask = FXAS21002C_READY_BIT;
		break;
	case FXAS21002C_OM_ACTIVE:
		mask = FXAS21002C_ACTIVE_BIT;
		break;

	default:
		return -EINVAL;
	}

	ret = regmap_write(data->regmap, FXAS21002C_REG_CTRL_REG1, mask);
	if (ret) {
		dev_err(&data->client->dev,
			"could not switch operating mode\n");
		return ret;
	}

	msleep(FXAS21002C_MAX_TRANSITION_TIME_MS);

	return 0;
}

static void fxas21002c_standby(void *_data)
{
	struct fxas21002c_data *data = _data;

	fxas21002c_set_operating_mode(data, FXAS21002C_OM_STANDBY);
}

static int fxas21002c_reset(struct fxas21002c_data *data)
{
	int ret;

	/*
	 * On issuing a Software Reset command over an I2C interface,
	 * the device immediately resets and does not send any
	 * acknowledgment (ACK) of the written byte to the Master.
	 *
	 * This is documented in table 46 on the datasheet. Due to this
	 * the write will fail with EREMOTEIO.
	 */
	ret = regmap_write(data->regmap,
			   FXAS21002C_REG_CTRL_REG1, FXAS21002C_RST_BIT);

	if (ret != -EREMOTEIO) {
		dev_err(&data->client->dev, "could not reset device\n");
		return ret;
	}

	regcache_mark_dirty(data->regmap);

	/* Wait for device to boot up */
	msleep(FXAS21002C_MAX_TRANSITION_TIME_MS);

	return 0;
}

static int fxas21002c_verify_chip(struct fxas21002c_data *data)
{
	int ret;
	unsigned int chip_id;

	ret = regmap_read(data->regmap, FXAS21002C_REG_WHO_AM_I, &chip_id);
	if (ret) {
		dev_err(&data->client->dev, "could not read device id\n");
		return ret;
	}

	if (chip_id != FXAS21002C_CHIP_ID) {
		dev_err(&data->client->dev,
			"unsupported chip id %02x\n", chip_id);
		return -ENODEV;
	}

	return 0;
}

static int fxas21002c_read_oneshot(struct fxas21002c_data *data,
				   struct iio_chan_spec const *chan, int *val)
{
	int ret;
	__be16 bulk_raw;
	unsigned int uval;

	switch (chan->type) {
	case IIO_ANGL_VEL:
		ret = regmap_bulk_read(data->regmap, chan->address,
				       &bulk_raw, sizeof(bulk_raw));
		if (ret)
			return ret;

		*val = sign_extend32(be16_to_cpu(bulk_raw), 15);
		return IIO_VAL_INT;
	case IIO_TEMP:
		ret = regmap_read(data->regmap, chan->address, &uval);
		if (ret)
			return ret;

		*val = sign_extend32(uval, 7);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int fxas21002c_scale_read(struct fxas21002c_data *data, int *val,
				 int *val2)
{
	int ret;
	unsigned int raw;

	ret = regmap_read(data->regmap, FXAS21002C_REG_CTRL_REG0, &raw);
	if (ret)
		return ret;

	raw &= FXAS21002C_SCALE_MASK;

	*val = 0;
	*val2 = FXAS21002C_SCALE(raw);

	return IIO_VAL_INT_PLUS_MICRO;
}

static int fxas21002c_odr_read(struct fxas21002c_data *data, int *val,
			       int *val2)
{
	int ret;
	unsigned int raw;

	ret = regmap_read(data->regmap, FXAS21002C_REG_CTRL_REG1, &raw);
	if (ret)
		return ret;

	raw = (raw & FXAS21002C_ODR_MASK) >> FXAS21002C_ODR_SHIFT;

	/*
	 * We don't use this mode but according to the datasheet its
	 * also a 12.5Hz
	 */
	if (raw == 7)
		raw = FXAS21002C_ODR_12_5;

	*val = FXAS21002C_SAMPLE_FREQ(raw);
	*val2 = FXAS21002C_SAMPLE_FREQ_MICRO(raw);

	return IIO_VAL_INT_PLUS_MICRO;
}

static int fxas21002c_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, int *val,
			       int *val2, long mask)
{
	struct fxas21002c_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return fxas21002c_read_oneshot(data, chan, val);
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ANGL_VEL:
			return fxas21002c_scale_read(data, val, val2);
		case IIO_TEMP:
			*val = FXAS21002C_TEMP_SCALE;

			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->type != IIO_ANGL_VEL)
			return -EINVAL;

		return fxas21002c_odr_read(data, val, val2);
	}

	return -EINVAL;
}


static int fxas21002c_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, int val,
				int val2, long mask)
{
	struct fxas21002c_data *data = iio_priv(indio_dev);
	int ret = -EINVAL;
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < __FXAS21002C_ODR_MAX; i++) {
			if (FXAS21002C_SAMPLE_FREQ(i) == val &&
			    FXAS21002C_SAMPLE_FREQ_MICRO(i) == val2)
				break;
		}

		if (i == __FXAS21002C_ODR_MAX)
			break;

		return regmap_update_bits(data->regmap,
					  FXAS21002C_REG_CTRL_REG1,
					  FXAS21002C_ODR_MASK,
					  i << FXAS21002C_ODR_SHIFT);
	case IIO_CHAN_INFO_SCALE:
		for (i = 0; i < __FXAS21002C_SCALE_MAX; i++) {
			if (val == 0 && FXAS21002C_SCALE(i) == val2)
				break;
		}

		if (i == __FXAS21002C_SCALE_MAX)
			break;

		return regmap_update_bits(data->regmap,
					  FXAS21002C_REG_CTRL_REG0,
					  FXAS21002C_SCALE_MASK, i);
	}

	return ret;
}

static IIO_CONST_ATTR(anglevel_scale_available,
		      "0.001090831 "  /* 62.5    mdps */
		      "0.000545415 "  /* 31.25   mdps */
		      "0.000272708 "  /* 15.625  mdps */
		      "0.000136354"); /*  7.8125 mdps */

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("800 400 200 100 50 25 12.5");

static struct attribute *fxas21002c_attributes[] = {
	&iio_const_attr_anglevel_scale_available.dev_attr.attr,
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL
};

static const struct attribute_group fxas21002c_attribute_group = {
	.attrs = fxas21002c_attributes,
};

static const struct iio_info fxas21002c_info = {
	.read_raw		= fxas21002c_read_raw,
	.write_raw              = fxas21002c_write_raw,
	.attrs                  = &fxas21002c_attribute_group,
};

static int fxas21002c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	int ret;
	struct iio_dev *indio_dev;
	struct fxas21002c_data *data;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	i2c_set_clientdata(client, indio_dev);
	data = iio_priv(indio_dev);
	data->client = client;

	data->regmap = devm_regmap_init_i2c(client, &fxas21002c_regmap_config);
	if (IS_ERR(data->regmap)) {
		ret = PTR_ERR(data->regmap);
		dev_err(&client->dev,
			"Failed to allocate regmap, err: %d\n", ret);
		return ret;
	}

	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = fxas21002c_channels;
	indio_dev->num_channels = ARRAY_SIZE(fxas21002c_channels);
	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &fxas21002c_info;

	ret = fxas21002c_verify_chip(data);
	if (ret < 0)
		return ret;

	ret = fxas21002c_reset(data);
	if (ret < 0)
		return ret;

	ret = fxas21002c_set_operating_mode(data, FXAS21002C_OM_ACTIVE);
	if (ret < 0)
		return ret;

	ret = devm_add_action(&client->dev, fxas21002c_standby, data);
	if (ret < 0) {
		fxas21002c_standby(data);
		dev_err(&client->dev, "failed to add standby action\n");
		return ret;
	}

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		dev_err(&client->dev, "failed to register iio device\n");

	return ret;
}

static const struct of_device_id fxas21002c_of_ids[] = {
	{.compatible = "fsl,fxas21002c"},
	{}
};
MODULE_DEVICE_TABLE(of, fxas21002c_of_ids);

static const struct i2c_device_id fxas21002c_id[] = {
	{"fxas21002c", ID_FXAS21002C},
	{}
};
MODULE_DEVICE_TABLE(i2c, fxas21002c_id);

static struct i2c_driver fxas21002c_driver = {
	.driver = {
		.name = FXAS21002C_DRV_NAME,
		.of_match_table = fxas21002c_of_ids,
	},
	.probe		= fxas21002c_probe,
	.id_table	= fxas21002c_id,
};

module_i2c_driver(fxas21002c_driver);

MODULE_AUTHOR("Afonso Bordado <afonsobordado@az8.co>");
MODULE_DESCRIPTION("FXAS21002C Digital Angular Rate Gyroscope driver");
MODULE_LICENSE("GPL v2");
