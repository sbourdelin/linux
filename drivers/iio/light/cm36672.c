/*
 * CM36672 Proximity Sensor
 *
 * Copyright (C) 2014-2016 Vishay Capella
 * Author: Kevin Tsai <capellamicro@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published
 * by the Free Software Foundation.
 *
 * IIO driver for CM36672 (7-bit I2C slave address 0x60).
 */
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/gpio.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>

#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#endif

#define CM36672_DRIVER_NAME		"cm36672"
#define CM36672_REGMAP_NAME		"cm36672_regmap"

/* Sensor registers */
#define CM36672_ADDR_PRX_CONF		0x03
#define CM36672_ADDR_PRX_CONF3		0x04
#define CM36672_ADDR_PRX_THDL		0x06
#define CM36672_ADDR_PRX_THDH		0x07
#define CM36672_REGS_NUM		0x08
/* Read only registers */
#define CM36672_ADDR_PRX		0x08
#define CM36672_ADDR_STATUS		0x0B

/* PRX_CONF */
#define CM36672_PRX_HD_SHIFT		11
#define CM36672_PRX_HD			(1 << CM36672_PRX_HD_SHIFT)

/* PRX_CONF: interrupt */
#define CM36672_PRX_INT_THDH		BIT(8)
#define CM36672_PRX_INT_THDL		BIT(9)
#define CM36672_PRX_INT_MASK		(CM36672_PRX_INT_THDH |	\
					CM36672_PRX_INT_THDL)

/* PRX_CONF: persistence */
#define CM36672_PRX_PERS_MASK		(BIT(4) | BIT(5))
#define CM36672_PRX_PERS_SHIFT		4
#define CM36672_PRX_PERS_DISABLE	0
#define CM36672_PRX_PERS_2		(1 << CM36672_PRX_PERS_SHIFT)
#define CM36672_PRX_PERS_3		(2 << CM36672_PRX_PERS_SHIFT)
#define CM36672_PRX_PERS_4		(3 << CM36672_PRX_PERS_SHIFT)

/* PRX_CONF: integration time */
#define CM36672_PRX_IT_MASK		(BIT(1) | BIT(2) | BIT(3))
#define CM36672_PRX_IT_SHIFT		1
#define CM36672_PRX_IT_1T		0
#define CM36672_PRX_IT_1_5T		(1 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_2T		(2 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_2_5T		(3 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_3T		(4 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_3_5T		(5 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_4T		(6 << CM36672_PRX_IT_SHIFT)
#define CM36672_PRX_IT_8T		(7 << CM36672_PRX_IT_SHIFT)

/* PRX_CONF3 */
#define CM36672_PRX_LED_I_MASK		(BIT(8) | BIT(9) | BIT(10))
#define CM36672_PRX_LED_I_SHIFT		8
#define CM36672_PRX_LED_I_50MA		0
#define CM36672_PRX_LED_I_75MA		(1 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_100MA		(2 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_120MA		(3 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_140MA		(4 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_160MA		(5 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_180MA		(6 << CM36672_PRX_LED_I_SHIFT)
#define CM36672_PRX_LED_I_200MA		(7 << CM36672_PRX_LED_I_SHIFT)

/* INT_FLAG */
#define CM36672_INT_PRX_CLOSE		BIT(9)
#define CM36672_INT_PRX_AWAY		BIT(8)

struct cm36672_it_scale {
	u8 it;
	int val;
	int val2;
};

static const struct cm36672_it_scale cm36672_prx_it_scales[] = {
	{0, 0, 100},	/* 0.00010 */
	{1, 0, 150},	/* 0.00015 */
	{2, 0, 200},	/* 0.00020 */
	{3, 0, 250},	/* 0.00025 */
	{4, 0, 300},	/* 0.00030 */
	{5, 0, 350},	/* 0.00035 */
	{6, 0, 400},	/* 0.00040 */
	{7, 0, 800},	/* 0.00080 */
};

