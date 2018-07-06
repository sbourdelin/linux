// SPDX-License-Identifier: GPL-2.0
/*
 * Bosch BME680 - Temperature, Pressure, Humidity & Gas Sensor
 *
 * Copyright (C) 2017 - 2018 Bosch Sensortec GmbH
 * Copyright (C) 2018 Himanshu Jha <himanshujha199640@gmail.com>
 *
 * IIO core driver - I2C & SPI bus support
 */
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/log2.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "bme680.h"

struct bme680_calib {
	u16 par_t1;
	s16 par_t2;
	s8  par_t3;
	u16 par_p1;
	s16 par_p2;
	s8  par_p3;
	s16 par_p4;
	s16 par_p5;
	s8  par_p6;
	s8  par_p7;
	s16 par_p8;
	s16 par_p9;
	u8  par_p10;
	u16 par_h1;
	u16 par_h2;
	s8  par_h3;
	s8  par_h4;
	s8  par_h5;
	s8  par_h6;
	s8  par_h7;
	s8  par_gh1;
	s16 par_gh2;
	s8  par_gh3;
	u8  res_heat_range;
	s8  res_heat_val;
	s8  range_sw_err;
};

struct bme680_data {
	struct regmap *regmap;
	struct bme680_calib bme680;
	u8 oversampling_temp;
	u8 oversampling_press;
	u8 oversampling_humid;
	u16 heater_dur;
	u16 heater_temp;
	/*
	 * Carryover value from temperature conversion, used in pressure
	 * and humidity compensation calculations.
	 */
	s32 t_fine;
};

const struct regmap_config bme680_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};
EXPORT_SYMBOL(bme680_regmap_config);

