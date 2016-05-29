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
#include <linux/gpio.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>

#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#endif

/* Sensor registers */
#define CM36672_ADDR_RESERVED00		0x00
#define CM36672_ADDR_RESERVED01		0x01
#define CM36672_ADDR_RESERVED02		0x02
#define CM36672_ADDR_PRX_CONF		0x03
#define CM36672_ADDR_PRX_CONF3		0x04
#define CM36672_ADDR_PRX_CANC		0x05
#define CM36672_ADDR_PRX_THDL		0x06
#define CM36672_ADDR_PRX_THDH		0x07
#define CM36672_REGS_NUM		0x08
/* Read only registers */
#define CM36672_ADDR_PRX		0x08
#define CM36672_ADDR_STATUS		0x0B

/* RESERVED00 */
#define CM36672_RESERVED00_DEFAULT	0x0001

/* RESERVED01 */
#define CM36672_RESERVED01_DEFAULT	0x0000

/* RESERVED02 */
#define CM36672_RESERVED02_DEFAULT	0x0000

/* PRX_CONF */
#define CM36672_PRX_DISABLE		0x0001
#define CM36672_PRX_INT_RISING		BIT(8)
#define CM36672_PRX_INT_FALLING		BIT(9)
#define CM36672_PRX_INT_MASK		(CM36672_PRX_INT_RISING |	\
					CM36672_PRX_INT_FALLING)

/* PRX_CONF: duty ratio */
#define CM36672_PRX_DR_MASK		(BIT(6) | BIT(7))
#define CM36672_PRX_DR_SHIFT		6
#define CM36672_PRX_DR1			0x0000 /* Duty ratio 1/40 */
#define CM36672_PRX_DR2			0x0040 /* Duty ratio 1/80 */
#define CM36672_PRX_DR3			0x0080 /* Duty ratio 1/160 */
#define CM36672_PRX_DR4			0x00C0 /* Duty ratio 1/320 */

/* PRX_CONF: persistence */
#define CM36672_PRX_PERS_MASK		(BIT(4) | BIT(5))
#define CM36672_PRX_PERS_SHIFT		4
#define CM36672_PRX_PERS2		0x0010
#define CM36672_PRX_PERS3		0x0020
#define CM36672_PRX_PERS4		0x0030

/* PRX_CONF: integration time */
#define CM36672_PRX_IT_MASK		(BIT(1) | BIT(2) | BIT(3))
#define CM36672_PRX_IT_SHIFT		1
#define CM36672_PRX_IT0			0x0000 /* 1T */
#define CM36672_PRX_IT1			0x0002 /* 1.5T */
#define CM36672_PRX_IT2			0x0004 /* 2T */
#define CM36672_PRX_IT3			0x0006 /* 2.5T */
#define CM36672_PRX_IT4			0x0008 /* 3T */
#define CM36672_PRX_IT5			0x000A /* 3.5T */
#define CM36672_PRX_IT6			0x000C /* 4T */
#define CM36672_PRX_IT7			0x000E /* 8T */

#define CM36672_PRX_CONF_DEFAULT	(CM36672_PRX_IT2 |		\
					CM36672_PRX_PERS3)

/* PRX_CONF3 */
#define CM36672_PRX_LED_I_MASK		(BIT(8) | BIT(9) | BIT(10))
#define CM36672_PRX_LED_I_SHIFT		8
#define CM36672_PRX_LED_I0		0x0000 /* 50mA */
#define CM36672_PRX_LED_I1		0x0100 /* 75mA */
#define CM36672_PRX_LED_I2		0x0200 /* 100mA */
#define CM36672_PRX_LED_I3		0x0300 /* 120mA */
#define CM36672_PRX_LED_I4		0x0400 /* 140mA */
#define CM36672_PRX_LED_I5		0x0500 /* 160mA */
#define CM36672_PRX_LED_I6		0x0600 /* 180mA */
#define CM36672_PRX_LED_I7		0x0700 /* 200mA */

#define CM36672_PRX_CONF3_DEFAULT	CM36672_PRX_LED_I0

/* PRX_CANC */
#define CM36672_PRX_CANC_DEFAULT	0x0000

