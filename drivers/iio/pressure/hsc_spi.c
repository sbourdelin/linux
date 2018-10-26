// SPDX-License-Identifier: GPL-2.0

/*
 * hsc_spi.c - Driver for Honeywell HSC pressure sensors with
 *             SPI interface
 *
 * Copyright (c) 2018 Carlos Iglesias <carlos.iglesias@emutex.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/spi/spi.h>

#define HSC_MAX_SPI_FREQ_HZ	400000

#define HSC_TEMP_BITS		11
#define HSC_PRESS_BITS		14
#define HSC_TEMP_MASK		(0x7FF)
#define HSC_TEMP_SHIFT		(5)

#define HSC_STATUS_S0		BIT(14)
#define HSC_STATUS_S1		BIT(15)
#define HSC_STATUS_MSK		((HSC_STATUS_S0) | (HSC_STATUS_S1))
#define HSC_STATUS_CMD		HSC_STATUS_S0
#define HSC_STATUS_STALE	HSC_STATUS_S1
#define HSC_STATUS_DIAG		((HSC_STATUS_S0) | (HSC_STATUS_S1))

static inline int hsc_status_error(struct device dev, int val)
{
	int st_check = val & HSC_STATUS_MSK;

	if (st_check) {
		switch (st_check) {
		case HSC_STATUS_CMD:
			dev_warn(&dev, "%s:Device in COMMAND MODE\n",
				 __func__);
			return -EIO;
		case HSC_STATUS_STALE:
			dev_warn(&dev, "%s:Stale data - sampling too fast?\n",
				 __func__);
			return -EAGAIN;
		case HSC_STATUS_DIAG:
			dev_warn(&dev, "%s:Calibration signature changed\n",
				 __func__);
			return -EIO;
		default:
			dev_err(&dev, "%s:Invalid status code (%d)\n",
				__func__, st_check);
			return -EIO;
		}
	}

	return 0;
}

enum hsc_variant {
	/* Note: Only the absolute range sensors are supported */
	HSC001BAA, HSC001BAB, HSC001BAC, HSC001BAF,
	HSC1_6BAA, HSC1_6BAB, HSC1_6BAC, HSC1_6BAF,
	HSC2_5BAA, HSC2_5BAB, HSC2_5BAC, HSC2_5BAF,
	HSC004BAA, HSC004BAB, HSC004BAC, HSC004BAF,
	HSC006BAA, HSC006BAB, HSC006BAC, HSC006BAF,
	HSC010BAA, HSC010BAB, HSC010BAC, HSC010BAF,

	HSC100KAA, HSC100KAB, HSC100KAC, HSC100KAF,
	HSC160KAA, HSC160KAB, HSC160KAC, HSC160KAF,
	HSC250KAA, HSC250KAB, HSC250KAC, HSC250KAF,
	HSC400KAA, HSC400KAB, HSC400KAC, HSC400KAF,
	HSC600KAA, HSC600KAB, HSC600KAC, HSC600KAF,
	HSC001GAA, HSC001GAB, HSC001GAC, HSC001GAF,

	HSC015PAA, HSC015PAB, HSC015PAC, HSC015PAF,
	HSC030PAA, HSC030PAB, HSC030PAC, HSC030PAF,
	HSC060PAA, HSC060PAB, HSC060PAC, HSC060PAF,
	HSC100PAA, HSC100PAB, HSC100PAC, HSC100PAF,
	HSC150PAA, HSC150PAB, HSC150PAC, HSC150PAF,
};

enum hsc_meas_channel {
	HSC_CH_PRESSURE,
	HSC_CH_TEMPERATURE
};

struct hsc_config {
	int pmin;	/* Lower pressure limit */
	int pmax;	/* Upper pressure limit */
	int rmin;	/* Lower transfer function limit (%) */
	int rmax;	/* Upper transfer function limit (%) */
	int knum;	/* Pressure kPa conversion factor (numerator) */
	int kden;	/* Pressure kPa conversion factor /denominator) */
};

struct hsc_fract_val {
	int num;	/* numerator */
	int den;	/* denominator */
};

struct hsc_state {
	struct device *dev;
	struct spi_device *spi_dev;
	struct spi_transfer spi_xfer;
	struct spi_message spi_msg;
	__be16 rx_buf[2];

	/* Model-dependent values */
	struct hsc_fract_val scale;
	struct hsc_fract_val offset;
};