static const struct iio_chan_spec bme680_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),
	},
	{
		.type = IIO_RESISTANCE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static const int bme680_oversampling_avail[] = { 1, 2, 4, 8, 16 };

static int bme680_read_calib(struct bme680_data *data,
			     struct bme680_calib *calib)
{
	int ret;
	u8 t_buf[BME680_COEFF_SIZE] = {0};
	unsigned int tmp;
	struct device *dev = regmap_get_device(data->regmap);

	/* Read calibration parameters values from sensor's NVM */
	ret = regmap_bulk_read(data->regmap, BME680_COEFF_ADDR1,
			       t_buf, BME680_COEFF_ADDR1_LEN);
	if (ret < 0) {
		dev_err(dev,
			"failed to read first set of calibration parameters\n");
		return ret;
	}

	/* append the remaing parameters to the same array */
	ret = regmap_bulk_read(data->regmap, BME680_COEFF_ADDR2,
			       &t_buf[BME680_COEFF_ADDR1_LEN],
			       BME680_COEFF_ADDR2_LEN);
	if (ret < 0) {
		dev_err(dev,
			"failed to read second set of calibration parameters\n");
		return ret;
	}
		/* Temperature related coefficients */
	calib->par_t1 = BME680_CONCAT_BYTES(t_buf[BME680_T1_MSB_REG],
					    t_buf[BME680_T1_LSB_REG]);
	calib->par_t2 = BME680_CONCAT_BYTES(t_buf[BME680_T2_MSB_REG],
					    t_buf[BME680_T2_LSB_REG]);
	calib->par_t3 = t_buf[BME680_T3_REG];

		/* Pressure related coefficients */
	calib->par_p1 =	BME680_CONCAT_BYTES(t_buf[BME680_P1_MSB_REG],
					    t_buf[BME680_P1_LSB_REG]);
	calib->par_p2 = BME680_CONCAT_BYTES(t_buf[BME680_P2_MSB_REG],
					    t_buf[BME680_P2_LSB_REG]);
	calib->par_p3 = t_buf[BME680_P3_REG];
	calib->par_p4 = BME680_CONCAT_BYTES(t_buf[BME680_P4_MSB_REG],
					    t_buf[BME680_P4_LSB_REG]);
	calib->par_p5 = BME680_CONCAT_BYTES(t_buf[BME680_P5_MSB_REG],
					    t_buf[BME680_P5_LSB_REG]);
	calib->par_p6 = t_buf[BME680_P6_REG];
	calib->par_p7 = t_buf[BME680_P7_REG];
	calib->par_p8 = BME680_CONCAT_BYTES(t_buf[BME680_P8_MSB_REG],
					    t_buf[BME680_P8_LSB_REG]);
	calib->par_p9 = BME680_CONCAT_BYTES(t_buf[BME680_P9_MSB_REG],
					    t_buf[BME680_P9_LSB_REG]);
	calib->par_p10 = t_buf[BME680_P10_REG];

		/* Humidity related coefficients */
	calib->par_h1 = ((t_buf[BME680_H1_MSB_REG] << BME680_HUM_REG_SHIFT_VAL)
				| (t_buf[BME680_H1_LSB_REG]
					& BME680_BIT_H1_DATA_MSK));
	calib->par_h2 = ((t_buf[BME680_H2_MSB_REG] << BME680_HUM_REG_SHIFT_VAL)
				| ((t_buf[BME680_H2_LSB_REG])
					>> BME680_HUM_REG_SHIFT_VAL));
	calib->par_h3 = t_buf[BME680_H3_REG];
	calib->par_h4 = t_buf[BME680_H4_REG];
	calib->par_h5 = t_buf[BME680_H5_REG];
	calib->par_h6 = t_buf[BME680_H6_REG];
	calib->par_h7 = t_buf[BME680_H7_REG];

	/* Gas heater related coefficients */
	calib->par_gh1 = t_buf[BME680_GH1_REG];
	calib->par_gh2 = BME680_CONCAT_BYTES(t_buf[BME680_GH2_MSB_REG],
					     t_buf[BME680_GH2_LSB_REG]);
	calib->par_gh3 = t_buf[BME680_GH3_REG];

	/* Other coefficients */
	ret = regmap_read(data->regmap, BME680_REG_RES_HEAT_RANGE, &tmp);
	if (ret < 0) {
		dev_err(dev, "failed to read resistance heat range\n");
		return ret;
	}
	calib->res_heat_range = ((tmp & BME680_RHRANGE_MSK) / 16);

	ret = regmap_read(data->regmap, BME680_REG_RES_HEAT_VAL, &tmp);
	if (ret < 0) {
		dev_err(dev, "failed to read resistance heat value\n");
		return ret;
	}
	calib->res_heat_val = (s8)tmp;

	ret = regmap_read(data->regmap, BME680_REG_RANGE_SW_ERR, &tmp);
	if (ret < 0) {
		dev_err(dev, "failed to read range software error\n");
		return ret;
	}
	calib->range_sw_err = ((s8)tmp & (s8)BME680_RSERROR_MSK) / 16;

	return 0;
}

/* Taken from Bosch BME680 API */
static s32 bme680_compensate_temp(struct bme680_data *data,
				  s32 adc_temp)
{
	s64 var1, var2, var3, calc_temp;
	struct bme680_calib *calib = &data->bme680;

	var1 = ((s32) adc_temp >> 3) - ((s32) calib->par_t1 << 1);
	var2 = (var1 * (s32) calib->par_t2) >> 11;
	var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
	var3 = ((var3) * ((s32) calib->par_t3 << 4)) >> 14;
	data->t_fine = (s32) (var2 + var3);
	calc_temp = (s16) (((data->t_fine * 5) + 128) >> 8);

	return calc_temp;
}

/* Taken from Bosch BME680 API */
static u32 bme680_compensate_press(struct bme680_data *data,
				   u32 adc_press)
{
	struct bme680_calib *calib = &data->bme680;
	s32 var1 = 0, var2 = 0, var3 = 0, pressure_comp = 0;

	var1 = (((s32)data->t_fine) >> 1) - 64000;
	var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) *
				(s32)calib->par_p6) >> 2;
	var2 = var2 + ((var1 * (s32)calib->par_p5) << 1);
	var2 = (var2 >> 2) + ((s32)calib->par_p4 << 16);
	var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) *
			((s32)calib->par_p3 << 5)) >> 3) +
			(((s32)calib->par_p2 * var1) >> 1);
	var1 = var1 >> 18;
	var1 = ((32768 + var1) * (s32)calib->par_p1) >> 15;
	pressure_comp = 1048576 - adc_press;
	pressure_comp = (s32)((pressure_comp - (var2 >> 12)) * ((u32)3125));

	if (pressure_comp >= BME680_MAX_OVERFLOW_VAL)
		pressure_comp = ((pressure_comp / (u32)var1) << 1);
	else
		pressure_comp = ((pressure_comp << 1) / (u32)var1);

	var1 = ((s32)calib->par_p9 * (s32)(((pressure_comp >> 3) *
					(pressure_comp >> 3)) >> 13)) >> 12;
	var2 = ((s32)(pressure_comp >> 2) * (s32)calib->par_p8) >> 13;
	var3 = ((s32)(pressure_comp >> 8) * (s32)(pressure_comp >> 8) *
					(s32)(pressure_comp >> 8) *
					(s32)calib->par_p10) >> 17;

	pressure_comp = (s32)(pressure_comp) + ((var1 + var2 + var3 +
					((s32)calib->par_p7 << 7)) >> 4);

	return (u32)pressure_comp;
}