#define CM36672_PRX_INT_TIME_AVAIL			\
	"0.000100 0.000150 0.000200 0.000250 "		\
	"0.000300 0.000350 0.000400 0.000800"

static const u16 cm36672_regs_default[] = {
	0x0001,
	0x0000,
	0x0000,
	CM36672_PRX_INT_THDH | CM36672_PRX_INT_THDL |
	CM36672_PRX_IT_2T | CM36672_PRX_PERS_3,
	CM36672_PRX_LED_I_100MA,
	0x0000,
	0x0005,
	0x000A,
};

struct cm36672_chip {
	const struct cm36672_platform_data *pdata;
	struct i2c_client *client;
	struct mutex lock;

	/* regmap fields */
	struct regmap *regmap;
	struct regmap_field *reg_prx_int_hi;
	struct regmap_field *reg_prx_int_lo;
	struct regmap_field *reg_prx_it;

	u16 regs[CM36672_REGS_NUM];
};

static const struct reg_field cm36672_reg_field_prx_int_hi =
				REG_FIELD(CM36672_ADDR_PRX_CONF, 8, 8);
static const struct reg_field cm36672_reg_field_prx_int_lo =
				REG_FIELD(CM36672_ADDR_PRX_CONF, 9, 9);
static const struct reg_field cm36672_reg_field_prx_it =
				REG_FIELD(CM36672_ADDR_PRX_CONF, 1, 3);

#ifdef CONFIG_OF
static void cm36672_mod_u16(u16 *reg, u16 mask, u8 shift, u16 val)
{
	*reg &= ~mask;
	*reg |= (u16)(val << shift);
}

static void cm36672_parse_dt(struct cm36672_chip *chip)
{
	struct device_node *dn = chip->client->dev.of_node;
	u32 temp_val;

	if (!of_property_read_u32(dn, "cm36672,prx_led_current",
				&temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF3],
				CM36672_PRX_LED_I_MASK,
				CM36672_PRX_LED_I_SHIFT,
				(u16)temp_val);

	if (!of_property_read_u32(dn, "cm36672,prx_hd", &temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_HD, CM36672_PRX_HD_SHIFT,
				(u16)temp_val);
}
#endif

#ifdef CONFIG_ACPI
/**
 * cm36672_acpi_get_cpm_info() - Get CPM object from ACPI
 * @client:	pointer of struct i2c_client.
 * @obj_name:	pointer of ACPI object name.
 * @count:	maximum size of return array.
 * @vals:	pointer of array for return elements.
 *
 * Convert ACPI CPM table to array. Special thanks to Srinivas Pandruvada's
 * help to implement this routine.
 *
 * Return: -ENODEV for fail. Otherwise the number of elements.
 */
static int cm36672_acpi_get_cpm_info(struct i2c_client *client, char *obj_name,
							int count, u64 *vals)
{
	acpi_handle handle;
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};
	int i;
	acpi_status status;
	union acpi_object *cpm;

	handle = ACPI_HANDLE(&client->dev);
	if (!handle)
		return -ENODEV;

	status = acpi_evaluate_object(handle, obj_name, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		dev_err(&client->dev, "object %s not found\n", obj_name);
		return -ENODEV;
	}

	cpm = buffer.pointer;
	for (i = 0; i < cpm->package.count && i < count; ++i) {
		union acpi_object *elem;

		elem = &(cpm->package.elements[i]);
		vals[i] = elem->integer.value;
	}

	kfree(buffer.pointer);

	return i;
}

static void cm36672_parse_acpi(struct cm36672_chip *chip)
{
	struct i2c_client *client = chip->client;
	int cpm_elem_count, i;
	u64 cpm_elems[20];

	cpm_elem_count = cm36672_acpi_get_cpm_info(client, "CPM0",
				ARRAY_SIZE(cpm_elems), cpm_elems);

	if (cpm_elem_count > 0) {
		int header_num = 3;
		int regs_bmp = cpm_elems[2];
		int reg_num = cpm_elem_count - header_num;

		if (reg_num > CM36672_REGS_NUM)
			reg_num = CM36672_REGS_NUM;
		for (i = 0; i < reg_num; i++)
			if (regs_bmp & (1 << i))
				chip->regs[i] = cpm_elems[header_num + i];
	}
}