/* PRX_THDL */
#define CM36672_PRX_THDL_DEFAULT	0x0005

/* PRX_THDH */
#define CM36672_PRX_THDH_DEFAULT	0x000A

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
	CM36672_RESERVED00_DEFAULT,
	CM36672_RESERVED01_DEFAULT,
	CM36672_RESERVED02_DEFAULT,
	CM36672_PRX_CONF_DEFAULT,
	CM36672_PRX_CONF3_DEFAULT,
	CM36672_PRX_CANC_DEFAULT,
	CM36672_PRX_THDL_DEFAULT,
	CM36672_PRX_THDH_DEFAULT,
};

struct cm36672_chip {
	const struct cm36672_platform_data *pdata;
	struct i2c_client *client;
	struct mutex lock;
	u16 regs[CM36672_REGS_NUM];
};

#define CM36672_DRIVER_NAME	"cm36672"

#ifdef CONFIG_OF
void cm36672_mod_u16(u16 *reg, u16 mask, u8 shift, u16 val)
{
	*reg &= ~mask;
	*reg |= (u16)(val << shift);
}

void cm36672_parse_dt(struct cm36672_chip *chip)
{
	struct device_node *dn = chip->client->dev.of_node;
	u32 temp_val;

	/* parse by registers */
	if (!of_property_read_u32(dn, "capella,reg_prx_conf", &temp_val))
		chip->regs[CM36672_ADDR_PRX_CONF] = (u16)temp_val;
	if (!of_property_read_u32(dn, "capella,reg_prx_conf3", &temp_val))
		chip->regs[CM36672_ADDR_PRX_CONF3] = (u16)temp_val;
	if (!of_property_read_u32(dn, "capella,reg_prx_canc", &temp_val))
		chip->regs[CM36672_ADDR_PRX_CANC] = (u16)temp_val;
	if (!of_property_read_u32(dn, "capella,reg_prx_thdl", &temp_val))
		chip->regs[CM36672_ADDR_PRX_THDL] = (u16)temp_val;
	if (!of_property_read_u32(dn, "capella,reg_prx_thdh", &temp_val))
		chip->regs[CM36672_ADDR_PRX_THDH] = (u16)temp_val;

	/* parse by name */
	if (!of_property_read_u32(dn, "capella,name_prx_int_rising", &temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_INT_RISING, 0,
				temp_val ? CM36672_PRX_INT_RISING : 0);
	if (!of_property_read_u32(dn, "capella,name_prx_int_falling",
				&temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_INT_FALLING, 0,
				temp_val ? CM36672_PRX_INT_FALLING : 0);
	if (!of_property_read_u32(dn, "capella,name_prx_duty", &temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_DR_MASK,
				CM36672_PRX_DR_SHIFT,
				(u16)temp_val);
	if (!of_property_read_u32(dn, "capella,name_prx_pers", &temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_PERS_MASK,
				CM36672_PRX_PERS_SHIFT,
				(u16)temp_val);
	if (!of_property_read_u32(dn, "capella,name_prx_it", &temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF],
				CM36672_PRX_IT_MASK,
				CM36672_PRX_IT_SHIFT,
				(u16)temp_val);
	if (!of_property_read_u32(dn, "capella,name_prx_led_current",
				&temp_val))
		cm36672_mod_u16(&chip->regs[CM36672_ADDR_PRX_CONF3],
				CM36672_PRX_LED_I_MASK,
				CM36672_PRX_LED_I_SHIFT,
				(u16)temp_val);
	if (!of_property_read_u32(dn, "capella,name_prx_threshold_rising",
				&temp_val))
		chip->regs[CM36672_ADDR_PRX_THDH] = (u16)temp_val;
	if (!of_property_read_u32(dn, "capella,name_prx_threshold_falling",
				&temp_val))
		chip->regs[CM36672_ADDR_PRX_THDL] = (u16)temp_val;
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
 * Convert ACPI CPM table to array. Special thanks Srinivas Pandruvada's
 * help to implement this routine.
 *
 * Return: -ENODEV for fail. Otherwise is number of elements.
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

void cm36672_parse_acpi(struct cm36672_chip *chip)
{
	struct i2c_client *client = chip->client;
	int cpm_elem_count, i;
	u64 cpm_elems[20];

	cpm_elem_count = cm36672_acpi_get_cpm_info(client, "CPM0",
						ARRAY_SIZE(cpm_elems),
						cpm_elems);

	if (cpm_elem_count > 0) {
		int header_num = 3;
		int regs_bmp = cpm_elems[2];
		int reg_num = cpm_elem_count - header_num;

		if (reg_num > CM36672_REGS_NUM)
			reg_num = CM36672_REGS_NUM;
		for (i = 0; i < reg_num; i++)
			if (regs_bmp & (1 << i))
				chip->regs[i] =
					cpm_elems[header_num + i];
	}

}

#endif

/**
 * cm36672_setup_reg() - Initialize sensor to default values.
 * @chip:	pointer of struct cm36672_chip
 *
 * Return: 0 for success; otherwise for error code.
 */
static int cm36672_setup_reg(struct cm36672_chip *chip)
{
	int i, ret;
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
	chip->regs[CM36672_ADDR_PRX_CONF] &= ~CM36672_PRX_INT_RISING;
	chip->regs[CM36672_ADDR_PRX_CONF] &= ~CM36672_PRX_INT_FALLING;

	for (i = 0; i < CM36672_REGS_NUM; i++) {
		ret = i2c_smbus_write_word_data(chip->client, i,
						chip->regs[i]);
		if (ret < 0)
			return ret;
	}

	/* Restore regs[CM36672_ADDR_PRX_CONF] */
	chip->regs[CM36672_ADDR_PRX_CONF] = prx_conf;

	/* Force to clear flags */
	ret = i2c_smbus_read_word_data(chip->client, CM36672_ADDR_STATUS);
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
 * Return: 0 for success; otherwise for error code.
 */
static irqreturn_t cm36672_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct cm36672_chip *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ev_dir, ret;
	u64 ev_code;

	ret = i2c_smbus_read_word_data(chip->client, CM36672_ADDR_STATUS);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: Data read failed: %d\n", __func__, ret);
		return IRQ_HANDLED;
	}
	ev_dir = 0;
	if (ret & CM36672_INT_PRX_CLOSE)
		ev_dir |= IIO_EV_DIR_RISING;
	if (ret & CM36672_INT_PRX_AWAY)
		ev_dir |= IIO_EV_DIR_FALLING;

	ev_code = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
				IIO_EV_TYPE_THRESH, ev_dir);

	iio_push_event(indio_dev, ev_code, iio_get_time_ns());

	return IRQ_HANDLED;
}

/**
 * cm36672_read_prx_it() - Get the current proximity integration time.
 * @chip:	point of struct cm36672_chip
 * @val:	point of the integer part of integration time
 * @val2:	point of the micro part of integration time
 *
 * Return: IIO_VAL_INT_PLUS_MICRO for success; otherwise for error code.
 */
static int cm36672_read_prx_it(struct cm36672_chip *chip, int *val, int *val2)
{
	u16 prx_it;
	int i;

	prx_it = chip->regs[CM36672_ADDR_PRX_CONF];
	prx_it &= CM36672_PRX_IT_MASK;
	prx_it >>= CM36672_PRX_IT_SHIFT;
	for (i = 0; i < ARRAY_SIZE(cm36672_prx_it_scales); i++) {
		if (prx_it == cm36672_prx_it_scales[i].it) {
			*val = cm36672_prx_it_scales[i].val;
			*val2 = cm36672_prx_it_scales[i].val2;
			return IIO_VAL_INT_PLUS_MICRO;
		}
	}

	return -EINVAL;
}

/**
 * cm36672_write_prx_it() - Set the proximity integration time.
 * @chip:	point of struct cm36672_chip
 * @val:	the integer part of integration time
 * @val2:	the micro part of integration time
 *
 * Return: 0 for success; otherwise for error code.
 */
static int cm36672_write_prx_it(struct cm36672_chip *chip, int val, int val2)
{
	struct i2c_client *client = chip->client;
	u16 prx_it, cmd;
	int i;
	s32 ret;

	for (i = 0; i < ARRAY_SIZE(cm36672_prx_it_scales); i++) {
		if (val == cm36672_prx_it_scales[i].val &&
			val2 == cm36672_prx_it_scales[i].val2) {

			prx_it = cm36672_prx_it_scales[i].it;
			prx_it <<= CM36672_PRX_IT_SHIFT;

			cmd = chip->regs[CM36672_ADDR_PRX_CONF];
			cmd &= ~CM36672_PRX_IT_MASK;
			cmd |= prx_it;
			ret = i2c_smbus_write_byte_data(client,
				CM36672_ADDR_PRX_CONF,
				cmd);
			if (ret < 0)
				return ret;
			chip->regs[CM36672_ADDR_PRX_CONF] = cmd;
			return 0;
		}
	}
	return -ERANGE;
}

/**
 * cm36672_prx_read() - Read from proximity sensor.
 * @indio_dev:	point of struct iio_dev
 * @chan:	point of struct iio_chan_spec
 * @val:	point of integer
 * @val2:	point of integer
 * @mask:	iio_chan_info_enum
 *
 * Return: IIO_VAL type for success; otherwise for error code.
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
		*val = 0;
		ret = cm36672_read_prx_it(chip, val, val2);
		break;
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_OFFSET:
		ret = i2c_smbus_read_word_data(
			chip->client,
			mask == IIO_CHAN_INFO_RAW ?
			CM36672_ADDR_PRX : CM36672_ADDR_PRX_CANC);
		if (ret >= 0) {
			*val = ret;
			ret = IIO_VAL_INT;
		}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/**
 * cm36672_prx_write() - Write to proximity sensor.
 * @indio_dev:	point of struct iio_dev
 * @chan:	point of struct iio_chan_spec
 * @val:	point of integer for integer part
 * @val2:	point of integer for micro part
 * @mask:	point of iio_chan_info_enum
 *
 * Return: Equal or great than 0 for success; otherwise for error code.
 */
static int cm36672_prx_write(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int val, int val2, long mask)
{
	struct cm36672_chip *chip = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return cm36672_write_prx_it(chip, val, val2);
	case IIO_CHAN_INFO_OFFSET:
		chip->regs[CM36672_ADDR_PRX_CANC] = val;
		return i2c_smbus_write_word_data(
				chip->client,
				CM36672_ADDR_PRX_CANC,
				chip->regs[CM36672_ADDR_PRX_CANC]);
	}

	return -EINVAL;
}

enum cm36672_prx_attribute_type {
	CM36672_ATTR_TYPE_THRESH_FALLING_EN,
	CM36672_ATTR_TYPE_THRESH_FALLING,
	CM36672_ATTR_TYPE_THRESH_RISING_EN,
	CM36672_ATTR_TYPE_THRESH_RISING,
};

struct cm36672_prx_attribute {
	struct device_attribute dev_attr;
	enum cm36672_prx_attribute_type type;
};

static inline struct cm36672_prx_attribute *
to_cm36672_prx_attr(struct device_attribute *attr)
{
	return container_of(attr, struct cm36672_prx_attribute, dev_attr);
}

/**
 * show_prx_attr() - Show event attribute.
 * @dev:	point of struct device
 * @attr:	point of struct device_attribute
 * @buf:	point of buffer
 *
 * Return: length of buffer; Otherwise for error code.
 */
static ssize_t show_prx_attr(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct cm36672_chip *chip = iio_priv(indio_dev);
	struct cm36672_prx_attribute *prx_attr = to_cm36672_prx_attr(attr);
	u16 val;

	switch (prx_attr->type) {
	case CM36672_ATTR_TYPE_THRESH_FALLING_EN:
		val = (chip->regs[CM36672_ADDR_PRX_CONF] &
			CM36672_PRX_INT_FALLING) ? 1 : 0;
		break;
	case CM36672_ATTR_TYPE_THRESH_RISING_EN:
		val = (chip->regs[CM36672_ADDR_PRX_CONF] &
			CM36672_PRX_INT_RISING) ? 1 : 0;
		break;
	case CM36672_ATTR_TYPE_THRESH_FALLING:
		val = chip->regs[CM36672_ADDR_PRX_THDL];
		break;
	case CM36672_ATTR_TYPE_THRESH_RISING:
		val = chip->regs[CM36672_ADDR_PRX_THDH];
		break;
	default:
		return -ENXIO;
	}

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

/**
 * store_prx_attr() - Store to event attribute.
 * @dev:	point of struct device
 * @attr:	point of struct device_attribute
 * @buf:	point of buffer
 * @len:	length of buffer
 *
 * Return: length of buffer; otherwise for error code.
 */
static ssize_t store_prx_attr(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct cm36672_chip *chip = iio_priv(indio_dev);
	struct cm36672_prx_attribute *prx_attr = to_cm36672_prx_attr(attr);
	u16 val, cmd;
	int ret;

	if (kstrtou16(buf, 0, &val))
		return -EINVAL;

	mutex_lock(&chip->lock);
	switch (prx_attr->type) {
	case CM36672_ATTR_TYPE_THRESH_FALLING_EN:
		cmd = chip->regs[CM36672_ADDR_PRX_CONF];
		if (!val)
			cmd &= ~CM36672_PRX_INT_FALLING;
		else
			cmd |= CM36672_PRX_INT_FALLING;
		ret = i2c_smbus_write_word_data(chip->client,
				CM36672_ADDR_PRX_CONF,
				cmd);
		if (!ret)
			chip->regs[CM36672_ADDR_PRX_CONF] = cmd;
		break;
	case CM36672_ATTR_TYPE_THRESH_RISING_EN:
		cmd = chip->regs[CM36672_ADDR_PRX_CONF];
		if (!val)
			cmd &= ~CM36672_PRX_INT_RISING;
		else
			cmd |= CM36672_PRX_INT_RISING;
		ret = i2c_smbus_write_word_data(chip->client,
				CM36672_ADDR_PRX_CONF,
				cmd);
		if (!ret)
			chip->regs[CM36672_ADDR_PRX_CONF] = cmd;
		break;
	case CM36672_ATTR_TYPE_THRESH_FALLING:
		if (val > chip->regs[CM36672_ADDR_PRX_THDH]) {
			ret = -ERANGE;
			break;
		}
		ret = i2c_smbus_write_word_data(chip->client,
				CM36672_ADDR_PRX_THDL,
				val);
		if (!ret)
			chip->regs[CM36672_ADDR_PRX_THDL] = val;
		break;
	case CM36672_ATTR_TYPE_THRESH_RISING:
		if (val < chip->regs[CM36672_ADDR_PRX_THDL]) {
			ret = -ERANGE;
			break;
		}
		ret = i2c_smbus_write_word_data(chip->client,
				CM36672_ADDR_PRX_THDH,
				val);
		if (!ret)
			chip->regs[CM36672_ADDR_PRX_THDH] = val;
		break;
	default:
		ret = -ENXIO;
	}
	mutex_unlock(&chip->lock);
	if (ret < 0)
		return ret;

	return len;
}

#define PRX_ATTR(_name, _mode, _show, _store, _type)			\
	{	.dev_attr	= __ATTR(_name, _mode, _show, _store),	\
		.type		= _type }

#define CM36672_PRX_ATTR(_name, _mode, _show, _store, _type)		\
	struct cm36672_prx_attribute cm36672_prx_attr_##_name =		\
		PRX_ATTR(_name, _mode, _show, _store, _type)

static CM36672_PRX_ATTR(in_proximity_thresh_falling_en,
			S_IRUGO | S_IWUSR,
			show_prx_attr,
			store_prx_attr,
			CM36672_ATTR_TYPE_THRESH_FALLING_EN);

static CM36672_PRX_ATTR(in_proximity_thresh_rising_en,
			S_IRUGO | S_IWUSR,
			show_prx_attr,
			store_prx_attr,
			CM36672_ATTR_TYPE_THRESH_RISING_EN);

static CM36672_PRX_ATTR(in_proximity_thresh_falling_value,
			S_IRUGO | S_IWUSR,
			show_prx_attr,
			store_prx_attr,
			CM36672_ATTR_TYPE_THRESH_FALLING);

static CM36672_PRX_ATTR(in_proximity_thresh_rising_value,
			S_IRUGO | S_IWUSR,
			show_prx_attr,
			store_prx_attr,
			CM36672_ATTR_TYPE_THRESH_RISING);

static struct attribute *cm36672_prx_event_attributes[] = {
	&cm36672_prx_attr_in_proximity_thresh_falling_en.dev_attr.attr,
	&cm36672_prx_attr_in_proximity_thresh_rising_en.dev_attr.attr,
	&cm36672_prx_attr_in_proximity_thresh_falling_value.dev_attr.attr,
	&cm36672_prx_attr_in_proximity_thresh_rising_value.dev_attr.attr,
	NULL,
};

static struct attribute_group cm36672_prx_event_attribute_group = {
	.attrs = cm36672_prx_event_attributes
};

static const struct iio_event_spec cm36672_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	}
};

static const struct iio_chan_spec cm36672_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW),
			BIT(IIO_CHAN_INFO_INT_TIME),
			BIT(IIO_CHAN_INFO_OFFSET),
	},
};