/* Taken from Bosch BME680 API */
static u32 bme680_compensate_humid(struct bme680_data *data,
				   u16 adc_humid)
{
	struct bme680_calib *calib = &data->bme680;
	s32 var1, var2, var3, var4, var5, var6, temp_scaled, calc_hum;

	temp_scaled = (((s32) data->t_fine * 5) + 128) >> 8;
	var1 = (s32) (adc_humid - ((s32) ((s32) calib->par_h1 * 16)))
				- (((temp_scaled * (s32) calib->par_h3)
				/ ((s32) 100)) >> 1);
	var2 = ((s32) calib->par_h2 *
			(((temp_scaled * (s32) calib->par_h4) / ((s32) 100))
			+ (((temp_scaled * ((temp_scaled * (s32) calib->par_h5)
			/ ((s32) 100))) >> 6)
			/ ((s32) 100)) + (s32) (1 << 14))) >> 10;
	var3 = var1 * var2;
	var4 = (s32) calib->par_h6 << 7;
	var4 = ((var4) + ((temp_scaled * (s32) calib->par_h7)
				       / ((s32) 100))) >> 4;
	var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
	var6 = (var4 * var5) >> 1;
	calc_hum = (((var3 + var6) >> 10) * ((s32) 1000)) >> 12;

	if (calc_hum > 100000) /* Cap at 100%rH */
		calc_hum = 100000;
	else if (calc_hum < 0)
		calc_hum = 0;

	return (u32) calc_hum;
}

/* Taken from Bosch BME680 API */
static u32 bme680_compensate_gas(struct bme680_data *data, u16 gas_res_adc,
				 u8 gas_range)
{
	struct bme680_calib *calib = &data->bme680;
	s64 var1;
	u64 var2;
	s64 var3;
	u32 calc_gas_res;

	/* Look up table 1 for the possible gas range values */
	u32 lookupTable1[16] = {2147483647, 2147483647, 2147483647, 2147483647,
				2147483647, 2126008810, 2147483647, 2130303777,
				2147483647, 2147483647, 2143188679, 2136746228,
				2147483647, 2126008810, 2147483647, 2147483647};
	/* Look up table 2 for the possible gas range values */
	u32 lookupTable2[16] = {4096000000, 2048000000, 1024000000, 512000000,
				255744255, 127110228, 64000000, 32258064,
				16016016, 8000000, 4000000, 2000000, 1000000,
				500000, 250000, 125000};

	var1 = (s64) ((1340 + (5 * (s64) calib->range_sw_err)) *
				((s64) lookupTable1[gas_range])) >> 16;
	var2 = (((s64) ((s64) gas_res_adc << 15) - (s64) (16777216)) + var1);
	var3 = (((s64) lookupTable2[gas_range] * (s64) var1) >> 9);
	calc_gas_res = (u32) ((var3 + ((s64) var2 >> 1)) / (s64) var2);

	return calc_gas_res;
}

