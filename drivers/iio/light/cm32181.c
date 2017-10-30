/*
 * Copyright (C) 2013 Capella Microsystems Inc.
 * Author: Kevin Tsai <ktsai@capellamicro.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2, as published
 * by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/init.h>
#include <linux/i2c-smbus.h>
#include <linux/acpi.h>
#include <linux/of_device.h>
#include <linux/resource_ext.h>

/* Registers Addresses */
#define CM32181_REG_ADDR_CMD		0x00
#define CM32181_REG_ADDR_ALS		0x04
#define CM32181_REG_ADDR_STATUS		0x06
#define CM32181_REG_ADDR_ID		0x07

/* Number of Configurable Registers */
#define CM32181_CONF_REG_NUM		0x01

/* CMD register */
#define CM32181_CMD_ALS_ENABLE		0x00
#define CM32181_CMD_ALS_DISABLE		0x01
#define CM32181_CMD_ALS_INT_EN		0x02

#define CM32181_CMD_ALS_IT_SHIFT	6
#define CM32181_CMD_ALS_IT_MASK		(0x0F << CM32181_CMD_ALS_IT_SHIFT)
#define CM32181_CMD_ALS_IT_DEFAULT	(0x00 << CM32181_CMD_ALS_IT_SHIFT)

#define CM32181_CMD_ALS_SM_SHIFT	11
#define CM32181_CMD_ALS_SM_MASK		(0x03 << CM32181_CMD_ALS_SM_SHIFT)
#define CM32181_CMD_ALS_SM_DEFAULT	(0x01 << CM32181_CMD_ALS_SM_SHIFT)

#define CM32181_MLUX_PER_BIT		5	/* ALS_SM=01 IT=800ms */
#define CM32181_MLUX_PER_BIT_BASE_IT	800000	/* Based on IT=800ms */
#define	CM32181_CALIBSCALE_DEFAULT	1000
#define CM32181_CALIBSCALE_RESOLUTION	1000
#define CM32181_MLUX_PER_LUX			1000

#define CM32181_ID			0x81
#define CM3218_ID			0x18

#define CM3218_ARA_ADDR			0x0c

static const u8 cm32181_reg[CM32181_CONF_REG_NUM] = {
	CM32181_REG_ADDR_CMD,
};

static const int cm32181_als_it_bits[] = {12, 8, 0, 1, 2, 3};
static const int cm32181_als_it_value[] = {25000, 50000, 100000, 200000, 400000,
	800000};

struct cm32181_chip {
	struct i2c_client *client;
	int chip_id;
	struct i2c_client *ara;
	struct mutex lock;
	u16 conf_regs[CM32181_CONF_REG_NUM];
	int calibscale;
};

/**
 * cm32181_reg_init() - Initialize CM32181 registers
 * @cm32181:	pointer of struct cm32181.
 *
 * Initialize CM32181 ambient light sensor register to default values.
 *
 * Return: 0 for success; otherwise for error code.
 */
