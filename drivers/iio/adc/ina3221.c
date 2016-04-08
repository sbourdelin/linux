/*
 * INA3221 Triple Current/Voltage Monitor
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define INA3221_DRIVER_NAME		"ina3221"

#define INA3221_CONFIG			0x00
#define INA3221_SHUNT1			0x01
#define INA3221_BUS1			0x02
#define INA3221_SHUNT2			0x03
#define INA3221_BUS2			0x04
#define INA3221_SHUNT3			0x05
#define INA3221_BUS3			0x06
#define INA3221_CRIT1			0x07
#define INA3221_WARN1			0x08
#define INA3221_CRIT2			0x09
#define INA3221_WARN2			0x0a
#define INA3221_CRIT3			0x0b
#define INA3221_WARN3			0x0c
#define INA3221_SHUNT_SUM		0x0d
#define INA3221_SHUNT_SUM_LIMIT		0x0e
#define INA3221_MASK_ENABLE		0x0f
#define INA3221_POWERV_HLIMIT		0x10
#define INA3221_POWERV_LLIMIT		0x11

#define INA3221_CONFIG_MODE_SHUNT	BIT(1)
#define INA3221_CONFIG_MODE_BUS		BIT(2)
#define INA3221_CONFIG_MODE_CONTINUOUS	BIT(3)

enum ina3221_fields {
	/* Configuration */
	F_MODE, F_SHUNT_CT, F_BUS_CT, F_AVG,
	F_CHAN3_EN, F_CHAN2_EN, F_CHAN1_EN, F_RST,

	/* sentinel */
	F_MAX_FIELDS
};

static const struct reg_field ina3221_reg_fields[] = {
	[F_MODE]	= REG_FIELD(INA3221_CONFIG, 0, 2),
	[F_SHUNT_CT]	= REG_FIELD(INA3221_CONFIG, 3, 5),
	[F_BUS_CT]	= REG_FIELD(INA3221_CONFIG, 6, 8),
	[F_AVG]		= REG_FIELD(INA3221_CONFIG, 9, 11),
	[F_CHAN3_EN]	= REG_FIELD(INA3221_CONFIG, 12, 12),
	[F_CHAN2_EN]	= REG_FIELD(INA3221_CONFIG, 13, 13),
	[F_CHAN1_EN]	= REG_FIELD(INA3221_CONFIG, 14, 14),
	[F_RST]		= REG_FIELD(INA3221_CONFIG, 15, 15),
};

#define is_bus_reg(_reg) \
	(_reg == INA3221_BUS1 || \
	 _reg == INA3221_BUS2 || \
	 _reg == INA3221_BUS3)

/**
 * struct ina3221_data - device specific information
 * @dev: Device structure
 * @regmap: Register map of the device
 * @fields: Register fields of the device
 */
struct ina3221_data {
	struct device *dev;
	struct regmap *regmap;
	struct regmap_field *fields[F_MAX_FIELDS];
};

/**
 * struct ina3221_reg_lookup - value element in iio lookup table map
 * @integer: Integer component of value
 * @fract: Fractional component of value
 */
struct ina3221_reg_lookup {
	int integer;
	int fract;
};

static const struct ina3221_reg_lookup ina3221_conv_time_table[] = {
	{.fract = 140}, {.fract = 204}, {.fract = 332}, {.fract = 588},
	{.fract = 1100}, {.fract = 2116}, {.fract = 4156}, {.fract = 8244},
};

static const int ina3221_avg_table[] = { 1, 4, 16, 64, 128, 256, 512, 1024 };
static IIO_CONST_ATTR(oversampling_ratio_available, "1 4 16 64 128 256 512 1024");