#endif

static int cm36672_regfield_init(struct cm36672_chip *chip)
{
	struct device *dev = &chip->client->dev;
	struct regmap *regmap = chip->regmap;

	chip->reg_prx_int_lo = devm_regmap_field_alloc(dev, regmap,
					cm36672_reg_field_prx_int_lo);
	if (IS_ERR(chip->reg_prx_int_lo)) {
		dev_err(dev, "%s: reg_prx_int_lo init failed\n", __func__);
		return PTR_ERR(chip->reg_prx_int_lo);
	}

	chip->reg_prx_int_hi = devm_regmap_field_alloc(dev, regmap,
					cm36672_reg_field_prx_int_hi);
	if (IS_ERR(chip->reg_prx_int_hi)) {
		dev_err(dev, "%s: reg_prx_int_hi init failed\n", __func__);
		return PTR_ERR(chip->reg_prx_int_hi);
	}

	chip->reg_prx_it = devm_regmap_field_alloc(dev, regmap,
					cm36672_reg_field_prx_it);
	if (IS_ERR(chip->reg_prx_it)) {
		dev_err(dev, "%s: reg_prx_it init failed\n", __func__);
		return PTR_ERR(chip->reg_prx_it);
	}

	return 0;
}

/**
 * cm36672_setup_reg() - Initialize sensor to default values.
 * @chip:	pointer of struct cm36672_chip
 *
 * Return: 0 for success; otherwise an error code.
 */
static int cm36672_setup_reg(struct cm36672_chip *chip)
{
	int i, reg, ret;
	u16 prx_conf;

	memcpy((char *)&chip->regs, (char *)&cm36672_regs_default,
		sizeof(cm36672_regs_default));

#ifdef CONFIG_OF
	if (chip->client->dev.of_node)
		cm36672_parse_dt(chip);
#endif

#ifdef CONFIG_ACPI
	if (ACPI_HANDLE(&chip->client->dev))
		cm36672_parse_acpi(chip);
#endif

	/* Store regs[CM36672_ADDR_PRX_CONF] */
	prx_conf = chip->regs[CM36672_ADDR_PRX_CONF];

	/* Disable INT when initialize registers */
	chip->regs[CM36672_ADDR_PRX_CONF] &= ~CM36672_PRX_INT_THDH;
	chip->regs[CM36672_ADDR_PRX_CONF] &= ~CM36672_PRX_INT_THDL;

	for (i = 0; i < CM36672_REGS_NUM; i++) {
		ret = regmap_write(chip->regmap, i, chip->regs[i]);
		if (ret < 0)
			return ret;
	}

	/* Restore regs[CM36672_ADDR_PRX_CONF] */
	chip->regs[CM36672_ADDR_PRX_CONF] = prx_conf;

	/* Force to clear flags */
	ret = regmap_read(chip->regmap, CM36672_ADDR_STATUS, &reg);
	if (ret < 0) {
		dev_err(&chip->client->dev,
			"%s: Failed to read Status register, err= %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

/**
 * cm36672_irq_handler() - Interrupt handling routine.
 * @irq:	irq number
 * @private:	pointer of void
 *
 * Return: IRQ_HANDLED.
 */
static irqreturn_t cm36672_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct cm36672_chip *chip = iio_priv(indio_dev);
	s64 timestamp = iio_get_time_ns();
	int ret, status;

	ret = regmap_read(chip->regmap, CM36672_ADDR_STATUS, &status);
	if (ret < 0)
		return IRQ_HANDLED;

	if (status & CM36672_INT_PRX_CLOSE)
		iio_push_event(indio_dev,
				IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						IIO_EV_TYPE_THRESH,
						IIO_EV_DIR_RISING),
				timestamp);

	if (status & CM36672_INT_PRX_AWAY)
		iio_push_event(indio_dev,
				IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						IIO_EV_TYPE_THRESH,
						IIO_EV_DIR_FALLING),
				timestamp);

	return IRQ_HANDLED;
}