static int cm32181_reg_init(struct cm32181_chip *cm32181)
{
	struct i2c_client *client = cm32181->client;
	int i;
	s32 ret;

	ret = i2c_smbus_read_word_data(client, CM32181_REG_ADDR_ID);
	if (ret < 0)
		return ret;

	/* check device ID */
	if ((ret & 0xFF) != cm32181->chip_id)
		return -ENODEV;

	/* Default Values */
	cm32181->conf_regs[CM32181_REG_ADDR_CMD] = CM32181_CMD_ALS_ENABLE |
			CM32181_CMD_ALS_IT_DEFAULT | CM32181_CMD_ALS_SM_DEFAULT;
	cm32181->calibscale = CM32181_CALIBSCALE_DEFAULT;

	/* Initialize registers*/
	for (i = 0; i < CM32181_CONF_REG_NUM; i++) {
		ret = i2c_smbus_write_word_data(client, cm32181_reg[i],
			cm32181->conf_regs[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/**
 *  cm32181_read_als_it() - Get sensor integration time (ms)
 *  @cm32181:	pointer of struct cm32181
 *  @val2:	pointer of int to load the cm32181_als_it value.
 *
 *  Report the current integartion time by millisecond.
 *
 *  Return: IIO_VAL_INT_PLUS_MICRO for success, otherwise -EINVAL.
 */
static int cm32181_read_als_it(struct cm32181_chip *cm32181, int *val2)
{
	u16 als_it;
	int i;

	als_it = cm32181->conf_regs[CM32181_REG_ADDR_CMD];
	als_it &= CM32181_CMD_ALS_IT_MASK;
	als_it >>= CM32181_CMD_ALS_IT_SHIFT;
	for (i = 0; i < ARRAY_SIZE(cm32181_als_it_bits); i++) {
		if (als_it == cm32181_als_it_bits[i]) {
			*val2 = cm32181_als_it_value[i];
			return IIO_VAL_INT_PLUS_MICRO;
		}
	}

	return -EINVAL;
}

/**
 * cm32181_write_als_it() - Write sensor integration time
 * @cm32181:	pointer of struct cm32181.
 * @val:	integration time by millisecond.
 *
 * Convert integration time (ms) to sensor value.
 *
 * Return: i2c_smbus_write_word_data command return value.
 */
static int cm32181_write_als_it(struct cm32181_chip *cm32181, int val)
{
	struct i2c_client *client = cm32181->client;
	u16 als_it;
	int ret, i, n;

	n = ARRAY_SIZE(cm32181_als_it_value);
	for (i = 0; i < n; i++)
		if (val <= cm32181_als_it_value[i])
			break;
	if (i >= n)
		i = n - 1;

	als_it = cm32181_als_it_bits[i];
	als_it <<= CM32181_CMD_ALS_IT_SHIFT;

	mutex_lock(&cm32181->lock);
	cm32181->conf_regs[CM32181_REG_ADDR_CMD] &=
		~CM32181_CMD_ALS_IT_MASK;
	cm32181->conf_regs[CM32181_REG_ADDR_CMD] |=
		als_it;
	ret = i2c_smbus_write_word_data(client, CM32181_REG_ADDR_CMD,
			cm32181->conf_regs[CM32181_REG_ADDR_CMD]);
	mutex_unlock(&cm32181->lock);

	return ret;
}

/**
 * cm32181_get_lux() - report current lux value
 * @cm32181:	pointer of struct cm32181.
 *
 * Convert sensor raw data to lux.  It depends on integration
 * time and calibscale variable.
 *
 * Return: Positive value is lux, otherwise is error code.
 */
static int cm32181_get_lux(struct cm32181_chip *cm32181)
{
	struct i2c_client *client = cm32181->client;
	int ret;
	int als_it;
	unsigned long lux;

	ret = cm32181_read_als_it(cm32181, &als_it);
	if (ret < 0)
		return -EINVAL;

	lux = CM32181_MLUX_PER_BIT;
	lux *= CM32181_MLUX_PER_BIT_BASE_IT;
	lux /= als_it;

	ret = i2c_smbus_read_word_data(client, CM32181_REG_ADDR_ALS);
	if (ret < 0)
		return ret;

	lux *= ret;
	lux *= cm32181->calibscale;
	lux /= CM32181_CALIBSCALE_RESOLUTION;
	lux /= CM32181_MLUX_PER_LUX;

	if (lux > 0xFFFF)
		lux = 0xFFFF;

	return lux;
}

static int cm32181_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct cm32181_chip *cm32181 = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		ret = cm32181_get_lux(cm32181);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_CALIBSCALE:
		*val = cm32181->calibscale;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0;
		ret = cm32181_read_als_it(cm32181, val2);
		return ret;
	}

	return -EINVAL;
}

static int cm32181_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct cm32181_chip *cm32181 = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_CALIBSCALE:
		cm32181->calibscale = val;
		return val;
	case IIO_CHAN_INFO_INT_TIME:
		ret = cm32181_write_als_it(cm32181, val2);
		return ret;
	}

	return -EINVAL;
}

