// SPDX-License-Identifier: GPL-2.0+
/*
 * PNI RM3100 9-axis geomagnetic sensor driver core.
 *
 * Copyright (C) 2018 Song Qiang <songqiang1304521@gmail.com>
 *
 * User Manual available at
 * <https://www.pnicorp.com/download/rm3100-user-manual/>
 *
 * TODO: Scale channel, event generaton, pm.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <linux/iio/iio.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/buffer.h>
#include <linux/iio/kfifo_buf.h>

#include "rm3100.h"

static const struct regmap_range rm3100_readable_ranges[] = {
		regmap_reg_range(RM_W_REG_START, RM_W_REG_END),
};

const struct regmap_access_table rm3100_readable_table = {
		.yes_ranges = rm3100_readable_ranges,
		.n_yes_ranges = ARRAY_SIZE(rm3100_readable_ranges),
};

static const struct regmap_range rm3100_writable_ranges[] = {
		regmap_reg_range(RM_R_REG_START, RM_R_REG_END),
};

const struct regmap_access_table rm3100_writable_table = {
		.yes_ranges = rm3100_writable_ranges,
		.n_yes_ranges = ARRAY_SIZE(rm3100_writable_ranges),
};

static const struct regmap_range rm3100_volatile_ranges[] = {
		regmap_reg_range(RM_V_REG_START, RM_V_REG_END),
};

const struct regmap_access_table rm3100_volatile_table = {
		.yes_ranges = rm3100_volatile_ranges,
		.n_yes_ranges = ARRAY_SIZE(rm3100_volatile_ranges),
};

static irqreturn_t rm3100_measurement_irq_handler(int irq, void *d)
{
	struct rm3100_data *data = d;

	complete(&data->measuring_done);

	return IRQ_HANDLED;
}

static int rm3100_wait_measurement(struct rm3100_data *data)
{
	struct regmap *regmap = data->regmap;
	unsigned int val;
	u16 tries = 20;
	int ret;

	/* A read cycle of 400kbits i2c bus is about 20us, plus the time
	 * used for schduling, a read cycle of fast mode of this device
	 * can reach 1.7ms, it may be possible for data arrives just
	 * after we check the RM_REG_STATUS. In this case, irq_handler is
	 * called before measuring_done is reinitialized, it will wait
	 * forever for a data that has already been ready.
	 * Reinitialize measuring_done before looking up makes sure we
	 * will always capture interrupt no matter when it happened.
	 */
	if (data->use_interrupt)
		reinit_completion(&data->measuring_done);

	ret = regmap_read(regmap, RM_REG_STATUS, &val);
	if (ret < 0)
		return ret;

	if ((val & RM_STATUS_DRDY) != RM_STATUS_DRDY) {
		if (data->use_interrupt) {
			ret = wait_for_completion_timeout(&data->measuring_done,
				msecs_to_jiffies(data->conversion_time));
			if (!ret)
				return -ETIMEDOUT;
		} else {
			do {
				ret = regmap_read(regmap, RM_REG_STATUS, &val);
				if (ret < 0)
					return ret;

				if (val & RM_STATUS_DRDY)
					break;

				usleep_range(1000, 5000);
			} while (--tries);
			if (!tries)
				return -ETIMEDOUT;
		}
	}
	return 0;
}

static int rm3100_read_mag(struct rm3100_data *data, int idx, int *val)
{
	struct regmap *regmap = data->regmap;
	u8 buffer[3];
	int ret;

	mutex_lock(&data->lock);
	ret = rm3100_wait_measurement(data);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}

	ret = regmap_bulk_read(regmap, RM_REG_MX2 + 3 * idx, buffer, 3);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;

	*val = le32_to_cpu((buffer[0] << 16) + (buffer[1] << 8) + buffer[2]);
	*val = sign_extend32(*val, 23);

	return IIO_VAL_INT;
}

#define RM_CHANNEL(axis, idx)					\
	{								\
		.type = IIO_MAGN,					\
		.modified = 1,						\
		.channel2 = IIO_MOD_##axis,				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
		.scan_index = idx,					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 24,					\
			.storagebits = 32,				\
			.shift = 8,					\
			.endianness = IIO_LE,				\
		},							\
	}

