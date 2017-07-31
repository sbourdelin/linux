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
#include <linux/iio/counter.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/isa.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/types.h>

#define QUAD8_EXTENT 32

static unsigned int base[max_num_isa_dev(QUAD8_EXTENT)];
static unsigned int num_quad8;
module_param_array(base, uint, &num_quad8, 0);
MODULE_PARM_DESC(base, "ACCES 104-QUAD-8 base addresses");

#define QUAD8_NUM_COUNTERS 8

/**
 * struct quad8_iio - IIO device private data structure
 * @counter:		instance of the iio_counter
 * @preset:		array of preset values
 * @count_mode:		array of count mode configurations
 * @quadrature_mode:	array of quadrature mode configurations
 * @quadrature_scale:	array of quadrature mode scale configurations
 * @ab_enable:		array of A and B inputs enable configurations
 * @preset_enable:	array of set_to_preset_on_index attribute configurations
 * @synchronous_mode:	array of index function synchronous mode configurations
 * @index_polarity:	array of index function polarity configurations
 * @base:		base port address of the IIO device
 */
struct quad8_iio {
	struct iio_counter counter;
	unsigned int preset[QUAD8_NUM_COUNTERS];
	unsigned int count_mode[QUAD8_NUM_COUNTERS];
	unsigned int quadrature_mode[QUAD8_NUM_COUNTERS];
	unsigned int quadrature_scale[QUAD8_NUM_COUNTERS];
	unsigned int ab_enable[QUAD8_NUM_COUNTERS];
	unsigned int preset_enable[QUAD8_NUM_COUNTERS];
	unsigned int synchronous_mode[QUAD8_NUM_COUNTERS];
	unsigned int index_polarity[QUAD8_NUM_COUNTERS];
	unsigned int base;
};

static int quad8_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel;
	unsigned int flags;
	unsigned int borrow;
	unsigned int carry;
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_INDEX) {
			*val = !!(inb(priv->base + 0x16) & BIT(chan->channel));
			return IIO_VAL_INT;
		}

		flags = inb(base_offset + 1);
		borrow = flags & BIT(0);
		carry = !!(flags & BIT(1));

		/* Borrow XOR Carry effectively doubles count range */
		*val = (borrow ^ carry) << 24;

		/* Reset Byte Pointer; transfer Counter to Output Latch */
		outb(0x11, base_offset + 1);

		for (i = 0; i < 3; i++)
			*val |= (unsigned int)inb(base_offset) << (8 * i);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_ENABLE:
		*val = priv->ab_enable[chan->channel];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		*val2 = priv->quadrature_scale[chan->channel];
		return IIO_VAL_FRACTIONAL_LOG2;
	}

	return -EINVAL;
}

static int quad8_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel;
	int i;
	unsigned int ior_cfg;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_INDEX)
			return -EINVAL;

		/* Only 24-bit values are supported */
		if ((unsigned int)val > 0xFFFFFF)
			return -EINVAL;

		/* Reset Byte Pointer */
		outb(0x01, base_offset + 1);

		/* Counter can only be set via Preset Register */
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
	case IIO_CHAN_INFO_ENABLE:
		/* only boolean values accepted */
		if (val < 0 || val > 1)
			return -EINVAL;

		priv->ab_enable[chan->channel] = val;

		ior_cfg = val | priv->preset_enable[chan->channel] << 1;

		/* Load I/O control configuration */
		outb(0x40 | ior_cfg, base_offset + 1);

		return 0;
	case IIO_CHAN_INFO_SCALE:
		/* Quadrature scaling only available in quadrature mode */
		if (!priv->quadrature_mode[chan->channel] && (val2 || val != 1))
			return -EINVAL;

		/* Only three gain states (1, 0.5, 0.25) */
		if (val == 1 && !val2)
			priv->quadrature_scale[chan->channel] = 0;
		else if (!val)
			switch (val2) {
			case 500000:
				priv->quadrature_scale[chan->channel] = 1;
				break;
			case 250000:
				priv->quadrature_scale[chan->channel] = 2;
				break;
			default:
				return -EINVAL;
			}
		else
			return -EINVAL;

		return 0;
	}

	return -EINVAL;
}