/**
 * cm32181_get_it_available() - Get available ALS IT value
 * @dev:	pointer of struct device.
 * @attr:	pointer of struct device_attribute.
 * @buf:	pointer of return string buffer.
 *
 * Display the available integration time values by millisecond.
 *
 * Return: string length.
 */
static ssize_t cm32181_get_it_available(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int i, n, len;

	n = ARRAY_SIZE(cm32181_als_it_value);
	for (i = 0, len = 0; i < n; i++)
		len += sprintf(buf + len, "0.%06u ", cm32181_als_it_value[i]);
	return len + sprintf(buf + len, "\n");
}

static const struct iio_chan_spec cm32181_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_PROCESSED) |
			BIT(IIO_CHAN_INFO_CALIBSCALE) |
			BIT(IIO_CHAN_INFO_INT_TIME),
	}
};

static IIO_DEVICE_ATTR(in_illuminance_integration_time_available,
			S_IRUGO, cm32181_get_it_available, NULL, 0);

static struct attribute *cm32181_attributes[] = {
	&iio_dev_attr_in_illuminance_integration_time_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group cm32181_attribute_group = {
	.attrs = cm32181_attributes
};

static const struct iio_info cm32181_info = {
	.read_raw		= &cm32181_read_raw,
	.write_raw		= &cm32181_write_raw,
	.attrs			= &cm32181_attribute_group,
};

#if defined(CONFIG_ACPI)
/* Filter acpi resources for a real i2c address on same adapter */
static int cm3218_filter_i2c_address(struct acpi_resource *ares, void *data)
{
	struct i2c_client *client = data;
	struct acpi_resource_i2c_serialbus *sb;
	acpi_status status;
	acpi_handle adapter_handle;

	if (ares->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
		return 1;

	sb = &ares->data.i2c_serial_bus;
	if (sb->type != ACPI_RESOURCE_SERIAL_TYPE_I2C)
		return 1;

	status = acpi_get_handle(ACPI_HANDLE(&client->dev),
				 sb->resource_source.string_ptr,
				 &adapter_handle);
	if (ACPI_FAILURE(status))
		return status;

	if (adapter_handle != ACPI_HANDLE(&client->adapter->dev))
		return 1;

	if (sb->slave_address == CM3218_ARA_ADDR)
		return 1;

	return AE_OK;
}

/* Get the real i2c address from acpi resources */
static int cm3218_acpi_get_address(struct i2c_client *client)
{
	int ret;
	struct acpi_device *adev;
	LIST_HEAD(res_list);
	struct resource_entry *res_entry;
	struct acpi_resource *ares;
	struct acpi_resource_i2c_serialbus *sb;

	adev = ACPI_COMPANION(&client->dev);
	if (!adev)
		return -ENODEV;

	ret = acpi_dev_get_resources(adev,
				     &res_list,
				     cm3218_filter_i2c_address,
				     client);
	if (ret < 0)
		return ret;

	/* get first resource */
	res_entry = list_entry(&res_list, struct resource_entry, node);
	ares = (struct acpi_resource *)res_entry->res;
	sb = &ares->data.i2c_serial_bus;

	/* set i2c address */
	client->addr = sb->slave_address;
	client->flags &= ~I2C_CLIENT_TEN;
	if (sb->access_mode == ACPI_I2C_10BIT_MODE)
		client->flags |= I2C_CLIENT_TEN;

	acpi_dev_free_resource_list(&res_list);

	return 0;
}
#else
static inline int cm3218_acpi_get_address(struct i2c_client *client)
{
	return -ENODEV;
}
#endif

static int cm32181_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cm32181_chip *cm32181;
	struct iio_dev *indio_dev;
	int ret;
	struct i2c_smbus_alert_setup ara_setup;
	const struct of_device_id *of_id;
	const struct acpi_device_id *acpi_id;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*cm32181));
	if (!indio_dev) {
		dev_err(&client->dev, "devm_iio_device_alloc failed\n");
		return -ENOMEM;
	}

	cm32181 = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	cm32181->client = client;

	mutex_init(&cm32181->lock);
	indio_dev->dev.parent = &client->dev;
	indio_dev->channels = cm32181_channels;
	indio_dev->num_channels = ARRAY_SIZE(cm32181_channels);
	indio_dev->info = &cm32181_info;
	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;

	/* Lookup for chip ID from platform, acpi or of device table */
	if (id) {
		cm32181->chip_id = id->driver_data;
	} else if (ACPI_COMPANION(&client->dev)) {
		acpi_id = acpi_match_device(client->dev.driver->acpi_match_table,
					    &client->dev);
		if (!acpi_id)
			return -ENODEV;

		cm32181->chip_id = (kernel_ulong_t)acpi_id->driver_data;
	} else if (client->dev.of_node) {
		of_id = of_match_device(clients->dev.driver->of_match_table,
					&client->dev);
		if (!of_id)
			return -ENODEV;

		cm32181->chip_id = (kernel_ulong_t)of_id->data;
	} else {
		return -ENODEV;
	}

	if (cm32181->chip_id == CM3218_ID) {
		if (client->addr == CM3218_ARA_ADDR) {
			/*
			 * In case first address is the ARA device
			 * lookup for a second one in acpi resources if
			 * this client is enumerated on acpi
			 */
			ret = cm3218_acpi_get_address(client);
			if (ret < 0)
				return -ENODEV;
		}

#ifdef CONFIG_I2C_SMBUS
		if (client->irq > 0) {
			/* setup smb alert device */
			ara_setup.irq = client->irq;
			ara_setup.alert_edge_triggered = 0;
			cm32181->ara = i2c_setup_smbus_alert(client->adapter,
							     &ara_setup);
			if (!cm32181->ara)
				return -ENODEV;
		} else {
			return -ENODEV;
		}
#else
		return -ENODEV;
#endif
	}

	ret = cm32181_reg_init(cm32181);
	if (ret) {
		dev_err(&client->dev,
			"%s: register init failed\n",
			__func__);

		if (cm32181->ara)
			i2c_unregister_device(cm32181->ara);

		return ret;
	}

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret) {
		dev_err(&client->dev,
			"%s: regist device failed\n",
			__func__);

		if (cm32181->ara)
			i2c_unregister_device(cm32181->ara);

		return ret;
	}

	return 0;
}

