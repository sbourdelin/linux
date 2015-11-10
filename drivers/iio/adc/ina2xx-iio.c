/*
 * INA2XX Current and Power Monitors
 *
 * Copyright 2015 Baylibre SAS.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on linux/drivers/iio/adc/ad7291.c
 * Copyright 2010-2011 Analog Devices Inc.
 *
 * Based on linux/drivers/hwmon/ina2xx.c
 * Copyright 2012 Lothar Felten <l-felten@ti.com>
 *
 * Licensed under the GPL-2 or later.
 */
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/iio/kfifo_buf.h>

#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/platform_data/ina2xx.h>

#include <linux/util_macros.h>

/*
 * INA2XX registers definition
 */
/* common register definitions */
#define INA2XX_CONFIG                   0x00
#define INA2XX_SHUNT_VOLTAGE            0x01	/* readonly */
#define INA2XX_BUS_VOLTAGE              0x02	/* readonly */
#define INA2XX_POWER                    0x03	/* readonly */
#define INA2XX_CURRENT                  0x04	/* readonly */
#define INA2XX_CALIBRATION              0x05

/* register count */
#define INA219_REGISTERS                6
#define INA226_REGISTERS                8
#define INA2XX_MAX_REGISTERS            8

/* settings - depend on use case */
#define INA219_CONFIG_DEFAULT           0x399F	/* PGA=8 */
#define INA226_CONFIG_DEFAULT           0x4327
#define INA226_DEFAULT_AVG              4
#define INA226_DEFAULT_FREQ             454

#define INA2XX_RSHUNT_DEFAULT           10000

/* bit mask for reading the averaging setting in the configuration register */
#define INA226_AVG_RD_MASK              0x0E00
#define INA226_READ_AVG(reg)            (((reg) & INA226_AVG_RD_MASK) >> 9)
#define INA226_SHIFT_AVG(val)           ((val) << 9)

#define INA226_SFREQ_RD_MASK            0x01f8

static struct regmap_config ina2xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
};

enum ina2xx_ids { ina219, ina226 };

struct ina2xx_config {
	u16 config_default;
	int calibration_factor;
	int registers;
	int shunt_div;
	int bus_voltage_shift;
	int bus_voltage_lsb;	/* uV */
	int power_lsb;		/* uW */
};

struct ina2xx_chip_info {
	struct iio_dev *indio_dev;
	struct task_struct *task;
	const struct ina2xx_config *config;
	struct mutex state_lock;
	long rshunt;
	int avg;
	int freq;
	int period_us;
	struct regmap *regmap;
};

static const struct ina2xx_config ina2xx_config[] = {
	[ina219] = {
		    .config_default = INA219_CONFIG_DEFAULT,
		    .calibration_factor = 40960000,
		    .registers = INA219_REGISTERS,
		    .shunt_div = 100,
		    .bus_voltage_shift = 3,
		    .bus_voltage_lsb = 4000,
		    .power_lsb = 20000,
		    },
	[ina226] = {
		    .config_default = INA226_CONFIG_DEFAULT,
		    .calibration_factor = 5120000,
		    .registers = INA226_REGISTERS,
		    .shunt_div = 400,
		    .bus_voltage_shift = 0,
		    .bus_voltage_lsb = 1250,
		    .power_lsb = 25000,
		    },
};

static int ina2xx_get_value(struct ina2xx_chip_info *chip, u8 reg,
			    unsigned int regval, int *val, int *uval)
{
	*val = 0;

	switch (reg) {
	case INA2XX_SHUNT_VOLTAGE:
		/* signed register */
		*uval = DIV_ROUND_CLOSEST((s16) regval,
					  chip->config->shunt_div);
		*val = *uval/1000000;
		*uval = *uval - *val*1000000;
		return IIO_VAL_INT_PLUS_MICRO;

	case INA2XX_BUS_VOLTAGE:
		*uval = (regval >> chip->config->bus_voltage_shift)
			* chip->config->bus_voltage_lsb;
		*val = *uval/1000000;
		*uval = *uval - *val*1000000;
		return IIO_VAL_INT_PLUS_MICRO;

	case INA2XX_POWER:
		*uval = regval * chip->config->power_lsb;
		*val = *uval/1000000;
		*uval = *uval - *val*1000000;
		return IIO_VAL_INT_PLUS_MICRO;

	case INA2XX_CURRENT:
		/* signed register, LSB=1mA (selected), in mA */
		*uval = (s16) regval * 1000;
		return IIO_VAL_INT_PLUS_MICRO;

	case INA2XX_CALIBRATION:
		*val = DIV_ROUND_CLOSEST(chip->config->calibration_factor,
					regval);
		return IIO_VAL_INT;

	default:
		/* programmer goofed */
		WARN_ON_ONCE(1);
	}
	return -EINVAL;
}

