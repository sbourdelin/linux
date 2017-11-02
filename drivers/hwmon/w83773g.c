/*
 * Copyright (C) 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/*
 * Driver for the Nuvoton W83773G SMBus temperature sensor IC.
 * Supported models: W83773G
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/sysfs.h>

/* Addresses to scan */
static const unsigned short normal_i2c[] = { 0x4c, 0x4d, I2C_CLIENT_END };

enum chips { w83773g };

/* The W83773 registers */
#define W83773_CONVERSION_RATE_REG_READ 0x04
#define W83773_CONVERSION_RATE_REG_WRITE 0x0A
#define W83773_MANUFACTURER_ID_REG 0xFE
#define W83773_LOCAL_TEMP 0x00

static const u8 W83773_STATUS[2] = { 0x02, 0x17 };

static const u8 W83773_TEMP_LSB[2] = { 0x10, 0x25 };
static const u8 W83773_TEMP_MSB[2] = { 0x01, 0x24 };

static const u8 W83773_OFFSET_LSB[2] = { 0x12, 0x16 };
static const u8 W83773_OFFSET_MSB[2] = { 0x11, 0x15 };

/* Manufacturer / Device ID's */
#define W83773_MANUFACTURER_ID			0x5c


/* this is the number of sensors in the device */
static const struct i2c_device_id w83773_id[] = {
	{ "w83773g", 3 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, w83773_id);

static const struct of_device_id w83773_of_match[] = {
	{
		.compatible = "nuvoton,w83773g",
		.data = (void *)3
	},

	{ },
};
MODULE_DEVICE_TABLE(of, w83773_of_match);

/*
 * W83773G has 3 temp sensors:
 *   Channel 0 is the local sensor
 *   Channel 1-2 are two remote sensors
 */
struct w83773_data {
	struct i2c_client *client;
	struct mutex update_lock;
	u32 temp_config[4];
	struct hwmon_channel_info temp_info;
	const struct hwmon_channel_info *info[2];
	struct hwmon_chip_info chip;
	bool valid;
	unsigned long last_updated;
	int channels;
	s8 temp_local;
	s8 status[2];
	s8 temp_hb[2];
	s8 temp_lb[2];
	s8 offset_hb[2];
	s8 offset_lb[2];
};

static long temp_of_local(s8 reg)
{
	return reg * 1000;
}

static long temp_of_remote(s8 hb, s8 lb, s8 offset_hb, s8 offset_lb)
{
	return (hb + offset_hb) * 1000 + ((u8)(lb + offset_lb) >> 5) * 125;
}


static struct w83773_data *w83773_update_device(struct device *dev)
{
	struct w83773_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int i;

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + 2 * HZ) || !data->valid) {
		data->temp_local = i2c_smbus_read_byte_data(client, W83773_LOCAL_TEMP);
		for (i = 0; i < data->channels - 1; i++) {
			data->status[i] = i2c_smbus_read_byte_data(client, W83773_STATUS[i]);
			data->temp_hb[i] = i2c_smbus_read_byte_data(client, W83773_TEMP_MSB[i]);
			data->temp_lb[i] = i2c_smbus_read_byte_data(client, W83773_TEMP_LSB[i]);
			data->offset_hb[i] = i2c_smbus_read_byte_data(client, W83773_OFFSET_MSB[i]);
			data->offset_lb[i] = i2c_smbus_read_byte_data(client, W83773_OFFSET_LSB[i]);
		}
		data->last_updated = jiffies;
		data->valid = true;
	}

	mutex_unlock(&data->update_lock);

	return data;
}

static int w83773_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct w83773_data *w83773 = w83773_update_device(dev);

	switch (attr) {
	case hwmon_temp_input:
		if (channel == 0) {
			/* channel 0 is the local temp */
			*val = temp_of_local(w83773->temp_local);
		}
		else {
			/* channel 1-2 are the remote temps */
			channel--;
			*val = temp_of_remote(
				w83773->temp_hb[channel],
				w83773->temp_lb[channel],
				w83773->offset_hb[channel],
				w83773->offset_lb[channel]);
		}
		return 0;
	case hwmon_temp_fault:
		if (channel == 0)
			*val = 0;
		else
			/* Check the status register bit 2 for faults */
			*val = (w83773->status[channel - 1] & 0x04) >> 2;
		return 0;
	default:
		return -EOPNOTSUPP;
	}

}

static umode_t w83773_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	switch (attr) {
	case hwmon_temp_fault:
		if (channel == 0)
			return 0;
		return S_IRUGO;
	case hwmon_temp_input:
		return S_IRUGO;
	default:
		return 0;
	}
}

static int w83773_init_client(struct i2c_client *client)
{
	/* Set the conversion rate to 2 Hz */
	i2c_smbus_write_byte_data(client, W83773_CONVERSION_RATE_REG_WRITE, 0x05);

	return 0;
}

static int w83773_detect(struct i2c_client *client,
			 struct i2c_board_info *info)
{
	enum chips kind;
	struct i2c_adapter *adapter = client->adapter;
	const char * const names[] = { "W83773G" };
	u8 reg;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	reg = i2c_smbus_read_byte_data(client, W83773_MANUFACTURER_ID_REG);
	if (reg != W83773_MANUFACTURER_ID)
		return -ENODEV;

	reg = i2c_smbus_read_byte_data(client, W83773_CONVERSION_RATE_REG_READ);
	if (reg & 0xf8)
		return -ENODEV;

	kind = w83773g;

	strlcpy(info->type, w83773_id[kind].name, I2C_NAME_SIZE);
	dev_info(&adapter->dev, "Detected Nuvoton %s chip at 0x%02x\n",
		 names[kind], client->addr);

	return 0;
}

static const struct hwmon_ops w83773_ops = {
	.is_visible = w83773_is_visible,
	.read = w83773_read,
};

static int w83773_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct w83773_data *data;
	int i, err;

	data = devm_kzalloc(dev, sizeof(struct w83773_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->update_lock);
	if (client->dev.of_node)
		data->channels = (int)of_device_get_match_data(&client->dev);
	else
		data->channels = id->driver_data;
	data->client = client;

	err = w83773_init_client(client);
	if (err)
		return err;

	for (i = 0; i < data->channels; i++)
		data->temp_config[i] = HWMON_T_INPUT | HWMON_T_FAULT;

	data->chip.ops = &w83773_ops;
	data->chip.info = data->info;

	data->info[0] = &data->temp_info;

	data->temp_info.type = hwmon_temp;
	data->temp_info.config = data->temp_config;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data,
							 &data->chip,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct i2c_driver w83773_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name	= "w83773g",
		.of_match_table = of_match_ptr(w83773_of_match),
	},
	.probe = w83773_probe,
	.id_table = w83773_id,
	.detect = w83773_detect,
	.address_list = normal_i2c,
};

module_i2c_driver(w83773_driver);

MODULE_AUTHOR("Nickolaus Gruendler <ngruend@us.ibm.com>");
MODULE_DESCRIPTION("W83773G temperature sensor driver");
MODULE_LICENSE("GPL");
