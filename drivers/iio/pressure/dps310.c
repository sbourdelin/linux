/*
 * Copyright 2017 IBM Corporation
 *
 * Joel Stanley <joel@jms.id.au>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * The DPS310 is a barometric pressure and temperature sensor.
 * Currently only reading a single temperature is supported by
 * this driver.
 *
 * TODO:
 *  - Pressure sensor readings
 *  - Optionally support the FIFO
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define PRS_BASE	0x00
#define TMP_BASE	0x03
#define PRS_CFG		0x06
#define TMP_CFG		0x07
#define  TMP_RATE_BITS	GENMASK(6, 4)
#define  TMP_PRC_BITS	GENMASK(3, 0)
#define  TMP_EXT	BIT(7)
#define MEAS_CFG	0x08
#define  MEAS_CTRL_BITS	GENMASK(2, 0)
#define   PRESSURE_EN	BIT(0)
#define   TEMP_EN	BIT(1)
#define   BACKGROUND	BIT(2)
#define  PRS_RDY	BIT(4)
#define  TMP_RDY	BIT(5)
#define  SENSOR_RDY	BIT(6)
#define  COEF_RDY	BIT(7)
#define CFG_REG		0x09
#define  INT_HL		BIT(7)
#define  TMP_SHIFT_EN	BIT(3)
#define  PRS_SHIFT_EN	BIT(4)
#define  FIFO_EN	BIT(5)
#define  SPI_EN		BIT(6)
#define RESET		0x0c
#define  RESET_MAGIC	(BIT(0) | BIT(3))
#define COEF_BASE	0x10

#define TMP_RATE(_n)	ilog2(_n)
#define TMP_PRC(_n)	ilog2(_n)

#define MCELSIUS_PER_CELSIUS	1000

const int scale_factor[] = {
	 524288,
	1572864,
	3670016,
	7864320,
	 253952,
	 516096,
	1040384,
	2088960,
};

struct dps310_data {
	struct i2c_client *client;
	struct regmap *regmap;

	s32 c0, c1;
	s32 temp_raw;
};

static const struct iio_chan_spec dps310_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_OFFSET) |
			BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			BIT(IIO_CHAN_INFO_RAW),
	},
};

static s32 dps310_twos_compliment(u32 raw, size_t num_bits)
{
	s32 out = raw;

	if (raw & BIT(num_bits - 1))
		out = raw - BIT(num_bits);

	return out;
}

static int dps310_get_temp_coef(struct dps310_data *data)
{
	struct regmap *regmap = data->regmap;
	uint8_t coef[3] = {0};
	int ready, r;
	u32 c0, c1;

	r = regmap_read(regmap, MEAS_CFG, &ready);
	if (r < 0)
		return r;

	if (!(ready & COEF_RDY))
		return -EAGAIN;

	/*
	 * Read temperature calibration coefficients c0 and c1 from the
	 * COEF register. The numbers are 12-bit 2's compliment numbers
	 */
	r = regmap_bulk_read(regmap, COEF_BASE, coef, 3);
	if (r < 0)
		return r;

	c0 = (coef[0] << 4) | (coef[1] >> 4);
	data->c0 = dps310_twos_compliment(c0, 12);

	c1 = ((coef[1] & GENMASK(3, 0)) << 8) | coef[2];
	data->c1 = dps310_twos_compliment(c1, 12);

	return 0;
}

static int dps310_get_temp_precision(struct dps310_data *data)
{
	int val, r;

	r = regmap_read(data->regmap, TMP_CFG, &val);
	if (r < 0)
		return r;

	/*
	 * Scale factor is bottom 4 bits of the register, but 1111 is
	 * reserved so just grab bottom three
	 */
	return BIT(val & GENMASK(2, 0));
}

static int dps310_set_temp_precision(struct dps310_data *data, int val)
{
	int ret;
	u8 shift_en;

	if (val < 0 || val > 128)
		return -EINVAL;

	shift_en = val >= 16 ? TMP_SHIFT_EN : 0;
	ret = regmap_write_bits(data->regmap, CFG_REG, TMP_SHIFT_EN, shift_en);
	if (ret)
		return ret;

	return regmap_update_bits(data->regmap, TMP_CFG, TMP_PRC_BITS,
			TMP_PRC(val));
}

static int dps310_set_temp_samp_freq(struct dps310_data *data, int freq)
{
	uint8_t val;

	if (freq < 0 || freq > 128)
		return -EINVAL;

	val = ilog2(freq) << 4;

	return regmap_update_bits(data->regmap, TMP_CFG, TMP_RATE_BITS, val);
}

static int dps310_get_temp_samp_freq(struct dps310_data *data)
{
	int val, r;

	r = regmap_read(data->regmap, TMP_CFG, &val);
	if (r < 0)
		return r;

	return BIT((val & TMP_RATE_BITS) >> 4);
}