static int ina2xx_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	int ret;
	struct ina2xx_chip_info *chip = iio_priv(indio_dev);
	unsigned int regval;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(chip->regmap, chan->address, &regval);
		if (ret < 0)
			return ret;

		return ina2xx_get_value(chip, chan->address, regval, val, val2);

	case IIO_CHAN_INFO_AVERAGE_RAW:
		*val = chip->avg;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_CALIBSCALE:
		ret = regmap_read(chip->regmap, INA2XX_CALIBRATION, &regval);
		if (ret < 0)
			return ret;

		return ina2xx_get_value(chip, INA2XX_CALIBRATION, regval,
					val, val2);

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = chip->freq;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}

	return 0;
}

static int ina2xx_calibrate(struct ina2xx_chip_info *chip)
{
	u16 val = DIV_ROUND_CLOSEST(chip->config->calibration_factor,
				    chip->rshunt);

	return regmap_write(chip->regmap, INA2XX_CALIBRATION, val);
}


/*
 * Available averaging rates for ina226. The indices correspond with
 * the bit values expected by the chip (according to the ina226 datasheet,
 * table 3 AVG bit settings, found at
 * http://www.ti.com/lit/ds/symlink/ina226.pdf.
 */
static const int ina226_avg_tab[] = { 1, 4, 16, 64, 128, 256, 512, 1024 };

static unsigned int ina226_set_average(struct ina2xx_chip_info *chip,
				       unsigned int val,
				       unsigned int *config)
{
	int bits;

	if (val > 1024 || val < 1)
		return -EINVAL;

	bits = find_closest(val, ina226_avg_tab,
			    ARRAY_SIZE(ina226_avg_tab));

	chip->avg = ina226_avg_tab[bits];

	*config &= ~INA226_AVG_RD_MASK;
	*config |= INA226_SHIFT_AVG(bits) & INA226_AVG_RD_MASK;

	return 0;
}

/*
 * Conversion times in uS
 */
static const int ina226_conv_time_tab[] = { 140, 204, 332, 588, 1100,
	2116, 4156, 8244};

static unsigned int ina226_set_frequency(struct ina2xx_chip_info *chip,
					 unsigned int val,
					 unsigned int *config)
{
	int bits;

	if (val > 3550  || val < 50)
		return -EINVAL;

	/* integration time in uS, for both voltage channels */
	val = DIV_ROUND_CLOSEST(1000000, 2 * val);

	bits = find_closest(val, ina226_conv_time_tab,
			    ARRAY_SIZE(ina226_conv_time_tab));

	chip->period_us = 2 * ina226_conv_time_tab[bits];

	chip->freq = DIV_ROUND_CLOSEST(1000000, chip->period_us);

	*config &= ~INA226_SFREQ_RD_MASK;
	*config |= (bits << 3) | (bits << 6);

	return 0;
}


static int ina2xx_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct ina2xx_chip_info *chip = iio_priv(indio_dev);
	int ret = 0;
	unsigned int config, tmp;

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	mutex_lock(&chip->state_lock);

	ret = regmap_read(chip->regmap, INA2XX_CONFIG, &config);
	if (ret < 0)
		goto _err;

	tmp = config;

	switch (mask) {
	case IIO_CHAN_INFO_AVERAGE_RAW:

		ret = ina226_set_average(chip, val, &tmp);
		break;

	case IIO_CHAN_INFO_SAMP_FREQ:

		ret = ina226_set_frequency(chip, val, &tmp);
		break;

	default:
		ret = -EINVAL;
	}

	if (!ret && (tmp != config))
		ret = regmap_write(chip->regmap, INA2XX_CONFIG, config);
