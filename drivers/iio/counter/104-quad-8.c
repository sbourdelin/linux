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
#include <linux/kernel.h>
#include <linux/isa.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#define QUAD8_EXTENT 32

static unsigned int base[max_num_isa_dev(QUAD8_EXTENT)];
static unsigned int num_quad8;
module_param_array(base, uint, &num_quad8, 0);
MODULE_PARM_DESC(base, "ACCES 104-QUAD-8 base addresses");

#define QUAD8_NUM_COUNTERS 8

/**
 * struct quad8_iio - IIO device private data structure
 * @preset:		array of preset values
 * @encoding:		array of encoding configurations
 * @counter_mode:	array of counter_mode configurations
 * @quadrature_mode:	array of quadrature_mode configurations
 * @ab_enable:		array of A and B inputs enable configurations
 * @preset_enable:	array of preset enable configurations
 * @index_function:	array of index function enable configurations
 * @index_polarity:	array of index polarity configurations
 * @base:		base port address of the IIO device
 */
struct quad8_iio {
	unsigned int preset[QUAD8_NUM_COUNTERS];
	unsigned int encoding[QUAD8_NUM_COUNTERS];
	unsigned int counter_mode[QUAD8_NUM_COUNTERS];
	unsigned int quadrature_mode[QUAD8_NUM_COUNTERS];
	unsigned int ab_enable[QUAD8_NUM_COUNTERS];
	unsigned int preset_enable[QUAD8_NUM_COUNTERS];
	unsigned int index_function[QUAD8_NUM_COUNTERS];
	unsigned int index_polarity[QUAD8_NUM_COUNTERS];
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
		if (chan->type == IIO_INDEX) {
			*val = !!(inb(base_offset + 1) & BIT(6));
			return IIO_VAL_INT;
		}

		/* Reset Byte Pointer; transfer Counter to Output Latch */
		outb(0x11, base_offset + 1);

		*val = 0;
		for (i = 0; i < 3; i++)
			*val |= (unsigned int)inb(base_offset) << (8 * i);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PRESET:
		*val = priv->preset[chan->channel];
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
		if (chan->type == IIO_INDEX)
			return -EINVAL;

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

		/* Reset Borrow, Carry, Compare, and Sign flags */
		outb(0x02, base_offset + 1);
		/* Reset Error flag */
		outb(0x06, base_offset + 1);

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
	}

	return -EINVAL;
}

static const struct iio_info quad8_info = {
	.driver_module = THIS_MODULE,
	.read_raw = quad8_read_raw,
	.write_raw = quad8_write_raw
};

static const char *const quad8_toggle_states[] = {
	"0",
	"1"
};

static int quad8_get_borrow(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return inb(base_offset) & BIT(0);
}

static const struct iio_enum quad8_borrow_enum = {
	.items = quad8_toggle_states,
	.num_items = ARRAY_SIZE(quad8_toggle_states),
	.get = quad8_get_borrow
};

static int quad8_get_carry(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(1));
}

static const struct iio_enum quad8_carry_enum = {
	.items = quad8_toggle_states,
	.num_items = ARRAY_SIZE(quad8_toggle_states),
	.get = quad8_get_carry
};

static int quad8_get_compare(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(2));
}

static const struct iio_enum quad8_compare_enum = {
	.items = quad8_toggle_states,
	.num_items = ARRAY_SIZE(quad8_toggle_states),
	.get = quad8_get_compare
};

static const char *const quad8_sign_states[] = {
	"+",
	"-"
};

static int quad8_get_sign(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(3));
}

static const struct iio_enum quad8_sign_enum = {
	.items = quad8_sign_states,
	.num_items = ARRAY_SIZE(quad8_sign_states),
	.get = quad8_get_sign
};

static const char *const quad8_error_states[] = {
	"No errors detected",
	"Excessive noise detected at the count inputs"
};

static int quad8_get_error(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(4));
}

static const struct iio_enum quad8_error_enum = {
	.items = quad8_error_states,
	.num_items = ARRAY_SIZE(quad8_error_states),
	.get = quad8_get_error
};

static const char *const quad8_count_direction_states[] = {
	"down",
	"up"
};

static int quad8_get_count_direction(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(5));
}

static const struct iio_enum quad8_count_direction_enum = {
	.items = quad8_count_direction_states,
	.num_items = ARRAY_SIZE(quad8_count_direction_states),
	.get = quad8_get_count_direction
};

static const char *const quad8_encoding_modes[] = {
	"binary",
	"binary-coded decimal"
};

static int quad8_set_encoding(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int encoding)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int mode_cfg = encoding |
		priv->counter_mode[chan->channel] << 1 |
		priv->quadrature_mode[chan->channel] << 3;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->encoding[chan->channel] = encoding;

	/* Load mode configuration to Counter Mode Register */
	outb(0x20 | mode_cfg, base_offset);

	return 0;
}

static int quad8_get_encoding(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->encoding[chan->channel];
}