#define HSC_CONFIG(_pmin, _pmax, _rmin, _rmax, _knum, _kden) {	\
		.pmin = (_pmin),				\
		.pmax = (_pmax),				\
		.rmin = (_rmin),				\
		.rmax = (_rmax),				\
		.knum = (_knum),				\
		.kden = (_kden),				\
	}

static struct hsc_config hsc_cfg[] = {
	/* Absolute range, mbar */
	[HSC001BAA] = HSC_CONFIG(0, 1000, 10, 90, 1, 10),
	[HSC001BAB] = HSC_CONFIG(0, 1000, 5, 95, 1, 10),
	[HSC001BAC] = HSC_CONFIG(0, 1000, 5, 85, 1, 10),
	[HSC001BAF] = HSC_CONFIG(0, 1000, 4, 94, 1, 10),
	[HSC1_6BAA] = HSC_CONFIG(0, 1600, 10, 90, 1, 10),
	[HSC1_6BAB] = HSC_CONFIG(0, 1600, 5, 95, 1, 10),
	[HSC1_6BAC] = HSC_CONFIG(0, 1600, 5, 85, 1, 10),
	[HSC1_6BAF] = HSC_CONFIG(0, 1600, 4, 94, 1, 10),
	[HSC2_5BAA] = HSC_CONFIG(0, 2500, 10, 90, 1, 10),
	[HSC2_5BAB] = HSC_CONFIG(0, 2500, 5, 95, 1, 10),
	[HSC2_5BAC] = HSC_CONFIG(0, 2500, 5, 85, 1, 10),
	[HSC2_5BAF] = HSC_CONFIG(0, 2500, 4, 94, 1, 10),
	[HSC004BAA] = HSC_CONFIG(0, 4000, 10, 90, 1, 10),
	[HSC004BAB] = HSC_CONFIG(0, 4000, 5, 95, 1, 10),
	[HSC004BAC] = HSC_CONFIG(0, 4000, 5, 85, 1, 10),
	[HSC004BAF] = HSC_CONFIG(0, 4000, 4, 94, 1, 10),
	[HSC006BAA] = HSC_CONFIG(0, 6000, 10, 90, 1, 10),
	[HSC006BAB] = HSC_CONFIG(0, 6000, 5, 95, 1, 10),
	[HSC006BAC] = HSC_CONFIG(0, 6000, 5, 85, 1, 10),
	[HSC006BAF] = HSC_CONFIG(0, 6000, 4, 94, 1, 10),
	[HSC010BAA] = HSC_CONFIG(0, 10000, 10, 90, 1, 10),
	[HSC010BAB] = HSC_CONFIG(0, 10000, 5, 95, 1, 10),
	[HSC010BAC] = HSC_CONFIG(0, 10000, 5, 85, 1, 10),
	[HSC010BAF] = HSC_CONFIG(0, 10000, 4, 94, 1, 10),
	/* Absolute range, kPa */
	[HSC100KAA] = HSC_CONFIG(0, 100, 10, 90, 1, 1),
	[HSC100KAB] = HSC_CONFIG(0, 100, 5, 95, 1, 1),
	[HSC100KAC] = HSC_CONFIG(0, 100, 5, 85, 1, 1),
	[HSC100KAF] = HSC_CONFIG(0, 100, 4, 94, 1, 1),
	[HSC160KAA] = HSC_CONFIG(0, 160, 10, 90, 1, 1),
	[HSC160KAB] = HSC_CONFIG(0, 160, 5, 95, 1, 1),
	[HSC160KAC] = HSC_CONFIG(0, 160, 5, 85, 1, 1),
	[HSC160KAF] = HSC_CONFIG(0, 160, 4, 94, 1, 1),
	[HSC250KAA] = HSC_CONFIG(0, 250, 10, 90, 1, 1),
	[HSC250KAB] = HSC_CONFIG(0, 250, 5, 95, 1, 1),
	[HSC250KAC] = HSC_CONFIG(0, 250, 5, 85, 1, 1),
	[HSC250KAF] = HSC_CONFIG(0, 250, 4, 94, 1, 1),
	[HSC400KAA] = HSC_CONFIG(0, 400, 10, 90, 1, 1),
	[HSC400KAB] = HSC_CONFIG(0, 400, 5, 95, 1, 1),
	[HSC400KAC] = HSC_CONFIG(0, 400, 5, 85, 1, 1),
	[HSC400KAF] = HSC_CONFIG(0, 400, 4, 94, 1, 1),
	[HSC600KAA] = HSC_CONFIG(0, 600, 10, 90, 1, 1),
	[HSC600KAB] = HSC_CONFIG(0, 600, 5, 95, 1, 1),
	[HSC600KAC] = HSC_CONFIG(0, 600, 5, 85, 1, 1),
	[HSC600KAF] = HSC_CONFIG(0, 600, 4, 94, 1, 1),
	[HSC001GAA] = HSC_CONFIG(0, 1000, 10, 90, 1, 1),
	[HSC001GAB] = HSC_CONFIG(0, 1000, 5, 95, 1, 1),
	[HSC001GAC] = HSC_CONFIG(0, 1000, 5, 85, 1, 1),
	[HSC001GAF] = HSC_CONFIG(0, 1000, 4, 94, 1, 1),
	/* Absolute range, psi */
	[HSC015PAA] = HSC_CONFIG(0, 15, 10, 90, 6895, 1000),
	[HSC015PAB] = HSC_CONFIG(0, 15, 5, 95, 6895, 1000),
	[HSC015PAC] = HSC_CONFIG(0, 15, 5, 85, 6895, 1000),
	[HSC015PAF] = HSC_CONFIG(0, 15, 4, 94, 6895, 1000),
	[HSC030PAA] = HSC_CONFIG(0, 30, 10, 90, 6895, 1000),
	[HSC030PAB] = HSC_CONFIG(0, 30, 5, 95, 6895, 1000),
	[HSC030PAC] = HSC_CONFIG(0, 30, 5, 85, 6895, 1000),
	[HSC030PAF] = HSC_CONFIG(0, 30, 4, 94, 6895, 1000),
	[HSC060PAA] = HSC_CONFIG(0, 60, 10, 90, 6895, 1000),
	[HSC060PAB] = HSC_CONFIG(0, 60, 5, 95, 6895, 1000),
	[HSC060PAC] = HSC_CONFIG(0, 60, 5, 85, 6895, 1000),
	[HSC060PAF] = HSC_CONFIG(0, 60, 4, 94, 6895, 1000),
	[HSC100PAA] = HSC_CONFIG(0, 100, 10, 90, 6895, 1000),
	[HSC100PAB] = HSC_CONFIG(0, 100, 5, 95, 6895, 1000),
	[HSC100PAC] = HSC_CONFIG(0, 100, 5, 85, 6895, 1000),
	[HSC100PAF] = HSC_CONFIG(0, 100, 4, 94, 6895, 1000),
	[HSC150PAA] = HSC_CONFIG(0, 150, 10, 90, 6895, 1000),
	[HSC150PAB] = HSC_CONFIG(0, 150, 5, 95, 6895, 1000),
	[HSC150PAC] = HSC_CONFIG(0, 150, 5, 85, 6895, 1000),
	[HSC150PAF] = HSC_CONFIG(0, 150, 4, 94, 6895, 1000),
};