_err:
	mutex_unlock(&chip->state_lock);

	return ret;
}

#define INA2XX_CHAN(_type, _index, _address) { \
	.type = _type, \
	.address = _address, \
	.indexed = 1, \
	.channel = (_index), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_dir = BIT(IIO_CHAN_INFO_AVERAGE_RAW) | \
					BIT(IIO_CHAN_INFO_SAMP_FREQ) | \
				   BIT(IIO_CHAN_INFO_CALIBSCALE), \
	.scan_index = (_index), \
	.scan_type = { \
		.sign = 'u', \
		.realbits = 16, \
		.storagebits = 16, \
		.shift = 0, \
	.endianness = IIO_BE, \
	} \
}

static const struct iio_chan_spec ina2xx_channels[] = {
	INA2XX_CHAN(IIO_VOLTAGE, 0, INA2XX_SHUNT_VOLTAGE),
	INA2XX_CHAN(IIO_VOLTAGE, 1, INA2XX_BUS_VOLTAGE),
	INA2XX_CHAN(IIO_CURRENT, 2, INA2XX_CURRENT),
	INA2XX_CHAN(IIO_POWER, 3, INA2XX_POWER),
	IIO_CHAN_SOFT_TIMESTAMP(4),
};

/*
 * return uS until next due sampling.
 */

static s64 prev_ns;

static int ina2xx_work_buffer(struct ina2xx_chip_info *chip)
{
	unsigned short data[8];
	struct iio_dev *indio_dev = chip->indio_dev;
	int bit, ret = 0, i = 0;
	unsigned long buffer_us = 0, elapsed_us = 0;
	s64 time_a, time_b;

	time_a = iio_get_time_ns();

	/* Single register reads: bulk_read will not work with ina226
	 * as there is no auto-increment of the address register for
	 * data length longer than 16bits.
	 */
	for_each_set_bit(bit, indio_dev->active_scan_mask,
			 indio_dev->masklength) {
		unsigned int val;

		ret = regmap_read(chip->regmap,
				  INA2XX_SHUNT_VOLTAGE + bit, &val);
		if (ret < 0)
			goto _err;

		data[i++] = val;
	}

	time_b = iio_get_time_ns();

	iio_push_to_buffers_with_timestamp(chip->indio_dev,
					   (unsigned int *)data, time_a);

	buffer_us = (unsigned long)(time_b - time_a) / 1000;
	elapsed_us = (unsigned long)(time_a - prev_ns) / 1000;

	trace_printk("uS: elapsed: %lu, buf: %lu\n", elapsed_us, buffer_us);

	prev_ns = time_a;
_err:
	return buffer_us;
};

static int ina2xx_capture_thread(void *data)
{
	struct ina2xx_chip_info *chip = (struct ina2xx_chip_info *)data;
	unsigned int sampling_us = chip->period_us * chip->avg;
	unsigned long buffer_us;

	do {
		buffer_us = ina2xx_work_buffer(chip);

		if (sampling_us > buffer_us)
			udelay(sampling_us - buffer_us);

	} while (!kthread_should_stop());

	chip->task = NULL;

	return 0;
}

int ina2xx_buffer_enable(struct iio_dev *indio_dev)
{
	struct ina2xx_chip_info *chip = iio_priv(indio_dev);
	unsigned int sampling_us = chip->period_us * chip->avg;

	trace_printk("Enabling buffer w/ scan_mask %02x, freq = %d, avg =%u\n",
		     (unsigned int)(*indio_dev->active_scan_mask), chip->freq,
		     chip->avg);

	trace_printk("Expected work period is %u us\n", sampling_us);

	prev_ns = iio_get_time_ns();

	chip->task = kthread_run(ina2xx_capture_thread, (void *)chip,
				 "ina2xx-%uus", sampling_us);

	return PTR_ERR_OR_ZERO(chip->task);
}

int ina2xx_buffer_disable(struct iio_dev *indio_dev)
{
	struct ina2xx_chip_info *chip = iio_priv(indio_dev);

	if (chip->task)
		kthread_stop(chip->task);

	return 0;
}

