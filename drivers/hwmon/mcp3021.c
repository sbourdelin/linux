/*
 * mcp3021.c - driver for Microchip MCP3021 and MCP3221
 *
 * Copyright (C) 2008-2009, 2012 Freescale Semiconductor, Inc.
 * Author: Mingkai Hu <Mingkai.hu@freescale.com>
 * Reworked by Sven Schuchmann <schuchmann@schleissheimer.de>
 * Copyright (C) 2016 Clemens Gruber <clemens.gruber@pqgruber.com>
 *
 * This driver export the value of analog input voltage to sysfs, the
 * voltage unit is mV. Through the sysfs interface, lm-sensors tool
 * can also display the input voltage.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hwmon.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>

/* Vdd / reference voltage in millivolt */
#define MCP3021_VDD_MAX		5500
#define MCP3021_VDD_MIN		2700
#define MCP3021_VDD_DEFAULT	3300

enum chips {
	mcp3021,
	mcp3221
};

struct mcp3021_chip_info {
	u16 sar_shift;
	u16 sar_mask;
	u8 output_res;
};

/*
 * Client data (each client gets its own)
 */
struct mcp3021_data {
	struct device *hwmon_dev;
	const struct mcp3021_chip_info *chip_info;
	u32 vdd; /* device power supply and reference voltage in millivolt */
};

static const struct mcp3021_chip_info mcp3021_chip_info_tbl[] = {
	[mcp3021] = {
		.sar_shift = 2,
		.sar_mask = 0x3ff,
		.output_res = 10,	/* 10-bit resolution */
	},
	[mcp3221] = {
		.sar_shift = 0,
		.sar_mask = 0xfff,
		.output_res = 12,	/* 12-bit resolution */
	},
};

#ifdef CONFIG_OF
static const struct of_device_id of_mcp3021_match[] = {
	{ .compatible = "microchip,mcp3021", .data = (void *)mcp3021 },
	{ .compatible = "microchip,mcp3221", .data = (void *)mcp3221 },
	{ }
};
MODULE_DEVICE_TABLE(of, of_mcp3021_match);
#endif

static int mcp3021_read16(struct i2c_client *client)
{
	struct mcp3021_data *data = i2c_get_clientdata(client);
	int ret;
	u16 reg;
	__be16 buf;

	ret = i2c_master_recv(client, (char *)&buf, 2);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	/* The output code of the MCP3021 is transmitted with MSB first. */
	reg = be16_to_cpu(buf);

	/*
	 * The ten-bit output code is composed of the lower 4-bit of the
	 * first byte and the upper 6-bit of the second byte.
	 */
	reg = (reg >> data->chip_info->sar_shift) & data->chip_info->sar_mask;

	return reg;
}

static inline u16 volts_from_reg(struct mcp3021_data *data, u16 val)
{
	return DIV_ROUND_CLOSEST(data->vdd * val,
				 1 << data->chip_info->output_res);
}

static ssize_t show_in_input(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mcp3021_data *data = i2c_get_clientdata(client);
	int reg, in_input;

	reg = mcp3021_read16(client);
	if (reg < 0)
		return reg;

	in_input = volts_from_reg(data, reg);

	return sprintf(buf, "%d\n", in_input);
}

static DEVICE_ATTR(in0_input, 0444, show_in_input, NULL);

#ifdef CONFIG_OF
static int mcp3021_probe_dt(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct mcp3021_data *data = i2c_get_clientdata(client);
	struct device_node *np = client->dev.of_node;
	const struct of_device_id *match;
	int devid, ret;

	match = of_match_device(of_mcp3021_match, &client->dev);
	if (!match)
		return -ENODEV;

	devid = (int)(uintptr_t)match->data;
	data->chip_info = &mcp3021_chip_info_tbl[devid];

	ret = of_property_read_u32(np, "reference-voltage-microvolt",
				   &data->vdd);
	if (ret) {
		/* fallback */
		data->vdd = MCP3021_VDD_DEFAULT;
		return 0;
	}

	/* Convert microvolt from DT to millivolt used in the formula */
	data->vdd /= 1000;

	if (data->vdd > MCP3021_VDD_MAX || data->vdd < MCP3021_VDD_MIN)
		return -EINVAL;

	return 0;
}
#else
static int mcp3021_probe_dt(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	return 1;
}
#endif

static int mcp3021_probe_pdata(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct mcp3021_data *data = i2c_get_clientdata(client);

	data->chip_info = &mcp3021_chip_info_tbl[id->driver_data];

	if (dev_get_platdata(&client->dev)) {
		data->vdd = *(u32 *)dev_get_platdata(&client->dev);
		if (data->vdd > MCP3021_VDD_MAX || data->vdd < MCP3021_VDD_MIN)
			return -EINVAL;
	} else {
		data->vdd = MCP3021_VDD_DEFAULT;
	}

	return 0;
}

static int mcp3021_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct mcp3021_data *data = NULL;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);

	err = mcp3021_probe_dt(client, id);
	if (err > 0)
		mcp3021_probe_pdata(client, id);
	else if (err < 0)
		return err;

	err = sysfs_create_file(&client->dev.kobj, &dev_attr_in0_input.attr);
	if (err)
		return err;

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	return 0;

exit_remove:
	sysfs_remove_file(&client->dev.kobj, &dev_attr_in0_input.attr);
	return err;
}

static int mcp3021_remove(struct i2c_client *client)
{
	struct mcp3021_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_file(&client->dev.kobj, &dev_attr_in0_input.attr);

	return 0;
}

static const struct i2c_device_id mcp3021_id[] = {
	{ "mcp3021", mcp3021 },
	{ "mcp3221", mcp3221 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp3021_id);

static struct i2c_driver mcp3021_driver = {
	.driver = {
		.name = "mcp3021",
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(of_mcp3021_match),
#endif
	},
	.probe = mcp3021_probe,
	.remove = mcp3021_remove,
	.id_table = mcp3021_id,
};

module_i2c_driver(mcp3021_driver);

MODULE_AUTHOR("Mingkai Hu <Mingkai.hu@freescale.com>");
MODULE_DESCRIPTION("Microchip MCP3021/MCP3221 driver");
MODULE_LICENSE("GPL");