static const struct iio_chan_spec hsc_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.channel = HSC_CH_PRESSURE,
		.scan_type = {
			.sign = 'u',
			.realbits = HSC_PRESS_BITS,
			.storagebits = 16,
			.shift = 0,
			.endianness = IIO_BE,
		}
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.channel = HSC_CH_TEMPERATURE,
		.scan_type = {
			.sign = 'u',
			.realbits = HSC_TEMP_BITS,
			.storagebits = 16,
			.shift = HSC_TEMP_SHIFT,
			.endianness = IIO_BE,
		}
	}
};

static int hsc_get_pressure(struct hsc_state *state)
{
	int ret = 0;
	int error;

	state->spi_xfer.len = 2;
	ret = spi_sync(state->spi_dev, &state->spi_msg);
	if (ret)
		return ret;

	ret = be16_to_cpu(state->rx_buf[0]);

	error = hsc_status_error(state->spi_dev->dev, ret);
	if (error)
		return error;

	return ret;
}

static int hsc_get_temperature(struct hsc_state *state)
{
	int ret;
	int error;

	state->spi_xfer.len = 4;
	ret = spi_sync(state->spi_dev, &state->spi_msg);
	if (ret)
		return ret;

	ret = be16_to_cpu(state->rx_buf[0]);
	error = hsc_status_error(state->spi_dev->dev, ret);
	if (error)
		return error;

	ret = be16_to_cpu(state->rx_buf[1]);
	ret = (ret >> HSC_TEMP_SHIFT) & HSC_TEMP_MASK;

	return ret;
}