/**
 * cm36672_read_prx_it() - Get the current proximity integration time.
 * @chip:	point of struct cm36672_chip
 * @val:	point of the integer part of integration time
 * @val2:	point of the micro part of integration time
 *
 * Return: IIO_VAL_INT_PLUS_MICRO for success; otherwise an error code.
 */
static int cm36672_read_prx_it(struct cm36672_chip *chip, int *val, int *val2)
{
	int ret, index;

	ret = regmap_field_read(chip->reg_prx_it, &index);
	if (ret < 0)
		return ret;

	if (index < 0 || index >= ARRAY_SIZE(cm36672_prx_it_scales))
		return -EINVAL;

	*val = cm36672_prx_it_scales[index].val;
	*val2 = cm36672_prx_it_scales[index].val2;

	return IIO_VAL_INT_PLUS_MICRO;
}

/**
 * cm36672_write_prx_it() - Set the proximity integration time.
 * @chip:	point of struct cm36672_chip
 * @val:	the integer part of integration time
 * @val2:	the micro part of integration time
 *
 * Return: 0 for success; otherwise an error code.
 */
static int cm36672_write_prx_it(struct cm36672_chip *chip, int val, int val2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cm36672_prx_it_scales); i++)
		if (val == cm36672_prx_it_scales[i].val &&
				val2 == cm36672_prx_it_scales[i].val2)
			return regmap_field_write(chip->reg_prx_it, i);

	return -EINVAL;
}

/**
 * cm36672_prx_read() - Read from proximity sensor.
 * @indio_dev:	point of struct iio_dev
 * @chan:	point of struct iio_chan_spec
 * @val:	point of integer
 * @val2:	point of integer
 * @mask:	iio_chan_info_enum
 *
 * Return: IIO_VAL type for success; otherwise an error code.
 */
static int cm36672_prx_read(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int *val, int *val2, long mask)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return cm36672_read_prx_it(chip, val, val2);
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(chip->regmap, CM36672_ADDR_PRX, val);
		if (ret < 0)
			return ret;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

/**
 * cm36672_prx_write() - Write to proximity sensor.
 * @indio_dev:	point of struct iio_dev
 * @chan:	point of struct iio_chan_spec
 * @val:	integer for integer part
 * @val2:	integer for micro part
 * @mask:	iio_chan_info_enum
 *
 * Return: Equal or great than 0 for success; otherwise an error code.
 */
static int cm36672_prx_write(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int val, int val2, long mask)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return cm36672_write_prx_it(chip, val, val2);
	default:
		return -EINVAL;
	}
}

static int cm36672_read_event(struct iio_dev *indio_dev,
			const struct iio_chan_spec *chan,
			enum iio_event_type type,
			enum iio_event_direction dir,
			enum iio_event_info info,
			int *val, int *val2)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);
	int ret = -EINVAL;

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	if (chan->type == IIO_PROXIMITY) {
		*val2 = 0;
		if (dir == IIO_EV_DIR_RISING)
			ret = regmap_read(chip->regmap, CM36672_ADDR_PRX_THDH,
					val);
		else if (dir == IIO_EV_DIR_FALLING)
			ret = regmap_read(chip->regmap, CM36672_ADDR_PRX_THDL,
					val);
	}

	if (ret < 0)
		return ret;

	return IIO_VAL_INT;
}