/* Taken from Bosch BME680 API */
static u8 bme680_calc_heater_res(struct bme680_data *data, u16 temp)
{
	struct bme680_calib *calib = &data->bme680;
	u8 heatr_res;
	s32 var1, var2, var3, var4, var5, heatr_res_x100;

	if (temp > 400) /* Cap temperature */
		temp = 400;

	var1 = (((s32) BME680_AMB_TEMP * calib->par_gh3) / 1000) * 256;
	var2 = (calib->par_gh1 + 784) * (((((calib->par_gh2 + 154009) *
						temp * 5) / 100)
						+ 3276800) / 10);
	var3 = var1 + (var2 / 2);
	var4 = (var3 / (calib->res_heat_range + 4));
	var5 = (131 * calib->res_heat_val) + 65536;
	heatr_res_x100 = (s32) (((var4 / var5) - 250) * 34);
	heatr_res = (u8) ((heatr_res_x100 + 50) / 100);

	return heatr_res;
}

/* Taken from Bosch BME680 API */
static u8 bme680_calc_heater_dur(u16 dur)
{
	u8 durval, factor = 0;

	if (dur >= 0xfc0) {
		durval = 0xff; /* Max duration */
	} else {
		while (dur > 0x3F) {
			dur = dur / 4;
			factor += 1;
		}
		durval = (u8) (dur + (factor * 64));
	}

	return durval;
}

static int bme680_set_mode(struct bme680_data *data, bool mode)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;

	if (mode) {
		ret = regmap_write_bits(data->regmap, BME680_REG_CTRL_MEAS,
					BME680_MODE_MASK, BME680_MODE_FORCED);
		if (ret < 0) {
			dev_err(dev, "failed to set forced mode\n");
			return ret;
		}
	} else {
		ret = regmap_write_bits(data->regmap, BME680_REG_CTRL_MEAS,
					BME680_MODE_MASK, BME680_MODE_SLEEP);
		if (ret < 0) {
			dev_err(dev, "failed to set sleep mode\n");
			return ret;
		}
	}

	return 0;
}

static int bme680_chip_config(struct bme680_data *data)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;
	u8 osrs = BME680_OSRS_HUMID_X(data->oversampling_humid + 1);
	/*
	 * Highly recommended to set oversampling of humidity before
	 * temperature/pressure oversampling.
	 */
	ret = regmap_update_bits(data->regmap, BME680_REG_CTRL_HUMIDITY,
				 BME680_OSRS_HUMIDITY_MASK, osrs);
	if (ret < 0) {
		dev_err(dev, "failed to write ctrl_hum register\n");
		return ret;
	}

		/* IIR filter settings */
	ret = regmap_update_bits(data->regmap, BME680_REG_CONFIG,
				 BME680_FILTER_MASK,
				 BME680_FILTER_COEFF);
	if (ret < 0) {
		dev_err(dev, "failed to write config register\n");
		return ret;
	}

	osrs = BME680_OSRS_TEMP_X(data->oversampling_temp + 1) |
		BME680_OSRS_PRESS_X(data->oversampling_press + 1);

	ret = regmap_write_bits(data->regmap, BME680_REG_CTRL_MEAS,
				BME680_OSRS_TEMP_MASK |
				BME680_OSRS_PRESS_MASK,
				osrs);
	if (ret < 0) {
		dev_err(dev, "failed to write ctrl_meas register\n");
		return ret;
	}

	return 0;
}