static const struct iio_info quad8_info = {
	.driver_module = THIS_MODULE,
	.read_raw = quad8_read_raw,
	.write_raw = quad8_write_raw
};

static ssize_t quad8_read_preset(struct iio_dev *indio_dev, uintptr_t private,
	const struct iio_chan_spec *chan, char *buf)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", priv->preset[chan->channel]);
}

static ssize_t quad8_write_preset(struct iio_dev *indio_dev, uintptr_t private,
	const struct iio_chan_spec *chan, const char *buf, size_t len)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel;
	unsigned int preset;
	int ret;
	int i;

	ret = kstrtouint(buf, 0, &preset);
	if (ret)
		return ret;

	/* Only 24-bit values are supported */
	if (preset > 0xFFFFFF)
		return -EINVAL;

	priv->preset[chan->channel] = preset;

	/* Reset Byte Pointer */
	outb(0x01, base_offset + 1);

	/* Set Preset Register */
	for (i = 0; i < 3; i++)
		outb(preset >> (8 * i), base_offset);

	return len;
}

static ssize_t quad8_read_set_to_preset_on_index(struct iio_dev *indio_dev,
	uintptr_t private, const struct iio_chan_spec *chan, char *buf)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
		!priv->preset_enable[chan->channel]);
}

static ssize_t quad8_write_set_to_preset_on_index(struct iio_dev *indio_dev,
	uintptr_t private, const struct iio_chan_spec *chan, const char *buf,
	size_t len)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;
	bool preset_enable;
	int ret;
	unsigned int ior_cfg;

	ret = kstrtobool(buf, &preset_enable);
	if (ret)
		return ret;

	/* Preset enable is active low in Input/Output Control register */
	preset_enable = !preset_enable;

	priv->preset_enable[chan->channel] = preset_enable;

	ior_cfg = priv->ab_enable[chan->channel] |
		(unsigned int)preset_enable << 1;

	/* Load I/O control configuration to Input / Output Control Register */
	outb(0x40 | ior_cfg, base_offset);

	return len;
}

static const char *const quad8_noise_error_states[] = {
	"No excessive noise is present at the count inputs",
	"Excessive noise is present at the count inputs"
};

static int quad8_get_noise_error(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const int base_offset = priv->base + 2 * chan->channel + 1;

	return !!(inb(base_offset) & BIT(4));
}

static const struct iio_enum quad8_noise_error_enum = {
	.items = quad8_noise_error_states,
	.num_items = ARRAY_SIZE(quad8_noise_error_states),
	.get = quad8_get_noise_error
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

static const char *const quad8_count_modes[] = {
	"normal",
	"range limit",
	"non-recycle",
	"modulo-n"
};

static int quad8_set_count_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int count_mode)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	unsigned int mode_cfg = count_mode << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	priv->count_mode[chan->channel] = count_mode;

	/* Add quadrature mode configuration */
	if (priv->quadrature_mode[chan->channel])
		mode_cfg |= (priv->quadrature_scale[chan->channel] + 1) << 3;

	/* Load mode configuration to Counter Mode Register */
	outb(0x20 | mode_cfg, base_offset);

	return 0;
}

static int quad8_get_count_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->count_mode[chan->channel];
}

static const struct iio_enum quad8_count_mode_enum = {
	.items = quad8_count_modes,
	.num_items = ARRAY_SIZE(quad8_count_modes),
	.set = quad8_set_count_mode,
	.get = quad8_get_count_mode
};

static const char *const quad8_synchronous_modes[] = {
	"non-synchronous",
	"synchronous"
};