static int cm36672_write_event(struct iio_dev *indio_dev,
			const struct iio_chan_spec *chan,
			enum iio_event_type type,
			enum iio_event_direction dir,
			enum iio_event_info info,
			int val, int val2)
{
	int reg, ret = 0;
	int value, max;
	struct cm36672_chip *chip = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	if (chan->type == IIO_PROXIMITY) {
		ret = regmap_read(chip->regmap, CM36672_ADDR_PRX_CONF, &value);
		max = (value & CM36672_PRX_HD) ? 65535:4095;
		if (val < 0 || val > max)
			return -EINVAL;
		if (dir == IIO_EV_DIR_RISING)
			reg = CM36672_ADDR_PRX_THDH;
		else if (dir == IIO_EV_DIR_FALLING)
			reg = CM36672_ADDR_PRX_THDL;
		else
			return -EINVAL;

		ret = regmap_write(chip->regmap, reg, val);
		if (ret < 0)
			return ret;
	} else
		return -EINVAL;

	return 0;
}

static int cm36672_read_event_config(struct iio_dev *indio_dev,
			const struct iio_chan_spec *chan,
			enum iio_event_type type,
			enum iio_event_direction dir)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);
	int ret, state;

	if (chan->type == IIO_PROXIMITY) {
		if (dir == IIO_EV_DIR_RISING)
			ret = regmap_field_read(chip->reg_prx_int_hi, &state);
		else if (dir == IIO_EV_DIR_FALLING)
			ret = regmap_field_read(chip->reg_prx_int_lo, &state);
		else
			return -EINVAL;

		if (ret)
			return ret;

		return state;
	}

	return -EINVAL;
}

static int cm36672_write_event_config(struct iio_dev *indio_dev,
			const struct iio_chan_spec *chan,
			enum iio_event_type type,
			enum iio_event_direction dir,
			int state)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);
	int ret;

	if (chan->type == IIO_PROXIMITY) {
		if (dir == IIO_EV_DIR_RISING)
			ret = regmap_field_write(chip->reg_prx_int_hi, state);
		else if (dir == IIO_EV_DIR_FALLING)
			ret = regmap_field_write(chip->reg_prx_int_lo, state);
		else
			return -EINVAL;

		if (ret)
			return ret;

		return 0;
	}

	return -EINVAL;
}

static const struct iio_event_spec cm36672_prx_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec cm36672_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
			BIT(IIO_CHAN_INFO_INT_TIME),
		.channel = 0,
		.indexed = 0,
		.scan_index = -1,
		.event_spec = cm36672_prx_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36672_prx_event_spec),
	},
};

static bool cm36672_is_writeable_reg(struct device *dev, unsigned int reg)
{
	if (reg <= CM36672_ADDR_PRX_THDH)
		return true;
	return false;
}

static IIO_CONST_ATTR(in_proximity_integration_time_available,
		CM36672_PRX_INT_TIME_AVAIL);