static int ina3221_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ina3221_data *ina = iio_priv(indio_dev);
	unsigned int regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(ina->regmap, chan->address, &regval);
		if (ret)
			return ret;

		*val = (s16)sign_extend32(regval >> 3, 12);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (is_bus_reg(chan->address)) {
			*val = 8;
			*val2 = 0;
		} else {
			*val = 0;
			*val2 = 40000;
		}

		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		ret = regmap_field_read(ina->fields[F_AVG], &regval);
		if (ret)
			return ret;

		*val = ina3221_avg_table[regval];

		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int ina3221_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ina3221_data *ina = iio_priv(indio_dev);
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return regmap_write(ina->regmap, chan->address, val << 3);

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		if (val2)
			return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(ina3221_avg_table); i++)
			if (ina3221_avg_table[i] == val)
				break;
		if (i == ARRAY_SIZE(ina3221_avg_table))
			return -EINVAL;

		return regmap_field_write(ina->fields[F_AVG], i);
	}

	return -EINVAL;
}

#define INA3221_CHAN(_channel, _address, _name) { \
	.type = IIO_VOLTAGE, \
	.channel = (_channel), \
	.address = (_address), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO), \
	.extend_name = _name, \
	.indexed = true, \
}

static const struct iio_chan_spec ina3221_channels[] = {
	INA3221_CHAN(1, INA3221_SHUNT1, "shunt"),
	INA3221_CHAN(1, INA3221_BUS1, "bus"),
	INA3221_CHAN(1, INA3221_CRIT1, "shunt_critical"),
	INA3221_CHAN(1, INA3221_WARN1, "shunt_warning"),

	INA3221_CHAN(2, INA3221_SHUNT2, "shunt"),
	INA3221_CHAN(2, INA3221_BUS2, "bus"),
	INA3221_CHAN(2, INA3221_CRIT2, "shunt_critical"),
	INA3221_CHAN(2, INA3221_WARN2, "shunt_warning"),

	INA3221_CHAN(3, INA3221_SHUNT3, "shunt"),
	INA3221_CHAN(3, INA3221_BUS3, "bus"),
	INA3221_CHAN(3, INA3221_CRIT3, "shunt_critical"),
	INA3221_CHAN(3, INA3221_WARN3, "shunt_warning"),
};

struct ina3221_attr {
	struct device_attribute dev_attr;
	struct device_attribute dev_attr_available;
	unsigned int field;
	const struct ina3221_reg_lookup *table;
	unsigned int table_size;
};

#define to_ina3221_attr(_dev_attr) \
	container_of(_dev_attr, struct ina3221_attr, dev_attr)

#define to_ina3221_attr_available(_dev_attr) \
	container_of(_dev_attr, struct ina3221_attr, dev_attr_available)

static ssize_t ina3221_show_register(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ina3221_data *ina = iio_priv(indio_dev);
	struct ina3221_attr *ina3221_attr = to_ina3221_attr(attr);
	unsigned int reg_val;
	int vals[2];
	int ret;

	ret = regmap_field_read(ina->fields[ina3221_attr->field], &reg_val);
	if (ret)
		return ret;

	vals[0] = ina3221_attr->table[reg_val].integer;
	vals[1] = ina3221_attr->table[reg_val].fract;

	return iio_format_value(buf, IIO_VAL_INT_PLUS_MICRO, 2, vals);
}

static ssize_t ina3221_store_register(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ina3221_data *ina = iio_priv(indio_dev);
	struct ina3221_attr *ina3221_attr = to_ina3221_attr(attr);
	long val;
	int integer, fract, ret;

	ret = iio_str_to_fixpoint(buf, 100000, &integer, &fract);
	if (ret)
		return ret;

	if (integer < 0)
		return -EINVAL;

	for (val = 0; val < ina3221_attr->table_size; val++)
		if (ina3221_attr->table[val].integer == integer &&
		    ina3221_attr->table[val].fract == fract) {
			ret = regmap_field_write(ina->fields[ina3221_attr->field], val);
			if (ret)
				return ret;

			return count;
		}

	return -EINVAL;
}

static ssize_t ina3221_show_available(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct ina3221_attr *ina3221_attr = to_ina3221_attr_available(attr);
	ssize_t len = 0;
	int i;

	for (i = 0; i < ina3221_attr->table_size; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%d.%06u ",
				 ina3221_attr->table[i].integer,
				 ina3221_attr->table[i].fract);

	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}