static int hsc_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan, int *val,
			int *val2, long mask)
{
	struct hsc_state *state = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->channel) {
		case HSC_CH_PRESSURE:
			ret = hsc_get_pressure(state);
			if (ret < 0)
				break;
			*val = ret;
			ret = IIO_VAL_INT;
			break;
		case HSC_CH_TEMPERATURE:
			ret = hsc_get_temperature(state);
			if (ret < 0)
				break;
			*val = ret;
			ret = IIO_VAL_INT;
			break;
		default:
			ret = -EINVAL;
			dev_err(state->dev,
				"%s - IIO_CHAN_INFO_RAW-bad channel (%d)\n",
				__func__, chan->channel);
			break;
		}
		break;
	case IIO_CHAN_INFO_OFFSET:
		switch (chan->channel) {
		case HSC_CH_PRESSURE:
			*val = state->offset.num;
			*val2 = state->offset.den;
			ret = IIO_VAL_FRACTIONAL;
			break;
		case HSC_CH_TEMPERATURE:
			*val = BIT(HSC_TEMP_BITS) - 1;
			*val2 = -200 / 50;
			ret = IIO_VAL_FRACTIONAL;
			break;
		default:
			dev_err(state->dev,
				"%s - IIO_CHAN_INFO_OFFSET-bad channel (%d)\n",
				__func__, chan->channel);
			ret = -EINVAL;
			break;
		}
		break;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->channel) {
		case HSC_CH_PRESSURE:		/* output unit is kPa */
			*val = state->scale.num;
			*val2 = state->scale.den;
			ret = IIO_VAL_FRACTIONAL;
			break;
		case HSC_CH_TEMPERATURE:  /* output unit is milli Celsius */
			*val = 200 * 1000;
			*val2 = BIT(HSC_TEMP_BITS) - 1;
			ret = IIO_VAL_FRACTIONAL;
			break;
		default:
			dev_err(state->dev,
				"%s - IIO_CHAN_INFO_SCALE-bad channel (%d)\n",
				__func__, chan->channel);
			ret = -EINVAL;
			break;
		}
		break;
	default:
		dev_err(state->dev, "%s - mask = %ld (INVALID)\n",
			__func__, mask);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct iio_info hsc_info = {
	.read_raw = hsc_read_raw,
};

static void hsc_init_device(struct iio_dev *indio_dev)
{
	struct hsc_state *state = iio_priv(indio_dev);
	const struct hsc_config *cfg = of_device_get_match_data(state->dev);

	/* Pressure offset value */
	state->offset.num = BIT(HSC_PRESS_BITS) *
			    (cfg->pmin * cfg->rmax - cfg->pmax * cfg->rmin);
	state->offset.den = 100 * (cfg->pmax - cfg->pmin);

	/* Pressure scale value */
	state->scale.num = 100 * cfg->knum * (cfg->pmax - cfg->pmin);
	state->scale.den = BIT(HSC_PRESS_BITS) *
			   cfg->kden * (cfg->rmax - cfg->rmin);
}

static int hsc_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct hsc_state *state;
	struct iio_dev *indio_dev;
	int ret;

	if (spi->max_speed_hz > HSC_MAX_SPI_FREQ_HZ) {
		dev_warn(dev, "SPI CLK, %d Hz exceeds %d Hz - changed to max\n",
			spi->max_speed_hz,
			HSC_MAX_SPI_FREQ_HZ);
		spi->max_speed_hz = HSC_MAX_SPI_FREQ_HZ;
	}

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "%s - Error in spi_setup()\n", __func__);
		return ret;
	}

	indio_dev = devm_iio_device_alloc(dev, sizeof(*state));
	if (!indio_dev) {
		dev_err(dev, "%s - Error allocating iio_device\n", __func__);
		return -ENOMEM;
	}

	state = iio_priv(indio_dev);
	spi_set_drvdata(spi, state);
	state->spi_dev = spi;
	state->dev = dev;

	indio_dev->dev.parent = dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &hsc_info;
	indio_dev->channels = hsc_channels;
	indio_dev->num_channels = ARRAY_SIZE(hsc_channels);

	state->spi_xfer.rx_buf = &state->rx_buf[0];
	state->spi_xfer.tx_buf = NULL;
	state->spi_xfer.cs_change = 0;
	spi_message_init(&state->spi_msg);
	spi_message_add_tail(&state->spi_xfer, &state->spi_msg);

	hsc_init_device(indio_dev);

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		dev_err(dev, "iio_device_register failed: %d\n", ret);

	dev_dbg(dev, "%s - scale = %d/%d, offset = %d/%d\n", __func__,
		state->scale.num, state->scale.den,
		state->offset.num, state->offset.den);

	return ret;
}

