// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Linear Technology LTC1665/LTC1660, 8 channels DAC
 *
 * Copyright (C) 2018 Marcus Folkesson <marcus.folkesson@gmail.com>
 */
#include <linux/bitops.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#define LTC166X_REG_WAKE	0x0
#define LTC166X_REG_DAC_A	0x1
#define LTC166X_REG_DAC_B	0x2
#define LTC166X_REG_DAC_C	0x3
#define LTC166X_REG_DAC_D	0x4
#define LTC166X_REG_DAC_E	0x5
#define LTC166X_REG_DAC_F	0x6
#define LTC166X_REG_DAC_G	0x7
#define LTC166X_REG_DAC_H	0x8
#define LTC166X_REG_SLEEP	0xe

#define LTC166X_NUM_CHANNELS	8

static const struct regmap_config ltc166x_regmap_config = {
	.reg_bits = 4,
	.val_bits = 12,
};

enum ltc166x_supported_device_ids {
	ID_LTC1660,
	ID_LTC1665,
};

struct ltc166x_priv {
	struct spi_device *spi;
	struct regmap *regmap;
	struct regulator *vref_reg;
	unsigned int value[LTC166X_NUM_CHANNELS];
	unsigned int vref_mv;
};

static int ltc166x_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val,
		int *val2,
		long mask)
{
	struct ltc166x_priv *priv = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val = priv->value[chan->channel];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = priv->vref_mv;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ltc166x_write_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int val,
		int val2,
		long mask)
{
	struct ltc166x_priv *priv = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val2 != 0)
			return -EINVAL;
		if (val > GENMASK(chan->scan_type.realbits-1, 0))
			return -EINVAL;
		priv->value[chan->channel] = val;
		val <<= chan->scan_type.shift;
		return regmap_write(priv->regmap, chan->channel, val);
	default:
		return -EINVAL;
	}
}

#define LTC166X_CHAN(chan, bits) {			\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.output = 1,					\
	.channel = chan,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.scan_type = {					\
		.sign = 'u',				\
		.realbits = (bits),			\
		.storagebits = 16,			\
		.shift = 12 - (bits),			\
	},						\
}

#define  LTC166X_OCTAL_CHANNELS(bits) {			\
		LTC166X_CHAN(LTC166X_REG_DAC_A, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_B, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_C, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_D, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_E, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_F, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_G, bits),	\
		LTC166X_CHAN(LTC166X_REG_DAC_H, bits),	\
}

static const struct iio_chan_spec ltc166x_channels[][LTC166X_NUM_CHANNELS] = {
	[ID_LTC1660] = LTC166X_OCTAL_CHANNELS(10),
	[ID_LTC1665] = LTC166X_OCTAL_CHANNELS(8),
};

static const struct iio_info ltc166x_info = {
	.read_raw = &ltc166x_read_raw,
	.write_raw = &ltc166x_write_raw,
};

static int __maybe_unused ltc166x_suspend(struct device *dev)
{
	struct ltc166x_priv *priv = iio_priv(spi_get_drvdata(
						to_spi_device(dev)));
	return regmap_write(priv->regmap, LTC166X_REG_SLEEP, 0x00);
}

static int __maybe_unused ltc166x_resume(struct device *dev)
{
	struct ltc166x_priv *priv = iio_priv(spi_get_drvdata(
						to_spi_device(dev)));
	return regmap_write(priv->regmap, LTC166X_REG_WAKE, 0x00);
}
static SIMPLE_DEV_PM_OPS(ltc166x_pm_ops, ltc166x_suspend, ltc166x_resume);

static int ltc166x_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct ltc166x_priv *priv;
	const struct spi_device_id *id = spi_get_device_id(spi);
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*priv));
	if (indio_dev == NULL)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->regmap = devm_regmap_init_spi(spi, &ltc166x_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&spi->dev, "failed to register spi regmap %ld\n",
			PTR_ERR(priv->regmap));
		return PTR_ERR(priv->regmap);
	}

	priv->vref_reg = devm_regulator_get(&spi->dev, "vref");
	if (IS_ERR(priv->vref_reg)) {
		dev_err(&spi->dev, "vref regulator not specified\n");
		return PTR_ERR(priv->vref_reg);
	}

	ret = regulator_enable(priv->vref_reg);
	if (ret) {
		dev_err(&spi->dev, "failed to enable vref regulator: %d\n",
				ret);
		return ret;
	}

	ret = regulator_get_voltage(priv->vref_reg);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to read vref regulator: %d\n",
				ret);
		goto error_disable_reg;
	}
	priv->vref_mv = ret / 1000;

	priv->spi = spi;
	spi_set_drvdata(spi, indio_dev);
	indio_dev->dev.parent = &spi->dev;
	indio_dev->info = &ltc166x_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ltc166x_channels[id->driver_data];
	indio_dev->num_channels = LTC166X_NUM_CHANNELS;
	indio_dev->name = id->name;

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "failed to register iio device: %d\n",
				ret);
		goto error_disable_reg;
	}

	return 0;

error_disable_reg:
	regulator_disable(priv->vref_reg);

	return ret;
}

static int ltc166x_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct ltc166x_priv *priv = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	regulator_disable(priv->vref_reg);

	return 0;
}

static const struct of_device_id ltc166x_dt_ids[] = {
	{ .compatible = "lltc,ltc1660", .data = (void *)ID_LTC1660 },
	{ .compatible = "lltc,ltc1665", .data = (void *)ID_LTC1665 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ltc166x_dt_ids);

static const struct spi_device_id ltc166x_id[] = {
	{"ltc1660", ID_LTC1660},
	{"ltc1665", ID_LTC1665},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, ltc166x_id);

static struct spi_driver ltc166x_driver = {
	.driver = {
		.name = "ltc166x",
		.of_match_table = ltc166x_dt_ids,
		.pm = &ltc166x_pm_ops,
	},
	.probe	= ltc166x_probe,
	.remove = ltc166x_remove,
	.id_table = ltc166x_id,
};
module_spi_driver(ltc166x_driver);

MODULE_AUTHOR("Marcus Folkesson <marcus.folkesson@gmail.com>");
MODULE_DESCRIPTION("Linear Technology LTC166X DAC");
MODULE_LICENSE("GPL v2");