static int quad8_set_synchronous_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int synchronous_mode)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int idr_cfg = synchronous_mode |
		priv->index_polarity[chan->channel] << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	/* Index function must be non-synchronous in non-quadrature mode */
	if (synchronous_mode && !priv->quadrature_mode[chan->channel])
		return -EINVAL;

	priv->synchronous_mode[chan->channel] = synchronous_mode;

	/* Load Index Control configuration to Index Control Register */
	outb(0x60 | idr_cfg, base_offset);

	return 0;
}

static int quad8_get_synchronous_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	const struct quad8_iio *const priv = iio_priv(indio_dev);

	return priv->synchronous_mode[chan->channel];
}

static const struct iio_enum quad8_synchronous_mode_enum = {
	.items = quad8_synchronous_modes,
	.num_items = ARRAY_SIZE(quad8_synchronous_modes),
	.set = quad8_set_synchronous_mode,
	.get = quad8_get_synchronous_mode
};

static const char *const quad8_quadrature_modes[] = {
	"non-quadrature",
	"quadrature"
};

static int quad8_set_quadrature_mode(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int quadrature_mode)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	unsigned int mode_cfg = priv->count_mode[chan->channel] << 1;
	const int base_offset = priv->base + 2 * chan->channel + 1;

	if (quadrature_mode)
		mode_cfg |= (priv->quadrature_scale[chan->channel] + 1) << 3;
	else {
		/* Quadrature scaling only available in quadrature mode */
		priv->quadrature_scale[chan->channel] = 0;

		/* Synchronous function not supported in non-quadrature mode */
		if (priv->synchronous_mode[chan->channel])
			quad8_set_synchronous_mode(indio_dev, chan, 0);
	}

	priv->quadrature_mode[chan->channel] = quadrature_mode;

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

static const char *const quad8_index_polarity_modes[] = {
	"negative",
	"positive"
};

static int quad8_set_index_polarity(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, unsigned int index_polarity)
{
	struct quad8_iio *const priv = iio_priv(indio_dev);
	const unsigned int idr_cfg = priv->synchronous_mode[chan->channel] |
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
	{
		.name = "preset",
		.shared = IIO_SEPARATE,
		.read = quad8_read_preset,
		.write = quad8_write_preset
	},
	{
		.name = "set_to_preset_on_index",
		.shared = IIO_SEPARATE,
		.read = quad8_read_set_to_preset_on_index,
		.write = quad8_write_set_to_preset_on_index
	},
	IIO_ENUM("noise_error", IIO_SEPARATE, &quad8_noise_error_enum),
	IIO_ENUM_AVAILABLE("noise_error", &quad8_noise_error_enum),
	IIO_ENUM("count_direction", IIO_SEPARATE, &quad8_count_direction_enum),
	IIO_ENUM_AVAILABLE("count_direction", &quad8_count_direction_enum),
	IIO_ENUM("count_mode", IIO_SEPARATE, &quad8_count_mode_enum),
	IIO_ENUM_AVAILABLE("count_mode", &quad8_count_mode_enum),
	IIO_ENUM("quadrature_mode", IIO_SEPARATE, &quad8_quadrature_mode_enum),
	IIO_ENUM_AVAILABLE("quadrature_mode", &quad8_quadrature_mode_enum),
	{}
};

static const struct iio_chan_spec_ext_info quad8_index_ext_info[] = {
	IIO_ENUM("synchronous_mode", IIO_SEPARATE,
		&quad8_synchronous_mode_enum),
	IIO_ENUM_AVAILABLE("synchronous_mode", &quad8_synchronous_mode_enum),
	IIO_ENUM("index_polarity", IIO_SEPARATE, &quad8_index_polarity_enum),
	IIO_ENUM_AVAILABLE("index_polarity", &quad8_index_polarity_enum),
	{}
};

#define QUAD8_COUNT_CHAN(_chan) {					\
	.type = IIO_COUNT,						\
	.channel = (_chan),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
		BIT(IIO_CHAN_INFO_ENABLE) | BIT(IIO_CHAN_INFO_SCALE),	\
	.ext_info = quad8_count_ext_info,				\
	.indexed = 1							\
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