static int cm32181_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct cm32181_chip *cm32181 = iio_priv(indio_dev);

	if (cm32181->ara)
		i2c_unregister_device(cm32181->ara);

	return 0;
};

static const struct i2c_device_id cm32181_id[] = {
	{ "cm32181", CM32181_ID },
	{ "cm3218", CM3218_ID },
	{ }
};

MODULE_DEVICE_TABLE(i2c, cm32181_id);

static const struct of_device_id cm32181_of_match[] = {
	{ .compatible = "capella,cm32181", (void *)CM32181_ID },
	{ .compatible = "capella,cm3218",  (void *)CM3218_ID },
	{ }
};
MODULE_DEVICE_TABLE(of, cm32181_of_match);

static const struct acpi_device_id cm32181_acpi_match[] = {
	{ "CPLM3218", CM3218_ID },
	{ }
};

MODULE_DEVICE_TABLE(acpi, cm32181_acpi_match);

static struct i2c_driver cm32181_driver = {
	.driver = {
		.name	= "cm32181",
		.of_match_table = of_match_ptr(cm32181_of_match),
		.acpi_match_table = ACPI_PTR(cm32181_acpi_match),
	},
	.id_table       = cm32181_id,
	.probe		= cm32181_probe,
	.remove		= cm32181_remove,
};

module_i2c_driver(cm32181_driver);

MODULE_AUTHOR("Kevin Tsai <ktsai@capellamicro.com>");
MODULE_DESCRIPTION("CM32181 ambient light sensor driver");
MODULE_LICENSE("GPL");