static const struct iio_enum quad8_encoding_enum = {
	.items = quad8_encoding_modes,
	.num_items = ARRAY_SIZE(quad8_encoding_modes),
	.set = quad8_set_encoding,
	.get = quad8_get_encoding
};

static const char *const quad8_counter_modes[] = {
	"normal",
	"range limit",
	"non-recycle",
	"modulo-n"
};

static int quad8_set_counter_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int counter_mode)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int mode_cfg = priv->encoding[chan->channel] |
		counter_mode << 1 | priv->quadrature_mode[chan->channel] << 3;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->counter_mode[chan->channel] = counter_mode;

	/* Load mode configuration to Counter Mode Register */
	outb(0x20 | mode_cfg, base_offset);

	return 0;
}

static int quad8_get_counter_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->counter_mode[chan->channel];
}

static const struct iio_enum quad8_counter_mode_enum = {
	.items = quad8_counter_modes,
	.num_items = ARRAY_SIZE(quad8_counter_modes),
	.set = quad8_set_counter_mode,
	.get = quad8_get_counter_mode
};

static const char *const quad8_enable_modes[] = {
	"disabled",
	"enabled"
};

static int quad8_set_index_function(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int index_function)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int idr_cfg = index_function |
		priv->index_polarity[chan->channel] << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	/* Index function must be disabled in non-quadrature mode */
	if (index_function && !priv->quadrature_mode[chan->channel])
		return -EINVAL;

	priv->index_function[chan->channel] = index_function;

	/* Load Index Control configuration to Index Control Register */
	outb(0x60 | idr_cfg, base_offset);

	return 0;
}

static int quad8_get_index_function(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->index_function[chan->channel];
}

static const struct iio_enum quad8_index_function_enum = {
	.items = quad8_enable_modes,
	.num_items = ARRAY_SIZE(quad8_enable_modes),
	.set = quad8_set_index_function,
	.get = quad8_get_index_function
};

static const char *const quad8_quadrature_modes[] = {
	"non-quadrature",
	"quadrature x1",
	"quadrature x2",
	"quadrature x4"
};

static int quad8_set_quadrature_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int quadrature_mode)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int mode_cfg = priv->encoding[chan->channel] |
		priv->counter_mode[chan->channel] << 1 | quadrature_mode << 3;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->quadrature_mode[chan->channel] = quadrature_mode;

	/* Index function must be disabled in non-quadrature mode */
	if (!quadrature_mode && priv->index_function[chan->channel])
		quad8_set_index_function(indio_dev, chan, 0);

	/* Load mode configuration to Counter Mode Register */
	outb(0x20 | mode_cfg, base_offset);

	return 0;
}

static int quad8_get_quadrature_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->quadrature_mode[chan->channel];
}

static const struct iio_enum quad8_quadrature_mode_enum = {
	.items = quad8_quadrature_modes,
	.num_items = ARRAY_SIZE(quad8_quadrature_modes),
	.set = quad8_set_quadrature_mode,
	.get = quad8_get_quadrature_mode
};

static int quad8_set_ab_enable(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int ab_enable)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int ioc_cfg = ab_enable |
		priv->preset_enable[chan->channel] << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->ab_enable[chan->channel] = ab_enable;

	/* Load I/O control configuration to Input / Output Control Register */
	outb(0x40 | ioc_cfg, base_offset);

	return 0;
}

static int quad8_get_ab_enable(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->ab_enable[chan->channel];
}

static const struct iio_enum quad8_ab_enable_enum = {
	.items = quad8_enable_modes,
	.num_items = ARRAY_SIZE(quad8_enable_modes),
	.set = quad8_set_ab_enable,
	.get = quad8_get_ab_enable
};

static const char *const quad8_preset_enable_modes[] = {
	"index active",
	"disabled"
};

static int quad8_set_preset_enable(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int preset_enable)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int ioc_cfg = priv->ab_enable[chan->channel] |
		preset_enable << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->preset_enable[chan->channel] = preset_enable;

	/* Load I/O control configuration to Input / Output Control Register */
	outb(0x40 | ioc_cfg, base_offset);

	return 0;
}

static int quad8_get_preset_enable(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->preset_enable[chan->channel];
}

static const struct iio_enum quad8_preset_enable_enum = {
	.items = quad8_preset_enable_modes,
	.num_items = ARRAY_SIZE(quad8_preset_enable_modes),
	.set = quad8_set_preset_enable,
	.get = quad8_get_preset_enable
};

static const char *const quad8_index_polarity_modes[] = {
	"negative",
	"positive"
};

static int quad8_set_index_polarity(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int index_polarity)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int idr_cfg = priv->index_function[chan->channel] |
		index_polarity << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->index_polarity[chan->channel] = index_polarity;

	/* Load Index Control configuration to Index Control Register */
	outb(0x60 | idr_cfg, base_offset);

	return 0;
}