static int quad8_signal_read(struct iio_counter *counter,
	struct iio_counter_signal *signal, int *val, int *val2)
{
	struct quad8_iio *const priv = counter->driver_data;

	if (signal->id < 16)
		return -EINVAL;

	*val = !!(inb(priv->base + 0x16) & BIT(signal->id - 16));

	return IIO_VAL_INT;
}

static int quad8_trigger_mode_get(struct iio_counter *counter,
	struct iio_counter_value *value, struct iio_counter_trigger *trigger)
{
	struct quad8_iio *const priv = counter->driver_data;
	const unsigned int mode = priv->quadrature_mode[value->id];
	const unsigned int scale = priv->quadrature_scale[value->id];
	unsigned int direction;
	const unsigned int flag_addr = priv->base + 2 * value->id + 1;
	const int signal_id = trigger->signal->id % 2;

	if (mode)
		switch (scale) {
		case 0:
			/* U/D flag: 1 = up, 0 = down */
			/* direction: 0 = up, 1 = down */
			direction = !(inb(flag_addr) & BIT(5));
			if (!signal_id)
				return direction + 1;
			break;
		case 1:
			if (!signal_id)
				return 3;
			break;
		case 2:
			return 3;
		}
	else
		if (!signal_id)
			return 1;

	return 0;
}

static int quad8_value_read(struct iio_counter *counter,
	struct iio_counter_value *value, int *val, int *val2)
{
	struct quad8_iio *const priv = counter->driver_data;
	const int base_offset = priv->base + 2 * value->id;
	unsigned int flags;
	unsigned int borrow;
	unsigned int carry;
	int i;

	flags = inb(base_offset + 1);
	borrow = flags & BIT(0);
	carry = !!(flags & BIT(1));

	/* Borrow XOR Carry effectively doubles count range */
	*val = (borrow ^ carry) << 24;

	/* Reset Byte Pointer; transfer Counter to Output Latch */
	outb(0x11, base_offset + 1);

	for (i = 0; i < 3; i++)
		*val |= (unsigned int)inb(base_offset) << (8 * i);

	return IIO_VAL_INT;
}

static int quad8_value_write(struct iio_counter *counter,
	struct iio_counter_value *value, int val, int val2)
{
	struct quad8_iio *const priv = counter->driver_data;
	const int base_offset = priv->base + 2 * value->id;
	int i;

	/* Only 24-bit values are supported */
	if ((unsigned int)val > 0xFFFFFF)
		return -EINVAL;

	/* Reset Byte Pointer */
	outb(0x01, base_offset + 1);

	/* Counter can only be set via Preset Register */
	for (i = 0; i < 3; i++)
		outb(val >> (8 * i), base_offset);

	/* Transfer Preset Register to Counter */
	outb(0x08, base_offset + 1);

	/* Reset Byte Pointer */
	outb(0x01, base_offset + 1);

	/* Set Preset Register back to original value */
	val = priv->preset[value->id];
	for (i = 0; i < 3; i++)
		outb(val >> (8 * i), base_offset);

	/* Reset Borrow, Carry, Compare, and Sign flags */
	outb(0x02, base_offset + 1);
	/* Reset Error flag */
	outb(0x06, base_offset + 1);

	return 0;
}

static int quad8_value_function_set(struct iio_counter *counter,
	struct iio_counter_value *value, unsigned int mode)
{
	struct quad8_iio *const priv = counter->driver_data;
	const unsigned int mode_cfg = mode << 3 |
		priv->count_mode[value->id] << 1;
	const unsigned int idr_cfg = priv->index_polarity[value->id] << 1;
	const int base_offset = priv->base + 2 * value->id + 1;

	if (mode)
		priv->quadrature_scale[value->id] = mode - 1;
	else {
		/* Quadrature scaling only available in quadrature mode */
		priv->quadrature_scale[value->id] = 0;

		/* Synchronous function not supported in non-quadrature mode */
		if (priv->synchronous_mode[value->id]) {
			priv->synchronous_mode[value->id] = 0;
			outb(0x60 | idr_cfg, base_offset);
		}
	}