static int dps310_get_temp_k(struct dps310_data *data)
{
	int index = ilog2(dps310_get_temp_precision(data));

	return scale_factor[index];
}

static int dps310_read_temp(struct dps310_data *data)
{
	struct device *dev = &data->client->dev;
	struct regmap *regmap = data->regmap;
	uint8_t val[3] = {0};
	int r, ready;
	int T_raw;

	r = regmap_read(regmap, MEAS_CFG, &ready);
	if (r < 0)
		return r;
	if (!(ready & TMP_RDY)) {
		dev_dbg(dev, "temperature not ready\n");
		return -EAGAIN;
	}

	r = regmap_bulk_read(regmap, TMP_BASE, val, 3);
	if (r < 0)
		return r;

	T_raw = (val[0] << 16) | (val[1] << 8) | val[2];
	data->temp_raw = dps310_twos_compliment(T_raw, 24);

	return 0;
}

static bool dps310_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PRS_CFG:
	case TMP_CFG:
	case MEAS_CFG:
	case CFG_REG:
	case RESET:
		return true;
	default:
		return false;
	}
}

static bool dps310_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PRS_BASE ... (PRS_BASE + 2):
	case TMP_BASE ... (TMP_BASE + 2):
	case MEAS_CFG:
		return true;
	default:
		return false;
	}
}

static int dps310_write_raw(struct iio_dev *iio,
			    struct iio_chan_spec const *chan, int val,
			    int val2, long mask)
{
	struct dps310_data *data = iio_priv(iio);

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return dps310_set_temp_samp_freq(data, val);
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		return dps310_set_temp_precision(data, val);
	default:
		return -EINVAL;
	}

	return -EINVAL;
}

static int dps310_read_raw(struct iio_dev *iio,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct dps310_data *data = iio_priv(iio);
	int ret;

	/* c0 * 0.5 + c1 * T_raw / kT °C */
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = dps310_get_temp_samp_freq(data);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_RAW:
		ret = dps310_read_temp(data);
		if (ret)
			return ret;

		*val = data->temp_raw * data->c1;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OFFSET:
		*val = (data->c0 >> 1) * dps310_get_temp_k(data);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = MCELSIUS_PER_CELSIUS;
		*val2 = dps310_get_temp_k(data);
		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*val = dps310_get_temp_precision(data);
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}

	return -EINVAL;
}

static const struct regmap_config dps310_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.writeable_reg = dps310_is_writeable_reg,
	.volatile_reg = dps310_is_volatile_reg,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 0x29,
};

static const struct iio_info dps310_info = {
	.driver_module = THIS_MODULE,
	.read_raw = dps310_read_raw,
	.write_raw = dps310_write_raw,
};

static int dps310_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct dps310_data *data;
	struct iio_dev *iio;
	int r;

	iio = devm_iio_device_alloc(&client->dev,  sizeof(*data));
	if (!iio)
		return -ENOMEM;

	data = iio_priv(iio);
	data->client = client;

	iio->dev.parent = &client->dev;
	iio->name = id->name;
	iio->channels = dps310_channels;
	iio->num_channels = ARRAY_SIZE(dps310_channels);
	iio->info = &dps310_info;
	iio->modes = INDIO_DIRECT_MODE;

	data->regmap = devm_regmap_init_i2c(client, &dps310_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	r = regmap_write(data->regmap, TMP_CFG, TMP_EXT | TMP_PRC(1));
	if (r < 0)
		return r;
	r = regmap_write_bits(data->regmap, CFG_REG, TMP_SHIFT_EN, 0);
	if (r < 0)
		return r;

	/* Turn on temperature measurement in the background */
	r = regmap_write_bits(data->regmap, MEAS_CFG, MEAS_CTRL_BITS,
			TEMP_EN | BACKGROUND);
	if (r < 0)
		return r;

	/*
	 * Calibration coefficients required for reporting temperature.
	 * They are availalbe 40ms after the device has started
	 */
	r = dps310_get_temp_coef(data);
	if (r == -EAGAIN)
		return -EPROBE_DEFER;
	if (r < 0)
		return r;

	r = devm_iio_device_register(&client->dev, iio);
	if (r)
		return r;

	i2c_set_clientdata(client, iio);

	dev_info(&client->dev, "%s: sensor '%s'\n", dev_name(&iio->dev),
			client->name);

	return 0;
}

static const struct i2c_device_id dps310_id[] = {
	{ "dps310", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, dps310_id);

static const unsigned short normal_i2c[] = {
	0x77, 0x76, I2C_CLIENT_END
};

static struct i2c_driver dps310_driver = {
	.driver = {
		.name = "dps310",
	},
	.probe = dps310_probe,
	.address_list = normal_i2c,
	.id_table = dps310_id,
};
module_i2c_driver(dps310_driver);

MODULE_AUTHOR("Joel Stanley <joel@jms.id.au>");
MODULE_DESCRIPTION("Infineon DPS310 pressure and temperature sensor");
MODULE_LICENSE("GPL");