static int quad8_get_index_polarity(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->index_polarity[chan->channel];
}

static const struct iio_enum quad8_index_polarity_enum = {
	.items = quad8_index_polarity_modes,
	.num_items = ARRAY_SIZE(quad8_index_polarity_modes),
	.set = quad8_set_index_polarity,
	.get = quad8_get_index_polarity
};

static const struct iio_chan_spec_ext_info quad8_count_ext_info[] = {
	IIO_ENUM("borrow", IIO_SEPARATE, &quad8_borrow_enum),
	IIO_ENUM_AVAILABLE("borrow", &quad8_borrow_enum),
	IIO_ENUM("carry", IIO_SEPARATE, &quad8_carry_enum),
	IIO_ENUM_AVAILABLE("carry", &quad8_carry_enum),
	IIO_ENUM("compare", IIO_SEPARATE, &quad8_compare_enum),
	IIO_ENUM_AVAILABLE("compare", &quad8_compare_enum),
	IIO_ENUM("sign", IIO_SEPARATE, &quad8_sign_enum),
	IIO_ENUM_AVAILABLE("sign", &quad8_sign_enum),
	IIO_ENUM("error", IIO_SEPARATE, &quad8_error_enum),
	IIO_ENUM_AVAILABLE("error", &quad8_error_enum),
	IIO_ENUM("count_direction", IIO_SEPARATE, &quad8_count_direction_enum),
	IIO_ENUM_AVAILABLE("count_direction", &quad8_count_direction_enum),
	IIO_ENUM("encoding", IIO_SEPARATE, &quad8_encoding_enum),
	IIO_ENUM_AVAILABLE("encoding", &quad8_encoding_enum),
	IIO_ENUM("counter_mode", IIO_SEPARATE, &quad8_counter_mode_enum),
	IIO_ENUM_AVAILABLE("counter_mode", &quad8_counter_mode_enum),
	IIO_ENUM("quadrature_mode", IIO_SEPARATE, &quad8_quadrature_mode_enum),
	IIO_ENUM_AVAILABLE("quadrature_mode", &quad8_quadrature_mode_enum),
	IIO_ENUM("ab_enable", IIO_SEPARATE, &quad8_ab_enable_enum),
	IIO_ENUM_AVAILABLE("ab_enable", &quad8_ab_enable_enum),
	IIO_ENUM("preset_enable", IIO_SEPARATE, &quad8_preset_enable_enum),
	IIO_ENUM_AVAILABLE("preset_enable", &quad8_preset_enable_enum),
	{}
};

static const struct iio_chan_spec_ext_info quad8_index_ext_info[] = {
	IIO_ENUM("index_function", IIO_SEPARATE, &quad8_index_function_enum),
	IIO_ENUM_AVAILABLE("index_function", &quad8_index_function_enum),
	IIO_ENUM("index_polarity", IIO_SEPARATE, &quad8_index_polarity_enum),
	IIO_ENUM_AVAILABLE("index_polarity", &quad8_index_polarity_enum),
	{}
};

#define QUAD8_COUNT_CHAN(_chan) {			\
	.type = IIO_COUNT,				\
	.channel = (_chan),				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
		BIT(IIO_CHAN_INFO_PRESET),		\
	.ext_info = quad8_count_ext_info,		\
	.indexed = 1					\
}

#define QUAD8_INDEX_CHAN(_chan) {			\
	.type = IIO_INDEX,				\
	.channel = (_chan),				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.ext_info = quad8_index_ext_info,		\
	.indexed = 1					\
}

static const struct iio_chan_spec quad8_channels[] = {
	QUAD8_COUNT_CHAN(0), QUAD8_INDEX_CHAN(0),
	QUAD8_COUNT_CHAN(1), QUAD8_INDEX_CHAN(1),
	QUAD8_COUNT_CHAN(2), QUAD8_INDEX_CHAN(2),
	QUAD8_COUNT_CHAN(3), QUAD8_INDEX_CHAN(3),
	QUAD8_COUNT_CHAN(4), QUAD8_INDEX_CHAN(4),
	QUAD8_COUNT_CHAN(5), QUAD8_INDEX_CHAN(5),
	QUAD8_COUNT_CHAN(6), QUAD8_INDEX_CHAN(6),
	QUAD8_COUNT_CHAN(7), QUAD8_INDEX_CHAN(7)
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
	indio_dev->num_channels = ARRAY_SIZE(quad8_channels);
	indio_dev->channels = quad8_channels;
	indio_dev->name = dev_name(dev);

	priv = iio_priv(indio_dev);
	priv->base = base[id];

	/* Reset all counters and disable interrupt function */
	outb(0x01, base[id] + 0x11);
	/* Set initial configuration for all counters */
	for (i = 0; i < QUAD8_NUM_COUNTERS; i++) {
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
		/* Disable A and B inputs; preset on index; FLG1 as Carry */
		outb(0x40, base_offset + 1);
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