static int bme680_gas_config(struct bme680_data *data)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;
	u8 heatr_res = 0, heatr_dur = 0;

	heatr_res = bme680_calc_heater_res(data, data->heater_temp);

		/* set target heater temperature */
	ret = regmap_write(data->regmap, BME680_REG_RES_HEAT_0, heatr_res);
	if (ret < 0) {
		dev_err(dev, "failed to write res_heat_0 register\n");
		return ret;
	}

	heatr_dur = bme680_calc_heater_dur(data->heater_dur);

		/* set target heating duration */
	ret = regmap_write(data->regmap, BME680_REG_GAS_WAIT_0, heatr_dur);
	if (ret < 0) {
		dev_err(dev, "failted to write gas_wait_0 register\n");
		return ret;
	}

	/* Selecting the runGas and NB conversion settings for the sensor */
	ret = regmap_update_bits(data->regmap, BME680_REG_CTRL_GAS_1,
				 BME680_RUN_GAS_MASK | BME680_NB_CONV_MASK,
				 BME680_RUN_GAS_EN | BME680_NB_CONV_0);
	if (ret < 0) {
		dev_err(dev, "failed to write ctrl_gas_1 register\n");
		return ret;
	}

	return 0;
}

/* Outputs temperature measurement in degC */
static int bme680_read_temp(struct bme680_data *data,
			    int *val, int *val2)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret = 0;
	__be32 tmp = 0;
	s32 adc_temp, comp_temp;

	/* set forced mode to trigger a single measurement */
	ret = bme680_set_mode(data, true);
	if (ret < 0)
		return ret;

	ret = regmap_bulk_read(data->regmap, BME680_REG_TEMP_MSB,
			       (u8 *) &tmp, 3);
	if (ret < 0) {
		dev_err(dev, "failed to read temperature\n");
		return ret;
	}

	adc_temp = be32_to_cpu(tmp) >> 12;
	if (adc_temp == BME680_MEAS_SKIPPED) {
		/* reading was skipped */
		dev_err(dev, "reading temperature skipped\n");
		return -EINVAL;
	}
	comp_temp = bme680_compensate_temp(data, adc_temp);
	/*
	 * val might be NULL if we're called by the read_press/read_humid
	 * routine which is callled to get t_fine value used in
	 * compensate_press/compensate_humid to get compensated
	 * pressure/humidity readings.
	 */
	if (val && val2) {
		*val = comp_temp;
		*val2 = 100;
		return IIO_VAL_FRACTIONAL;
	}

	return 0;
}

/* Outputs pressure measurement in hPa */
static int bme680_read_press(struct bme680_data *data,
			     int *val, int *val2)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;
	__be32 tmp = 0;
	s32 adc_press;
	u32 comp_press;

	/* Read and compensate temperature to get a reading of t_fine. */
	ret = bme680_read_temp(data, NULL, NULL);
	if (ret < 0)
		return ret;

	ret = regmap_bulk_read(data->regmap, BME680_REG_PRESS_MSB,
			       (u8 *) &tmp, 3);
	if (ret < 0) {
		dev_err(dev, "failed to read pressure\n");
		return ret;
	}

	adc_press = be32_to_cpu(tmp) >> 12;
	if (adc_press == BME680_MEAS_SKIPPED) {
		/* reading was skipped */
		dev_err(dev, "reading pressure skipped\n");
		return -EINVAL;
	}
	comp_press = bme680_compensate_press(data, adc_press);

	*val = comp_press;
	*val2 = 100;
	return IIO_VAL_FRACTIONAL;
}

