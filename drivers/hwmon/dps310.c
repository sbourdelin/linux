/*
 * Copyright 2017 IBM Corporation
 *
 * Joel Stanley <joel@jms.id.au>
 *
 * The DPS310 is a barometric pressure and temperature sensor.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/hwmon.h>
#include <linux/module.h>

#define PRS_B2		0x00
#define PRS_B1		0x01
#define PRS_B0		0x02
#define TMP_B2		0x03
#define TMP_B1		0x04
#define TMP_B0		0x05
#define PRS_CFG		0x06
#define TMP_CFG		0x07
#define  TMP_RATE_BITS	GENMASK(6, 4)
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
#define RESET		0x0c
#define  RESET_MAGIC	(BIT(0) | BIT(3))
#define COEF_BASE	0x10

#define TMP_BASE	TMP_B2
#define PRS_BASE	PRS_B2

#define TMP_RATE(_n)	ilog2(_n)
#define TMP_PRC(_n)	ilog2(_n)

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
	struct regmap *regmap;
	int interval;
	s32 c0, c1;
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

static int dps310_get_scale_factor(struct device *dev)
{
	struct dps310_data *data = dev_get_drvdata(dev);
	int val, r;

	r = regmap_read(data->regmap, TMP_CFG, &val);
	if (r < 0)
		return r;

	/* Scale factor is bottom 4 bits of the register */
	val = val & GENMASK(3, 0);
	if (val < 0 || val > ARRAY_SIZE(scale_factor))
		return -EINVAL;

	return scale_factor[val];
}

static int dps310_read_temp(struct device *dev, u32 attr, int channel,
			     long *temp)
{
	struct dps310_data *data = dev_get_drvdata(dev);
	struct regmap *regmap = data->regmap;
	uint8_t val[3] = {0};
	int r, ready;
	int kT, T_raw;

	switch (attr) {
	case hwmon_temp_input:
		r = regmap_read(regmap, MEAS_CFG, &ready);
		if (r < 0)
			return r;
		if (!(ready & TMP_RDY)) {
			dev_err(dev, "tmp not ready\n");
			return -EAGAIN;
		}

		/* Choose scaling factor kT based on chosen precision rate */
		kT = dps310_get_scale_factor(dev);

		r = regmap_bulk_read(regmap, TMP_BASE, val, 3);
		if (r < 0)
			return r;
		T_raw = (val[0] << 16) | (val[1] << 8) | val[2];
		T_raw = dps310_twos_compliment(T_raw, 24);

		/* (c0 * 0.5 + c1 * T_raw / kT) * 1000 m°C */
		*temp = ((data->c0 >> 1) + (data->c1 * T_raw) / kT) * 1000;

		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int dps310_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_temp:
		return dps310_read_temp(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int dps310_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_temp:
		/* Fall through */
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t dps310_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}


static const u32 dps310_temp_config[] = {
	HWMON_T_INPUT,
	0
};

static const struct hwmon_channel_info dps310_temp = {
	.type = hwmon_temp,
	.config = dps310_temp_config,
};

static const struct hwmon_channel_info *dps310_info[] = {
	&dps310_temp,
	NULL
};

static const struct hwmon_ops dps310_hwmon_ops = {
	.is_visible = dps310_is_visible,
	.read = dps310_read,
	.write = dps310_write,
};

static const struct hwmon_chip_info dps310_chip_info = {
	.ops = &dps310_hwmon_ops,
	.info = dps310_info,
};

static bool dps310_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PRS_CFG:
	case TMP_CFG:
	case MEAS_CFG:
	case RESET:
		return true;
	default:
		return false;
	}
}

static bool dps310_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case PRS_B2:
	case PRS_B1:
	case PRS_B0:
	case TMP_B2:
	case TMP_B1:
	case TMP_B0:
	case MEAS_CFG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config dps310_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.writeable_reg = dps310_is_writeable_reg,
	.volatile_reg = dps310_is_volatile_reg,
	.cache_type = REGCACHE_RBTREE,
	.use_single_rw = true,
};

static int dps310_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct dps310_data *data;
	struct device *hwmon_dev;
	int r;

	data = devm_kzalloc(&client->dev,  sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regmap = devm_regmap_init_i2c(client, &dps310_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	r = regmap_write(data->regmap, TMP_CFG, TMP_EXT | TMP_PRC(128));
	if (r < 0)
		return r;

	/* Turn on background temperature measurement */
	r = regmap_update_bits(data->regmap, MEAS_CFG, MEAS_CTRL_BITS,
			TEMP_EN);
	if (r < 0)
		return r;

	/* Get calibration coefficients required for reporting temperature */
	r = dps310_get_temp_coef(data);
	if (r == -EAGAIN)
		return -EPROBE_DEFER;
	if (r < 0)
		return r;

	hwmon_dev = devm_hwmon_device_register_with_info(&client->dev,
			client->name, data, &dps310_chip_info, NULL);

	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(&client->dev, "%s: sensor '%s'\n", dev_name(hwmon_dev),
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
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "dps310",
	},
	.probe = dps310_probe,
	.address_list = normal_i2c,
	.id_table = dps310_id,
};
module_i2c_driver(dps310_driver);

MODULE_AUTHOR("Joel Stanley <joel@jms.id.au>");
MODULE_DESCRIPTION("Infineon DPS310 driver");
MODULE_LICENSE("GPL");