	priv->quadrature_mode[value->id] = !!mode;

	/* Load mode configuration to Counter Mode Register */
	outb(0x20 | mode_cfg, base_offset);

	return 0;
}

static int quad8_value_function_get(struct iio_counter *counter,
	struct iio_counter_value *value)
{
	struct quad8_iio *const priv = counter->driver_data;
	unsigned int quadrature_mode = priv->quadrature_mode[value->id];

	return (quadrature_mode) ? priv->quadrature_scale[value->id] + 1 : 0;
}

static const struct iio_counter_ops quad8_ops = {
	.signal_read = quad8_signal_read,
	.trigger_mode_get = quad8_trigger_mode_get,
	.value_read = quad8_value_read,
	.value_write = quad8_value_write,
	.value_function_set = quad8_value_function_set,
	.value_function_get = quad8_value_function_get
};

static const char *const quad8_function_modes[] = {
	"non-quadrature",
	"quadrature x1",
	"quadrature x2",
	"quadrature x4"
};

#define QUAD8_SIGNAL(_id, _name) {	\
	.id = _id,			\
	.name = _name			\
}

static const struct iio_counter_signal quad8_signals[] = {
	QUAD8_SIGNAL(0, "Channel 1 Quadrature A"),
	QUAD8_SIGNAL(1, "Channel 1 Quadrature B"),
	QUAD8_SIGNAL(2, "Channel 2 Quadrature A"),
	QUAD8_SIGNAL(3, "Channel 2 Quadrature B"),
	QUAD8_SIGNAL(4, "Channel 3 Quadrature A"),
	QUAD8_SIGNAL(5, "Channel 3 Quadrature B"),
	QUAD8_SIGNAL(6, "Channel 4 Quadrature A"),
	QUAD8_SIGNAL(7, "Channel 4 Quadrature B"),
	QUAD8_SIGNAL(8, "Channel 5 Quadrature A"),
	QUAD8_SIGNAL(9, "Channel 5 Quadrature B"),
	QUAD8_SIGNAL(10, "Channel 6 Quadrature A"),
	QUAD8_SIGNAL(11, "Channel 6 Quadrature B"),
	QUAD8_SIGNAL(12, "Channel 7 Quadrature A"),
	QUAD8_SIGNAL(13, "Channel 7 Quadrature B"),
	QUAD8_SIGNAL(14, "Channel 8 Quadrature A"),
	QUAD8_SIGNAL(15, "Channel 8 Quadrature B"),
	QUAD8_SIGNAL(16, "Channel 1 Index"),
	QUAD8_SIGNAL(17, "Channel 2 Index"),
	QUAD8_SIGNAL(18, "Channel 3 Index"),
	QUAD8_SIGNAL(19, "Channel 4 Index"),
	QUAD8_SIGNAL(20, "Channel 5 Index"),
	QUAD8_SIGNAL(21, "Channel 6 Index"),
	QUAD8_SIGNAL(22, "Channel 7 Index"),
	QUAD8_SIGNAL(23, "Channel 8 Index")
};

#define QUAD8_VALUE(_id, _name) {					\
	.id = _id,							\
	.name = _name,							\
	.mode = 0,							\
	.function_modes = quad8_function_modes,				\
	.num_function_modes = ARRAY_SIZE(quad8_function_modes)		\
}

static const struct iio_counter_value quad8_values[] = {
	QUAD8_VALUE(0, "Channel 1 Count"), QUAD8_VALUE(1, "Channel 2 Count"),
	QUAD8_VALUE(2, "Channel 3 Count"), QUAD8_VALUE(3, "Channel 4 Count"),
	QUAD8_VALUE(4, "Channel 5 Count"), QUAD8_VALUE(5, "Channel 6 Count"),
	QUAD8_VALUE(6, "Channel 7 Count"), QUAD8_VALUE(7, "Channel 8 Count")
};

