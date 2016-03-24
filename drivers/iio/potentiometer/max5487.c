/*
 * max5487.c - Support for MAX5487, MAX5488, MAX5489 digital potentiometers
 *
 * Copyright (C) Cristina-Gabriela Moraru <cristina.moraru09@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/acpi.h>
#include <linux/regmap.h>

#include <linux/iio/sysfs.h>
#include <linux/iio/iio.h>

#define MAX5487_DRV_NAME "max5487"

#define MAX5487_WRITE_WIPER_A	0x01
#define MAX5487_WRITE_WIPER_B	0x02

/* copy both wiper regs to NV regs */
#define MAX5487_COPY_AB_TO_NV	0x23
/* copy both NV regs to wiper regs */
#define MAX5487_COPY_NV_TO_AB	0x33

enum {
	MAX5487,
	MAX5488,
	MAX5489,
};

struct max5487_cfg {
	int wipers;
	int max_pos;
	int kohms;
};

static const struct max5487_cfg max5487_cfg[] = {
	[MAX5487] = { .wipers = 2, .max_pos = 256, .kohms =  10,},
	[MAX5488] = { .wipers = 2, .max_pos = 256, .kohms =  50,},
	[MAX5489] = { .wipers = 2, .max_pos = 256, .kohms =  100,}
};

struct max5487_data {
	struct regmap *regmap;
	int chip_id;
};

#define MAX5487_CHANNEL(ch, addr) {				\
	.type = IIO_RESISTANCE,					\
	.indexed = 1,						\
	.output = 1,						\
	.channel = ch,						\
	.address = addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec max5487_channels[] = {
	MAX5487_CHANNEL(0, MAX5487_WRITE_WIPER_A),
	MAX5487_CHANNEL(1, MAX5487_WRITE_WIPER_B),
};

static int max5487_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct max5487_data *data = iio_priv(indio_dev);

	if (mask != IIO_CHAN_INFO_SCALE)
		return -EINVAL;

	*val = 1000 * max5487_cfg[data->chip_id].kohms;
	*val2 = max5487_cfg[data->chip_id].max_pos;
	return IIO_VAL_FRACTIONAL;
}

static int max5487_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct max5487_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val < 0 || val >= max5487_cfg[data->chip_id].max_pos)
			return -EINVAL;
		return regmap_write(data->regmap, chan->address, val);
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

static const struct iio_info max5487_info = {
	.read_raw = &max5487_read_raw,
	.write_raw = &max5487_write_raw,
	.driver_module = THIS_MODULE,
};

static const struct regmap_config max5487_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = MAX5487_COPY_NV_TO_AB,
};

static int max5487_spi_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct max5487_data *data;
	const struct spi_device_id *id = spi_get_device_id(spi);
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	dev_set_drvdata(&spi->dev, indio_dev);
	data = iio_priv(indio_dev);

	data->regmap = devm_regmap_init_spi(spi, &max5487_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	data->chip_id = id->driver_data;

	indio_dev->info = &max5487_info;
	indio_dev->name = id->name;
	indio_dev->dev.parent = &spi->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = max5487_channels;
	indio_dev->num_channels = ARRAY_SIZE(max5487_channels);

	/* restore both wiper regs from NV regs */
	ret = regmap_write(data->regmap, MAX5487_COPY_NV_TO_AB, 0);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static int max5487_spi_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = dev_get_drvdata(&spi->dev);
	struct max5487_data *data = iio_priv(indio_dev);

	/* save both wiper regs to NV regs */
	return regmap_write(data->regmap, MAX5487_COPY_AB_TO_NV, 0);
}

static const struct spi_device_id max5487_id[] = {
	{ "MAX5487", MAX5487 },
	{ "MAX5488", MAX5488 },
	{ "MAX5489", MAX5489 },
	{ }
};
MODULE_DEVICE_TABLE(spi, max5487_id);

static const struct acpi_device_id max5487_acpi_match[] = {
	{ "MAX5487", MAX5487 },
	{ "MAX5488", MAX5488 },
	{ "MAX5489", MAX5489 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, max5487_acpi_match);

static struct spi_driver max5487_driver = {
	.driver = {
		.name = MAX5487_DRV_NAME,
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(max5487_acpi_match),
	},
	.id_table = max5487_id,
	.probe = max5487_spi_probe,
	.remove = max5487_spi_remove
};
module_spi_driver(max5487_driver);

MODULE_AUTHOR("Cristina-Gabriela Moraru <cristina.moraru09@gmail.com>");
MODULE_DESCRIPTION("max5487 SPI driver");
MODULE_LICENSE("GPL v2");