/* Outputs humidity measurement in %r.H */
static int bme680_read_humid(struct bme680_data *data,
			     int *val, int *val2)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;
	__be16 tmp = 0;
	s32 adc_humidity;
	u32 comp_humidity;

	/* Read and compensate temperature so we get a reading of t_fine. */
	ret = bme680_read_temp(data, NULL, NULL);
	if (ret < 0)
		return ret;

	ret = regmap_bulk_read(data->regmap, BM6880_REG_HUMIDITY_MSB,
			       (u8 *) &tmp, 2);
	if (ret < 0) {
		dev_err(dev, "failed to read humidity\n");
		return ret;
	}

	adc_humidity = be16_to_cpu(tmp);
	if (adc_humidity == BME680_MEAS_SKIPPED) {
		/* reading was skipped */
		dev_err(dev, "reading humidity skipped\n");
		return -EINVAL;
	}
	comp_humidity = bme680_compensate_humid(data, adc_humidity);

	*val = comp_humidity;
	*val2 = 1000;
	return IIO_VAL_FRACTIONAL;
}

/* Outputs gas measurement in ohm */
static int bme680_read_gas(struct bme680_data *data,
			   int *val)
{
	struct device *dev = regmap_get_device(data->regmap);
	int ret;
	__be16 tmp = 0;
	unsigned int check;
	u16 adc_gas_res;
	u8 gas_range;
	u32 comp_gas;

	/* Set heater settings */
	ret = bme680_gas_config(data);
	if (ret < 0) {
		dev_err(dev, "failed to set gas config\n");
		return ret;
	}

	/* set forced mode to trigger a single measurement */
	ret = bme680_set_mode(data, true);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, BME680_REG_MEAS_STAT_0, &check);
	if (check & BME680_GAS_MEAS_BIT) {
		dev_err(dev, "gas measurement incomplete\n");
		return -EBUSY;
	}

	ret = regmap_read(data->regmap, BME680_REG_GAS_R_LSB, &check);
	if (ret < 0) {
		dev_err(dev,
			"failed to read gas_r_lsb register\n");
		return ret;
	}

	if ((check & BME680_GAS_STAB_BIT) == 0) {
		dev_err(dev,
			"heater failed to reach the target temperature\n");
		return -EINVAL;
	}

	ret = regmap_bulk_read(data->regmap, BME680_REG_GAS_MSB,
			       (u8 *) &tmp, 2);
	if (ret < 0) {
		dev_err(dev, "failed to read gas resistance\n");
		return ret;
	}

	gas_range = check & BME680_GAS_RANGE_MASK;
	adc_gas_res = be16_to_cpu(tmp) >> BME680_ADC_GAS_RES_SHIFT;
	comp_gas = bme680_compensate_gas(data, adc_gas_res, gas_range);

	*val = comp_gas;
	return IIO_VAL_INT;
}

static int bme680_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bme680_data *data = iio_priv(indio_dev);
	int ret = -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->type) {
		case IIO_TEMP:
			return bme680_read_temp(data, val, val2);
		case IIO_PRESSURE:
			return bme680_read_press(data, val, val2);
		case IIO_HUMIDITYRELATIVE:
			return bme680_read_humid(data, val, val2);
		case IIO_RESISTANCE:
			return bme680_read_gas(data, val);
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		switch (chan->type) {
		case IIO_TEMP:
			*val = 1 << data->oversampling_temp;
			return IIO_VAL_INT;
		case IIO_PRESSURE:
			*val = 1 << data->oversampling_press;
			return IIO_VAL_INT;
		case IIO_HUMIDITYRELATIVE:
			*val = 1 << data->oversampling_humid;
			return IIO_VAL_INT;
		default:
			break;
		}
	}

	return ret;
}

static int bme680_write_oversampling_ratio_temp(struct bme680_data *data,
						int val)
{
	const int *avail = bme680_oversampling_avail;
	const int n = ARRAY_SIZE(bme680_oversampling_avail);
	int i;

	for (i = 0; i < n; ++i) {
		if (avail[i] == val) {
			data->oversampling_temp = ilog2(val);

			return bme680_chip_config(data);
		}
	}

	return -EINVAL;
}

static int bme680_write_oversampling_ratio_press(struct bme680_data *data,
						 int val)
{
	const int *avail = bme680_oversampling_avail;
	const int n = ARRAY_SIZE(bme680_oversampling_avail);
	int i;

	for (i = 0; i < n; ++i) {
		if (avail[i] == val) {
			data->oversampling_press = ilog2(val);

			return bme680_chip_config(data);
		}
	}

	return -EINVAL;
}