static const struct iio_chan_spec cm36672_channels_no_irq[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_INT_TIME),
			BIT(IIO_CHAN_INFO_RAW),
			BIT(IIO_CHAN_INFO_OFFSET),
	},
};

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
	.event_attrs		= &cm36672_prx_event_attribute_group,
};

static const struct iio_info cm36672_info_no_irq = {
	.driver_module		= THIS_MODULE,
	.read_raw		= &cm36672_prx_read,
	.write_raw		= &cm36672_prx_write,
	.attrs			= &cm36672_attribute_group,
};

int irq;
module_param(irq, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(irq, "irq");

static int cm36672_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cm36672_chip *chip;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	chip->client = client;

	mutex_init(&chip->lock);
	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = cm36672_channels;
	indio_dev->num_channels = ARRAY_SIZE(cm36672_channels);
	indio_dev->name = CM36672_DRIVER_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = cm36672_setup_reg(chip);
	if (ret) {
		dev_err(&client->dev, "%s: register setup failed\n", __func__);
		return ret;
	}

	if (irq)
		client->irq = irq;
	indio_dev->info = (client->irq) ?
		&cm36672_info : &cm36672_info_no_irq;

	if (client->irq) {
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
			ret = i2c_smbus_write_word_data(chip->client,
					CM36672_ADDR_PRX_CONF,
					chip->regs[CM36672_ADDR_PRX_CONF]);
			if (ret) {
				dev_err(&client->dev,
					"%s: enable interrupt failed\n",
					__func__);
				return ret;
			}
		}
	}

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev,
			"%s: regist device failed\n",
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

	i2c_smbus_write_word_data(
			chip->client,
			CM36672_ADDR_PRX_CONF,
			CM36672_PRX_DISABLE);
	if (client->irq)
		free_irq(client->irq, indio_dev);
	iio_device_unregister(indio_dev);
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
	int ret;

	chip->regs[CM36672_ADDR_PRX_CONF] |= CM36672_PRX_DISABLE;
	ret = i2c_smbus_write_byte_data(
			chip->client,
			CM36672_ADDR_PRX_CONF,
			chip->regs[CM36672_ADDR_PRX_CONF]);
	return ret;
}

static int cm36672_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct cm36672_chip *chip = iio_priv(indio_dev);
	int i, ret;

	chip->regs[CM36672_ADDR_PRX_CONF] &= ~CM36672_PRX_DISABLE;

	/* Initialize registers*/
	for (i = 0; i < CM36672_REGS_NUM; i++) {
		ret = i2c_smbus_write_word_data(
				chip->client,
				i,
				chip->regs[i]);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static const struct dev_pm_ops cm36672_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cm36672_suspend, cm36672_resume)};
#endif

static struct i2c_driver cm36672_driver = {
	.driver = {
		.name	= CM36672_DRIVER_NAME,
		.of_match_table = cm36672_of_match,
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(cm36672_acpi_match),
#endif
		.owner	= THIS_MODULE,
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