static const struct iio_chan_spec rm3100_channels[] = {
	RM_CHANNEL(X, 0),
	RM_CHANNEL(Y, 1),
	RM_CHANNEL(Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const unsigned long rm3100_scan_masks[] = {GENMASK(2, 0), 0};

#define RM_SAMP_NUM	14

/* Frequency : rm3100_samp_rates[][0].rm3100_samp_rates[][1]Hz.
 * Time between reading: rm3100_sam_rates[][2]ms (The first on is actially 1.7).
 */
static const int rm3100_samp_rates[RM_SAMP_NUM][3] = {
	{600, 0, 2}, {300, 0, 3}, {150, 0, 7}, {75, 0, 13}, {37, 0, 27},
	{18, 0, 55}, {9, 0, 110}, {4, 500000, 220}, {2, 300000, 440},
	{1, 200000, 800}, {0, 600000, 1600}, {0, 300000, 3300},
	{0, 15000, 6700},  {0, 75000, 13000}
};

static int rm3100_get_samp_freq(struct rm3100_data *data, int *val, int *val2)
{
	int ret;
	int tmp;

	ret = regmap_read(data->regmap, RM_REG_TMRC, &tmp);
	if (ret < 0)
		return ret;
	*val = rm3100_samp_rates[tmp-RM_TMRC_OFFSET][0];
	*val2 = rm3100_samp_rates[tmp-RM_TMRC_OFFSET][1];

	return IIO_VAL_INT_PLUS_MICRO;
}

static int rm3100_set_samp_freq(struct rm3100_data *data, int val, int val2)
{
	struct regmap *regmap = data->regmap;
	int cycle_count;
	int ret;
	int i;

	/* All cycle count registers use the same value. */
	ret = regmap_read(regmap, RM_REG_CCXL, &cycle_count);
	if (cycle_count < 0)
		return cycle_count;

	for (i = 0; i < RM_SAMP_NUM; i++) {
		if (val == rm3100_samp_rates[i][0] &&
			val2 == rm3100_samp_rates[i][1])
			break;
	}

	if (i != RM_SAMP_NUM) {
		mutex_lock(&data->lock);
		ret = regmap_write(regmap, RM_REG_TMRC, i + RM_TMRC_OFFSET);
		if (ret < 0)
			return ret;

		/* Checking if cycle count registers need changing. */
		if (val == 600 && cycle_count == 200) {
			for (i = 0; i < 3; i++) {
				regmap_write(regmap, RM_REG_CCXL + 2 * i, 100);
				if (ret < 0)
					return ret;
			}
		} else if (val != 600 && cycle_count == 100) {
			for (i = 0; i < 3; i++) {
				regmap_write(regmap, RM_REG_CCXL + 2 * i, 200);
				if (ret < 0)
					return ret;
			}
		}
		/* Writing TMRC registers requires CMM reset. */
		ret = regmap_write(regmap, RM_REG_CMM, 0);
		if (ret < 0)
			return ret;
		ret = regmap_write(regmap, RM_REG_CMM, RM_CMM_PMX |
			RM_CMM_PMY | RM_CMM_PMZ | RM_CMM_START);
		if (ret < 0)
			return ret;
		mutex_unlock(&data->lock);

		data->conversion_time = rm3100_samp_rates[i][2] + 3000;
		return 0;
	}
	return -EINVAL;
}

static int rm3100_read_raw(struct iio_dev *indio_dev,
			   const struct iio_chan_spec *chan,
			   int *val, int *val2, long mask)
{
	struct rm3100_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret < 0)
			return ret;
		ret = rm3100_read_mag(data, chan->scan_index, val);
		iio_device_release_direct_mode(indio_dev);

		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ret = rm3100_get_samp_freq(data, val, val2);
	default:
		return -EINVAL;
	}
}

static int rm3100_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct rm3100_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = rm3100_set_samp_freq(data, val, val2);
		if (ret < 0)
			return ret;
		return 0;
	default:
		return -EINVAL;
	}

}

static const struct iio_info rm3100_info = {
	.read_raw = rm3100_read_raw,
	.write_raw = rm3100_write_raw,
};

static irqreturn_t rm3100_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct rm3100_data *data = iio_priv(indio_dev);
	struct regmap *regmap = data->regmap;
	u8 *buffer;
	int ret;
	int i;

	buffer = devm_kzalloc(data->dev, indio_dev->scan_bytes, GFP_KERNEL);
	if (!buffer)
		goto done;

	mutex_lock(&data->lock);
	ret = rm3100_wait_measurement(data);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		goto done;
	}

	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(regmap, RM_REG_MX2 + 3 * i,
				buffer + 4 * i, 3);
		if (ret < 0)
			return ret;
	}
	mutex_unlock(&data->lock);

	iio_push_to_buffers_with_timestamp(indio_dev, buffer,
			iio_get_time_ns(indio_dev));
done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

int rm3100_common_probe(struct device *dev, struct regmap *regmap, int irq)
{
	struct iio_dev *indio_dev;
	struct rm3100_data *data;
	int tmp;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	dev_set_drvdata(dev, indio_dev);
	data->dev = dev;
	data->regmap = regmap;

	mutex_init(&data->lock);

	indio_dev->dev.parent = dev;
	indio_dev->name = "rm3100";
	indio_dev->info = &rm3100_info;
	indio_dev->channels = rm3100_channels;
	indio_dev->num_channels = ARRAY_SIZE(rm3100_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->available_scan_masks = rm3100_scan_masks;

	if (!irq)
		data->use_interrupt = false;
	else {
		data->use_interrupt = true;
		ret = devm_request_irq(dev,
			irq,
			rm3100_measurement_irq_handler,
			IRQF_TRIGGER_RISING,
			indio_dev->name,
			data);
		if (ret < 0) {
			dev_err(dev,
			"request irq line failed.");
			return -ret;
		}
		init_completion(&data->measuring_done);
	}

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
					rm3100_trigger_handler, NULL);
	if (ret < 0)
		return ret;

	/* 3sec more wait time. */
	ret = regmap_read(data->regmap, RM_REG_TMRC, &tmp);
	data->conversion_time = rm3100_samp_rates[tmp-RM_TMRC_OFFSET][2] + 3000;

	/* Starting all channels' conversion. */
	ret = regmap_write(regmap, RM_REG_CMM,
		RM_CMM_PMX | RM_CMM_PMY | RM_CMM_PMZ | RM_CMM_START);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL(rm3100_common_probe);

int rm3100_common_remove(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct rm3100_data *data = iio_priv(indio_dev);
	struct regmap *regmap = data->regmap;

	regmap_write(regmap, RM_REG_CMM, 0x00);

	return 0;
}
EXPORT_SYMBOL(rm3100_common_remove);

MODULE_AUTHOR("Song Qiang <songqiang1304521@gmail.com>");
MODULE_DESCRIPTION("PNI RM3100 9-axis magnetometer i2c driver");
MODULE_LICENSE("GPL v2");