static const char *const quad8_trigger_modes[] = {
	"none",
	"rising edge",
	"falling edge",
	"both edges"
};

static int quad8_probe(struct device *dev, unsigned int id)
{
	struct iio_counter_signal *init_signals;
	const size_t num_init_signals = ARRAY_SIZE(quad8_signals);
	struct iio_counter_value *init_values;
	const size_t num_init_values = ARRAY_SIZE(quad8_values);
	struct iio_counter_trigger *triggers;
	struct quad8_iio *quad8iio;
	int i, j;
	unsigned int base_offset;

	if (!devm_request_region(dev, base[id], QUAD8_EXTENT, dev_name(dev))) {
		dev_err(dev, "Unable to lock port addresses (0x%X-0x%X)\n",
			base[id], base[id] + QUAD8_EXTENT);
		return -EBUSY;
	}

	init_signals = devm_kmalloc(dev, sizeof(quad8_signals), GFP_KERNEL);
	if (!init_signals)
		return -ENOMEM;

	memcpy(init_signals, quad8_signals, sizeof(quad8_signals));

	init_values = devm_kmalloc(dev, sizeof(quad8_values), GFP_KERNEL);
	if (!init_values)
		return -ENOMEM;

	memcpy(init_values, quad8_values, sizeof(quad8_values));

	/* Associate values with their respective signals */
	for (i = 0; i < num_init_values; i++) {
		triggers = devm_kmalloc(dev, 2 * sizeof(*triggers), GFP_KERNEL);
		if (!triggers)
			return -ENOMEM;

		/* Starts up in non-quadrature mode */
		triggers[0].mode = 1;
		triggers[0].trigger_modes = quad8_trigger_modes;
		triggers[0].num_trigger_modes = ARRAY_SIZE(quad8_trigger_modes);
		triggers[0].signal = &init_signals[2 * i];
		triggers[1].mode = 0;
		triggers[1].trigger_modes = quad8_trigger_modes;
		triggers[1].num_trigger_modes = ARRAY_SIZE(quad8_trigger_modes);
		triggers[1].signal = &init_signals[2 * i + 1];

		init_values[i].init_triggers = triggers;
		init_values[i].num_init_triggers = 2;
	}

	quad8iio = devm_kzalloc(dev, sizeof(*quad8iio), GFP_KERNEL);
	if (!quad8iio)
		return -ENOMEM;

	quad8iio->counter.name = dev_name(dev);
	quad8iio->counter.dev = dev;
	quad8iio->counter.ops = &quad8_ops;
	quad8iio->counter.init_signals = init_signals;
	quad8iio->counter.num_init_signals = num_init_signals;
	quad8iio->counter.init_values = init_values;
	quad8iio->counter.num_init_values = num_init_values;
	quad8iio->counter.channels = quad8_channels;
	quad8iio->counter.num_channels = ARRAY_SIZE(quad8_channels);
	quad8iio->counter.info = &quad8_info;
	quad8iio->counter.driver_data = quad8iio;
	quad8iio->base = base[id];

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
		/* Disable index function; negative index polarity */
		outb(0x60, base_offset + 1);
	}
	/* Enable all counters */
	outb(0x00, base[id] + 0x11);

	dev_set_drvdata(dev, &quad8iio->counter);

	return iio_counter_register(&quad8iio->counter);
}

static int quad8_remove(struct device *dev, unsigned int id)
{
	struct iio_counter *counter = dev_get_drvdata(dev);

	iio_counter_unregister(counter);

	return 0;
}

static struct isa_driver quad8_driver = {
	.probe = quad8_probe,
	.remove = quad8_remove,
	.driver = {
		.name = "104-quad-8"
	}
};

module_isa_driver(quad8_driver, num_quad8);

MODULE_AUTHOR("William Breathitt Gray <vilhelm.gray@gmail.com>");
MODULE_DESCRIPTION("ACCES 104-QUAD-8 IIO driver");
MODULE_LICENSE("GPL v2");