static int bme680_write_oversampling_ratio_humid(struct bme680_data *data,
						 int val)
{
	const int *avail = bme680_oversampling_avail;
	const int n = ARRAY_SIZE(bme680_oversampling_avail);
	int i;

	for (i = 0; i < n; ++i) {
		if (avail[i] == val) {
			data->oversampling_humid = ilog2(val);

			return bme680_chip_config(data);
		}
	}

	return -EINVAL;
}

static int bme680_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct bme680_data *data = iio_priv(indio_dev);
	int ret = -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		switch (chan->type) {
		case IIO_TEMP:
			return bme680_write_oversampling_ratio_temp(data, val);
		case IIO_PRESSURE:
			return bme680_write_oversampling_ratio_press(data, val);
		case IIO_HUMIDITYRELATIVE:
			return bme680_write_oversampling_ratio_humid(data, val);
		default:
			break;
		}
	break;
	}

	return ret;
}

static const char bme680_oversampling_ratio_show[] = "1 2 4 8 16";

static IIO_CONST_ATTR(oversampling_ratio_available,
		      bme680_oversampling_ratio_show);

static struct attribute *bme680_attributes[] = {
	&iio_const_attr_oversampling_ratio_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group bme680_attribute_group = {
	.attrs = bme680_attributes,
};

static const struct iio_info bme680_info = {
	.read_raw = &bme680_read_raw,
	.write_raw = &bme680_write_raw,
	.attrs = &bme680_attribute_group,
};

static const char *bme680_match_acpi_device(struct device *dev)
{
	const struct acpi_device_id *id;

	id = acpi_match_device(dev->driver->acpi_match_table, dev);
	if (!id)
		return NULL;

	return dev_name(dev);
}

static void bme680_core_remove(void *arg)
{
	struct bme680_data *data = iio_priv(arg);

	bme680_set_mode(data, false);
	iio_device_unregister(arg);
}

int bme680_core_probe(struct device *dev, struct regmap *regmap,
		      const char *name)
{
	struct iio_dev *indio_dev;
	struct bme680_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	ret = devm_add_action(dev, bme680_core_remove, indio_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register remove action\n");
		return ret;
	}

	if (!name && ACPI_HANDLE(dev))
		name = bme680_match_acpi_device(dev);

	data = iio_priv(indio_dev);
	dev_set_drvdata(dev, indio_dev);
	data->regmap = regmap;
	indio_dev->dev.parent = dev;
	indio_dev->name = name;
	indio_dev->channels = bme680_channels;
	indio_dev->num_channels = ARRAY_SIZE(bme680_channels);
	indio_dev->info = &bme680_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	/* default values for the sensor */
	data->oversampling_humid = ilog2(2); /* 2X oversampling rate */
	data->oversampling_press = ilog2(4); /* 4X oversampling rate */
	data->oversampling_temp = ilog2(8);  /* 8X oversampling rate */
	data->heater_temp = 320; /* degree Celsius */
	data->heater_dur = 150;  /* milliseconds */

	ret = bme680_chip_config(data);
	if (ret < 0) {
		dev_err(dev, "failed to set chip_config data\n");
		return ret;
	}

	ret = bme680_gas_config(data);
	if (ret < 0) {
		dev_err(dev, "failed to set gas config data\n");
		return ret;
	}

	ret = bme680_read_calib(data, &data->bme680);
	if (ret < 0) {
		dev_err(dev,
			"failed to read calibration coefficients at probe\n");
		return ret;
	}

	return iio_device_register(indio_dev);
}
EXPORT_SYMBOL_GPL(bme680_core_probe);

MODULE_AUTHOR("Himanshu Jha <himanshujha199640@gmail.com>");
MODULE_DESCRIPTION("Bosch BME680 Driver");
MODULE_LICENSE("GPL v2");
