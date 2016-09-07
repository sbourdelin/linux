/*
 * IIO driver for the ACCES 104-QUAD-8
 * Copyright (C) 2016 William Breathitt Gray
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This driver supports the ACCES 104-QUAD-8 and ACCES 104-QUAD-4.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/isa.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#define QUAD8_CHAN(chan) {						\
	.type = IIO_COUNT,						\
	.channel = chan,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
		BIT(IIO_CHAN_INFO_FLAGS) |				\
		BIT(IIO_CHAN_INFO_DIRECTION) |				\
		BIT(IIO_CHAN_INFO_INDEX) | BIT(IIO_CHAN_INFO_MODE) |	\
		BIT(IIO_CHAN_INFO_PRESET) |				\
		BIT(IIO_CHAN_INFO_PRESET_EN),				\
	.indexed = 1							\
}

#define QUAD8_NUM_CHAN 8

#define QUAD8_EXTENT 32

static unsigned int base[max_num_isa_dev(QUAD8_EXTENT)];
static unsigned int num_quad8;
module_param_array(base, uint, &num_quad8, 0);
MODULE_PARM_DESC(base, "ACCES 104-QUAD-8 base addresses");

/**
 * struct quad8_iio - IIO device private data structure
 * @mode:	array of counter mode configurations
 * @preset:	array of preset values
 * @preset_en:	array of preset enable configurations
 * @base:	base port address of the IIO device
 */
struct quad8_iio {
	unsigned int mode[QUAD8_NUM_CHAN];
	unsigned int preset[QUAD8_NUM_CHAN];
	unsigned int preset_en[QUAD8_NUM_CHAN];
	unsigned int base;
};

static int quad8_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel;
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* Reset Byte Pointer; transfer Counter to Output Latch */
		outb(0x11, base_offset + 1);

		*val = 0;
		for (i = 0; i < 3; i++)
			*val |= (unsigned int)inb(base_offset) << (8 * i);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_FLAGS:
		*val = inb(base_offset + 1);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_DIRECTION:
		*val = !!(inb(base_offset + 1) & BIT(5));
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_INDEX:
		*val = !!(inb(base_offset + 1) & BIT(6));
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_MODE:
		*val = priv->mode[chan->channel];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PRESET:
		*val = priv->preset[chan->channel];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PRESET_EN:
		*val = priv->preset_en[chan->channel];
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int quad8_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel;
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* Only 24-bit values are supported */
		if ((unsigned int)val > 0xFFFFFF)
			return -EINVAL;

		/* Reset Byte Pointer */
		outb(0x01, base_offset + 1);

		/* Set Preset Register */
		for (i = 0; i < 3; i++)
			outb(val >> (8 * i), base_offset);

		/* Transfer Preset Register to Counter */
		outb(0x08, base_offset + 1);

		/* Reset Byte Pointer */
		outb(0x01, base_offset + 1);

		/* Set Preset Register back to original value */
		val = priv->preset[chan->channel];
		for (i = 0; i < 3; i++)
			outb(val >> (8 * i), base_offset);

		return 0;
	case IIO_CHAN_INFO_FLAGS:
		/* Only clear operation is supported */
		if (val)
			return -EINVAL;

		/* Reset Borrow, Carry, Compare, and Sign flags */
		outb(0x02, base_offset + 1);
		/* Reset Error flag */
		outb(0x06, base_offset + 1);

		return 0;
	case IIO_CHAN_INFO_MODE:
		/* Counter Mode Register exposes 5 configuration bits */
		if ((unsigned int)val > 0x1F)
			return -EINVAL;

		priv->mode[chan->channel] = val;

		/* Load mode configuration to Counter Mode Register */
		outb(0x20 | val, base_offset + 1);

		return 0;
	case IIO_CHAN_INFO_PRESET:
		/* Only 24-bit values are supported */
		if ((unsigned int)val > 0xFFFFFF)
			return -EINVAL;

		priv->preset[chan->channel] = val;

		/* Reset Byte Pointer */
		outb(0x01, base_offset + 1);

		/* Set Preset Register */
		for (i = 0; i < 3; i++)
			outb(val >> (8 * i), base_offset);

		return 0;
	case IIO_CHAN_INFO_PRESET_EN:
		if (val && val != 1)
			return -EINVAL;

		priv->preset_en[chan->channel] = val;

		/* Enable/disable preset counter function */
		if (val)
			outb(0x41, base_offset + 1);
		else
			outb(0x43, base_offset + 1);

		return 0;
	}

	return -EINVAL;
}

static const struct iio_info quad8_info = {
	.driver_module = THIS_MODULE,
	.read_raw = quad8_read_raw,
	.write_raw = quad8_write_raw
};

static const struct iio_chan_spec quad8_channels[QUAD8_NUM_CHAN] = {
	QUAD8_CHAN(0), QUAD8_CHAN(1), QUAD8_CHAN(2), QUAD8_CHAN(3),
	QUAD8_CHAN(4), QUAD8_CHAN(5), QUAD8_CHAN(6), QUAD8_CHAN(7)
};

static int quad8_probe(struct device *dev, unsigned int id)
{
	struct iio_dev *indio_dev;
	struct quad8_iio *priv;
	int i, j;
	unsigned int base_offset;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	if (!devm_request_region(dev, base[id], QUAD8_EXTENT,
		dev_name(dev))) {
		dev_err(dev, "Unable to lock port addresses (0x%X-0x%X)\n",
			base[id], base[id] + QUAD8_EXTENT);
		return -EBUSY;
	}

	indio_dev->info = &quad8_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->num_channels = QUAD8_NUM_CHAN;
	indio_dev->channels = quad8_channels;
	indio_dev->name = dev_name(dev);

	priv = iio_priv(indio_dev);
	priv->base = base[id];

	/* Reset all counters and disable interrupt function */
	outb(0x01, base[id] + 0x11);
	/* Set initial configuration for all channels */
	for (i = 0; i < QUAD8_NUM_CHAN; i++) {
		base_offset = base[id] + 2 * i;
		/* Reset Byte Pointer */
		outb(0x01, base_offset + 1);
		/* Reset Preset Register */
		for (j = 0; j < 3; j++)
			outb(0x00, base_offset);
		/* Reset Borrow, Carry, Compare, and Sign flags */
		outb(0x04, base_offset + 1);
		/* Reset Error flag */
		outb(0x06, base_offset + 1);
		/* Binary encoding; Normal count; non-quadrature mode */
		outb(0x20, base_offset + 1);
		/* Enable A and B inputs; continuously count; FLG1 as Carry */
		outb(0x43, base_offset + 1);
		/* Disable index function */
		outb(0x60, base_offset + 1);
	}
	/* Enable all counters */
	outb(0x00, base[id] + 0x11);

	return devm_iio_device_register(dev, indio_dev);
}

static struct isa_driver quad8_driver = {
	.probe = quad8_probe,
	.driver = {
		.name = "104-quad-8"
	}
};

module_isa_driver(quad8_driver, num_quad8);

MODULE_AUTHOR("William Breathitt Gray <vilhelm.gray@gmail.com>");
MODULE_DESCRIPTION("ACCES 104-QUAD-8 IIO driver");
MODULE_LICENSE("GPL v2");