static int hsc_spi_remove(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev = dev_get_drvdata(dev);

	iio_device_unregister(indio_dev);

	return 0;
}

static const struct of_device_id hsc_of_match[] = {
	{ .compatible = "honeywell,hsc001baa", .data = &hsc_cfg[HSC001BAA] },
	{ .compatible = "honeywell,hsc001bab", .data = &hsc_cfg[HSC001BAB] },
	{ .compatible = "honeywell,hsc001bac", .data = &hsc_cfg[HSC001BAC] },
	{ .compatible = "honeywell,hsc001baf", .data = &hsc_cfg[HSC001BAF] },

	{ .compatible = "honeywell,hsc1_6baa", .data = &hsc_cfg[HSC1_6BAA] },
	{ .compatible = "honeywell,hsc1_6bab", .data = &hsc_cfg[HSC1_6BAB] },
	{ .compatible = "honeywell,hsc1_6bac", .data = &hsc_cfg[HSC1_6BAC] },
	{ .compatible = "honeywell,hsc1_6baf", .data = &hsc_cfg[HSC1_6BAF] },

	{ .compatible = "honeywell,hsc2_5baa", .data = &hsc_cfg[HSC2_5BAA] },
	{ .compatible = "honeywell,hsc2_5bab", .data = &hsc_cfg[HSC2_5BAB] },
	{ .compatible = "honeywell,hsc2_5bac", .data = &hsc_cfg[HSC2_5BAC] },
	{ .compatible = "honeywell,hsc2_5baf", .data = &hsc_cfg[HSC2_5BAF] },

	{ .compatible = "honeywell,hsc004baa", .data = &hsc_cfg[HSC004BAA] },
	{ .compatible = "honeywell,hsc004bab", .data = &hsc_cfg[HSC004BAB] },
	{ .compatible = "honeywell,hsc004bac", .data = &hsc_cfg[HSC004BAC] },
	{ .compatible = "honeywell,hsc004baf", .data = &hsc_cfg[HSC004BAF] },

	{ .compatible = "honeywell,hsc006baa", .data = &hsc_cfg[HSC006BAA] },
	{ .compatible = "honeywell,hsc006bab", .data = &hsc_cfg[HSC006BAB] },
	{ .compatible = "honeywell,hsc006bac", .data = &hsc_cfg[HSC006BAC] },
	{ .compatible = "honeywell,hsc006baf", .data = &hsc_cfg[HSC006BAF] },

	{ .compatible = "honeywell,hsc010baa", .data = &hsc_cfg[HSC010BAA] },
	{ .compatible = "honeywell,hsc010bab", .data = &hsc_cfg[HSC010BAB] },
	{ .compatible = "honeywell,hsc010bac", .data = &hsc_cfg[HSC010BAC] },
	{ .compatible = "honeywell,hsc010baf", .data = &hsc_cfg[HSC010BAF] },

	{ .compatible = "honeywell,hsc100kaa", .data = &hsc_cfg[HSC100KAA] },
	{ .compatible = "honeywell,hsc100kab", .data = &hsc_cfg[HSC100KAB] },
	{ .compatible = "honeywell,hsc100kac", .data = &hsc_cfg[HSC100KAC] },
	{ .compatible = "honeywell,hsc100kaf", .data = &hsc_cfg[HSC100KAF] },

	{ .compatible = "honeywell,hsc160kaa", .data = &hsc_cfg[HSC160KAA] },
	{ .compatible = "honeywell,hsc160kab", .data = &hsc_cfg[HSC160KAB] },
	{ .compatible = "honeywell,hsc160kac", .data = &hsc_cfg[HSC160KAC] },
	{ .compatible = "honeywell,hsc160kaf", .data = &hsc_cfg[HSC160KAF] },

	{ .compatible = "honeywell,hsc250kaa", .data = &hsc_cfg[HSC250KAA] },
	{ .compatible = "honeywell,hsc250kab", .data = &hsc_cfg[HSC250KAB] },
	{ .compatible = "honeywell,hsc250kac", .data = &hsc_cfg[HSC250KAC] },
	{ .compatible = "honeywell,hsc250kaf", .data = &hsc_cfg[HSC250KAF] },

	{ .compatible = "honeywell,hsc400kaa", .data = &hsc_cfg[HSC400KAA] },
	{ .compatible = "honeywell,hsc400kab", .data = &hsc_cfg[HSC400KAB] },
	{ .compatible = "honeywell,hsc400kac", .data = &hsc_cfg[HSC400KAC] },
	{ .compatible = "honeywell,hsc400kaf", .data = &hsc_cfg[HSC400KAF] },

	{ .compatible = "honeywell,hsc600kaa", .data = &hsc_cfg[HSC600KAA] },
	{ .compatible = "honeywell,hsc600kab", .data = &hsc_cfg[HSC600KAB] },
	{ .compatible = "honeywell,hsc600kac", .data = &hsc_cfg[HSC600KAC] },
	{ .compatible = "honeywell,hsc600kaf", .data = &hsc_cfg[HSC600KAF] },

	{ .compatible = "honeywell,hsc001gaa", .data = &hsc_cfg[HSC001GAA] },
	{ .compatible = "honeywell,hsc001gab", .data = &hsc_cfg[HSC001GAB] },
	{ .compatible = "honeywell,hsc001gac", .data = &hsc_cfg[HSC001GAC] },
	{ .compatible = "honeywell,hsc001gaf", .data = &hsc_cfg[HSC001GAF] },

	{ .compatible = "honeywell,hsc015paa", .data = &hsc_cfg[HSC015PAA] },
	{ .compatible = "honeywell,hsc015pab", .data = &hsc_cfg[HSC015PAB] },
	{ .compatible = "honeywell,hsc015pac", .data = &hsc_cfg[HSC015PAC] },
	{ .compatible = "honeywell,hsc015paf", .data = &hsc_cfg[HSC015PAF] },

	{ .compatible = "honeywell,hsc030paa", .data = &hsc_cfg[HSC030PAA] },
	{ .compatible = "honeywell,hsc030pab", .data = &hsc_cfg[HSC030PAB] },
	{ .compatible = "honeywell,hsc030pac", .data = &hsc_cfg[HSC030PAC] },
	{ .compatible = "honeywell,hsc030paf", .data = &hsc_cfg[HSC030PAF] },

	{ .compatible = "honeywell,hsc060paa", .data = &hsc_cfg[HSC060PAA] },
	{ .compatible = "honeywell,hsc060pab", .data = &hsc_cfg[HSC060PAB] },
	{ .compatible = "honeywell,hsc060pac", .data = &hsc_cfg[HSC060PAC] },
	{ .compatible = "honeywell,hsc060paf", .data = &hsc_cfg[HSC060PAF] },

	{ .compatible = "honeywell,hsc100paa", .data = &hsc_cfg[HSC100PAA] },
	{ .compatible = "honeywell,hsc100pab", .data = &hsc_cfg[HSC100PAB] },
	{ .compatible = "honeywell,hsc100pac", .data = &hsc_cfg[HSC100PAC] },
	{ .compatible = "honeywell,hsc100paf", .data = &hsc_cfg[HSC100PAF] },

	{ .compatible = "honeywell,hsc150paa", .data = &hsc_cfg[HSC150PAA] },
	{ .compatible = "honeywell,hsc150pab", .data = &hsc_cfg[HSC150PAB] },
	{ .compatible = "honeywell,hsc150pac", .data = &hsc_cfg[HSC150PAC] },
	{ .compatible = "honeywell,hsc150paf", .data = &hsc_cfg[HSC150PAF] },

	{ },
};
MODULE_DEVICE_TABLE(of, hsc_of_match);

static struct spi_driver hsc_spi_driver = {
	.probe = hsc_spi_probe,
	.remove = hsc_spi_remove,
	.driver = {
		.name = "hsc_spi_pressure_sensor",
		.of_match_table = hsc_of_match,
	},
};

module_spi_driver(hsc_spi_driver);

MODULE_AUTHOR("Carlos Iglesias <carlosiglesias@emutex.com>");
MODULE_DESCRIPTION("Honeywell HSC SPI pressure sensor driver");
MODULE_LICENSE("GPL v2");