static const struct iio_buffer_setup_ops ina2xx_setup_ops = {
	.postenable = &ina2xx_buffer_enable,
	.postdisable = &ina2xx_buffer_disable,
};

static int ina2xx_debug_reg(struct iio_dev *indio_dev,
			    unsigned reg, unsigned writeval, unsigned *readval)
{
	struct ina2xx_chip_info *chip = iio_priv(indio_dev);

	if (!readval)
		return regmap_write(chip->regmap, reg, writeval);

	return regmap_read(chip->regmap, reg, readval);
}

static const struct iio_info ina2xx_info = {
	.debugfs_reg_access = &ina2xx_debug_reg,
	.read_raw = &ina2xx_read_raw,
	.write_raw = &ina2xx_write_raw,
	.driver_module = THIS_MODULE,
};

/*
* Initialize the configuration and calibration registers.
*/
static int ina2xx_init(struct ina2xx_chip_info *chip, unsigned int config)
{
	int ret = regmap_write(chip->regmap, INA2XX_CONFIG, config);

	if (ret < 0)
		return ret;
	/*
	 * Set current LSB to 1mA, shunt is in uOhms
	 * (equation 13 in datasheet).
	 */
	return ina2xx_calibrate(chip);
}

static int ina2xx_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ina2xx_chip_info *chip;
	struct iio_dev *indio_dev;
	struct iio_buffer *buffer;
	int ret;
	unsigned int val;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);
	chip->indio_dev = indio_dev;

	/* set the device type */
	chip->config = &ina2xx_config[id->driver_data];

	if (of_property_read_u32(client->dev.of_node,
				 "shunt-resistor", &val) < 0) {
		struct ina2xx_platform_data *pdata =
		    dev_get_platdata(&client->dev);

		if (pdata)
			val = pdata->shunt_uohms;
		else
			val = INA2XX_RSHUNT_DEFAULT;
	}

	if (val <= 0 || val > chip->config->calibration_factor)
		return -ENODEV;

	chip->rshunt = val;

	ina2xx_regmap_config.max_register = chip->config->registers;

	mutex_init(&chip->state_lock);

	/* this is only used for device removal purposes */
	i2c_set_clientdata(client, indio_dev);

	indio_dev->name = id->name;
	indio_dev->channels = ina2xx_channels;
	indio_dev->num_channels = ARRAY_SIZE(ina2xx_channels);

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &ina2xx_info;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;

	chip->regmap = devm_regmap_init_i2c(client, &ina2xx_regmap_config);
	if (IS_ERR(chip->regmap)) {
		dev_err(&client->dev, "failed to allocate register map\n");
		return PTR_ERR(chip->regmap);
	}

	/* Patch the current config register with default. */
	val = chip->config->config_default;
	if (id->driver_data == ina226) {
		ina226_set_average(chip, INA226_DEFAULT_AVG, &val);
		ina226_set_frequency(chip, INA226_DEFAULT_FREQ, &val);
	}

	ret = ina2xx_init(chip, val);
	if (ret < 0) {
		dev_err(&client->dev, "error configuring the device: %d\n",
			ret);
		return -ENODEV;
	}

	buffer = devm_iio_kfifo_allocate(&indio_dev->dev);
	if (!buffer)
		return -ENOMEM;

	indio_dev->setup_ops = &ina2xx_setup_ops;

	iio_device_attach_buffer(indio_dev, buffer);

	return iio_device_register(indio_dev);
}

static int ina2xx_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);

	return 0;
}

static const struct i2c_device_id ina2xx_id[] = {
	{"ina219", ina219},
	{"ina220", ina219},
	{"ina226", ina226},
	{"ina230", ina226},
	{"ina231", ina226},
	{}
};

MODULE_DEVICE_TABLE(i2c, ina2xx_id);

static struct i2c_driver ina2xx_driver = {
	.driver = {
		   .name = KBUILD_MODNAME,
		   },
	.probe = ina2xx_probe,
	.remove = ina2xx_remove,
	.id_table = ina2xx_id,
};

module_i2c_driver(ina2xx_driver);

MODULE_AUTHOR("Marc Titinger <marc.titinger@baylibre.com>");
MODULE_DESCRIPTION("Texas Instruments INA2XX ADC driver");
MODULE_LICENSE("GPL v2");