static struct attribute *cm36672_attributes[] = {
	&iio_const_attr_in_proximity_integration_time_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group cm36672_attribute_group = {
	.attrs = cm36672_attributes
};

static const struct iio_info cm36672_info = {
	.driver_module		= THIS_MODULE,
	.read_raw		= &cm36672_prx_read,
	.write_raw		= &cm36672_prx_write,
	.attrs			= &cm36672_attribute_group,
	.read_event_value	= cm36672_read_event,
	.write_event_value	= cm36672_write_event,
	.read_event_config	= cm36672_read_event_config,
	.write_event_config	= cm36672_write_event_config,
};

static const struct iio_info cm36672_info_no_irq = {
	.driver_module		= THIS_MODULE,
	.read_raw		= &cm36672_prx_read,
	.write_raw		= &cm36672_prx_write,
	.attrs			= &cm36672_attribute_group,
};

static const struct regmap_config cm36672_regmap_config = {
	.name = CM36672_REGMAP_NAME,
	.reg_bits = 8,
	.val_bits = 16,
	.writeable_reg = cm36672_is_writeable_reg,
	.use_single_rw = true,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int cm36672_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cm36672_chip *chip;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &cm36672_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "%s: regmap initialize failed\n",
			__func__);
		return PTR_ERR(regmap);
	}

	chip = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	chip->client = client;
	chip->regmap = regmap;

	mutex_init(&chip->lock);
	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = cm36672_channels;
	indio_dev->num_channels = ARRAY_SIZE(cm36672_channels);
	indio_dev->name = CM36672_DRIVER_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = cm36672_regfield_init(chip);
	if (ret) {
		dev_err(&client->dev, "%s: regfield init failed\n", __func__);
		return ret;
	}

	ret = cm36672_setup_reg(chip);
	if (ret) {
		dev_err(&client->dev, "%s: register setup failed\n", __func__);
		return ret;
	}

	if (client->irq) {
		indio_dev->info = &cm36672_info;

		ret = request_threaded_irq(client->irq, NULL,
					cm36672_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"cm36672", indio_dev);
		if (ret) {
			dev_err(&client->dev,
				"%s: request irq failed\n",
				__func__);
			return ret;
		}

		/* Enable interrupt if default request*/
		if (chip->regs[CM36672_ADDR_PRX_CONF] & CM36672_PRX_INT_MASK) {
			ret = regmap_write(chip->regmap, CM36672_ADDR_PRX_CONF,
					chip->regs[CM36672_ADDR_PRX_CONF]);
			if (ret) {
				dev_err(&client->dev,
					"%s: enable interrupt failed\n",
					__func__);
				return ret;
			}
		}
	} else
		indio_dev->info = &cm36672_info_no_irq;

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev,
			"%s: registering device failed\n",
			__func__);
		if (client->irq)
			free_irq(client->irq, indio_dev);
	}

	return 0;
}

static int cm36672_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct cm36672_chip *chip = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	regmap_update_bits(chip->regmap, CM36672_ADDR_PRX_CONF, 1, 1);
	if (client->irq)
		free_irq(client->irq, indio_dev);

	return 0;
}

static const struct i2c_device_id cm36672_id[] = {
	{ "cm36672", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, cm36672_id);

static const struct of_device_id cm36672_of_match[] = {
	{ .compatible = "capella,cm36672" },
	{ }
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id cm36672_acpi_match[] = {
	{ "CPLM6672", 0},
	{},
};

MODULE_DEVICE_TABLE(acpi, cm36672_acpi_match);
#endif

#ifdef CONFIG_PM_SLEEP
static int cm36672_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct cm36672_chip *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int i, ret;

	for (i = 0; i < CM36672_REGS_NUM; i++) {
		ret = i2c_smbus_read_word_data(client, i);
		if (ret < 0)
			return ret;
		chip->regs[i] = ret;
	}

	return regmap_update_bits(chip->regmap, CM36672_ADDR_PRX_CONF, 1, 1);
}

static int cm36672_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct cm36672_chip *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int i, ret;

	for (i = 0; i < CM36672_REGS_NUM; i++) {
		ret = i2c_smbus_write_word_data(client, i, chip->regs[i]);
		if (ret < 0)
			return ret;
	}

	return regmap_update_bits(chip->regmap, CM36672_ADDR_PRX_CONF, 1, 0);
}

static const struct dev_pm_ops cm36672_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cm36672_suspend, cm36672_resume)};
#endif

static struct i2c_driver cm36672_driver = {
	.driver = {
		.name	= CM36672_DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = cm36672_of_match,
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(cm36672_acpi_match),
#endif
#ifdef CONFIG_PM_SLEEP
		.pm	= &cm36672_pm_ops,
#endif
	},
	.id_table	= cm36672_id,
	.probe		= cm36672_probe,
	.remove		= cm36672_remove,
};

module_i2c_driver(cm36672_driver);

MODULE_AUTHOR("Kevin Tsai <capellamicro@gmail.com>");
MODULE_DESCRIPTION("CM36672 proximity sensor driver");
MODULE_LICENSE("GPL v2");