#define INA3221_ATTR(_name, _field, _table) \
	struct ina3221_attr ina3221_attr_##_name = { \
		.dev_attr = __ATTR(_name, (S_IRUGO | S_IWUSR), \
				   ina3221_show_register, \
				   ina3221_store_register), \
		.dev_attr_available = __ATTR(_name##_available, S_IRUGO, \
					     ina3221_show_available, NULL), \
		.field = _field, \
		.table = _table, \
		.table_size = ARRAY_SIZE(_table), \
	}

static INA3221_ATTR(shunt_integration_time, F_SHUNT_CT, ina3221_conv_time_table);
static INA3221_ATTR(bus_integration_time, F_BUS_CT, ina3221_conv_time_table);

static struct attribute *ina3221_attributes[] = {
	&ina3221_attr_shunt_integration_time.dev_attr.attr,
	&ina3221_attr_shunt_integration_time.dev_attr_available.attr,
	&ina3221_attr_bus_integration_time.dev_attr.attr,
	&ina3221_attr_bus_integration_time.dev_attr_available.attr,
	&iio_const_attr_oversampling_ratio_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ina3221_attribute_group = {
	.attrs = ina3221_attributes,
};

static const struct iio_info ina3221_iio_info = {
	.driver_module = THIS_MODULE,
	.attrs = &ina3221_attribute_group,
	.read_raw = ina3221_read_raw,
	.write_raw = ina3221_write_raw,
};

static const struct regmap_range ina3221_yes_ranges[] = {
	regmap_reg_range(INA3221_SHUNT1, INA3221_BUS3),
	regmap_reg_range(INA3221_MASK_ENABLE, INA3221_MASK_ENABLE),
};

static const struct regmap_access_table ina3221_volatile_table = {
	.yes_ranges = ina3221_yes_ranges,
	.n_yes_ranges = ARRAY_SIZE(ina3221_yes_ranges),
};

static const struct regmap_config ina3221_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,

	.cache_type = REGCACHE_RBTREE,
	.volatile_table = &ina3221_volatile_table,
};

#ifdef CONFIG_OF
static const struct of_device_id ina3221_of_match[] = {
	{ .compatible = "ti,ina3221", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ina3221_of_match);
#endif

static int ina3221_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct ina3221_data *ina;
	int i, ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*ina));
	if (!indio_dev)
		return -ENOMEM;
	i2c_set_clientdata(client, indio_dev);

	ina = iio_priv(indio_dev);

	ina->dev = &client->dev;

	ina->regmap = devm_regmap_init_i2c(client, &ina3221_regmap_config);
	if (IS_ERR(ina->regmap)) {
		dev_err(ina->dev, "Unable to allocate register map\n");
		return PTR_ERR(ina->regmap);
	}

	for (i = 0; i < F_MAX_FIELDS; i++) {
		ina->fields[i] = devm_regmap_field_alloc(ina->dev,
							 ina->regmap,
							 ina3221_reg_fields[i]);
		if (IS_ERR(ina->fields[i])) {
			dev_err(ina->dev, "Unable to allocate regmap fields\n");
			return PTR_ERR(ina->fields[i]);
		}
	}

	ret = regmap_field_write(ina->fields[F_RST], true);
	if (ret) {
		dev_err(ina->dev, "Unable to reset device\n");
		return ret;
	}

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->dev.parent = ina->dev;
	indio_dev->channels = ina3221_channels;
	indio_dev->num_channels = ARRAY_SIZE(ina3221_channels);
	indio_dev->name = INA3221_DRIVER_NAME;
	indio_dev->info = &ina3221_iio_info;

	ret = devm_iio_device_register(ina->dev, indio_dev);
	if (ret) {
		dev_err(ina->dev, "Unable to register IIO device\n");
		return ret;
	}

	return 0;
}

static const struct i2c_device_id ina3221_ids[] = {
	{ "ina3221", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ina3221_ids);

static struct i2c_driver ina3221_i2c_driver = {
	.driver = {
		.name = INA3221_DRIVER_NAME,
		.of_match_table = of_match_ptr(ina3221_of_match),
	},
	.probe = ina3221_probe,
	.id_table = ina3221_ids,
};
module_i2c_driver(ina3221_i2c_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("Texas Instruments INA3221 Driver");
MODULE_LICENSE("GPL v2");
