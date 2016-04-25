/* Sensirion SHT3x-DIS humidity and temperature sensor driver.
 * The SHT3x comes in many different versions, this driver is for the
 * I2C version only.
 *
 * Copyright (C) 2016 Sensirion AG, Switzerland
 * Author: David Frey <david.frey@sensirion.com>
 * Author: Pascal Sachs <pascal.sachs@sensirion.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/page.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_data/sht3x.h>

/* commands (high precision mode) */
static const unsigned char sht3x_cmd_measure_blocking_hpm[]    = { 0x2c, 0x06 };
static const unsigned char sht3x_cmd_measure_nonblocking_hpm[] = { 0x24, 0x00 };

/* commands (low power mode) */
static const unsigned char sht3x_cmd_measure_blocking_lpm[]    = { 0x2c, 0x10 };
static const unsigned char sht3x_cmd_measure_nonblocking_lpm[] = { 0x24, 0x16 };

/* commands for periodic mode */
static const unsigned char sht3x_cmd_measure_periodic_mode[]   = { 0xe0, 0x00 };
static const unsigned char sht3x_cmd_break[]                   = { 0x30, 0x93 };

/* other commands */
static const unsigned char sht3x_cmd_clear_status_reg[]        = { 0x30, 0x41 };
static const unsigned char sht3x_cmd_soft_reset[]              = { 0x30, 0xa2 };

/* delays for non-blocking i2c commands, both in us */
#define SHT3X_NONBLOCKING_WAIT_TIME_HPM  15000
#define SHT3X_NONBLOCKING_WAIT_TIME_LPM   4000

#define SHT3X_WORD_LEN         2
#define SHT3X_CMD_LENGTH       2
#define SHT3X_CRC8_LEN         1
#define SHT3X_RESPONSE_LENGTH  6
#define SHT3X_CRC8_LEN         1
#define SHT3X_CRC8_POLYNOMIAL  0x31
#define SHT3X_CRC8_INIT        0xFF
#define SHT3X_ID_SHT           0
#define SHT3X_ID_STS           1

DECLARE_CRC8_TABLE(sht3x_crc8_table);

/* periodic measure commands (high precision mode) */
static const char periodic_measure_commands_hpm[][SHT3X_CMD_LENGTH] = {
	/* 0.5 measurements per second */
	{0x20, 0x32},
	/* 1 measurements per second */
	{0x21, 0x30},
	/* 2 measurements per second */
	{0x22, 0x36},
	/* 4 measurements per second */
	{0x23, 0x34},
	/* 10 measurements per second */
	{0x27, 0x37},
};

/* periodic measure commands (low power mode) */
static const char periodic_measure_commands_lpm[][SHT3X_CMD_LENGTH] = {
	/* 0.5 measurements per second */
	{0x20, 0x2f},
	/* 1 measurements per second */
	{0x21, 0x2d},
	/* 2 measurements per second */
	{0x22, 0x2b},
	/* 4 measurements per second */
	{0x23, 0x29},
	/* 10 measurements per second */
	{0x27, 0x2a},
};

struct sht3x_alert_commands {
	const char read_command[SHT3X_CMD_LENGTH];
	const char write_command[SHT3X_CMD_LENGTH];
};

const struct sht3x_alert_commands alert_commands[] = {
	/* temp1_max, humidity1_max */
	{ {0xe1, 0x1f}, {0x61, 0x1d} },
	/* temp_1_max_hyst, humidity1_max_hyst */
	{ {0xe1, 0x14}, {0x61, 0x16} },
	/* temp1_min, humidity1_min */
	{ {0xe1, 0x02}, {0x61, 0x00} },
	/* temp_1_min_hyst, humidity1_min_hyst */
	{ {0xe1, 0x09}, {0x61, 0x0B} },
};


static const u16 mode_to_frequency[] = {
	    0,
	  500,
	 1000,
	 2000,
	 4000,
	10000,
};

struct sht3x_data {
	struct i2c_client *client;
	struct mutex update_lock;

	u8 mode;
	const unsigned char *command;
	u32 wait_time;			/* in us*/

	struct sht3x_platform_data setup;

	int temperature;	/* 1000 * temperature in dgr C */
	u32 humidity;		/* 1000 * relative humidity in %RH */
};


static int find_index(const u16 *list, size_t size, u16 value)
{
	size_t index = 0;

	while (index < size && list[index] != value)
		index++;

	return index == size ? -1 : index;
}

static int sht3x_read_from_command(struct i2c_client *client,
				   struct sht3x_data *data,
				   const char *command,
				   char *buf, int length, u32 wait_time)
{
	int ret;

	mutex_lock(&data->update_lock);

	ret = i2c_master_send(client, command, SHT3X_CMD_LENGTH);

	if (ret != SHT3X_CMD_LENGTH) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}

	if (wait_time)
		usleep_range(wait_time, wait_time + 1000);

	ret = i2c_master_recv(client, buf, length);
	if (ret != length) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(&data->update_lock);
	return ret;

}

static int sht3x_extract_temperature(u16 raw)
{
	/*
	 * From datasheet:
	 * T = -45 + 175 * ST / 2^16
	 * Adapted for integer fixed point (3 digit) arithmetic.
	 */
	return ((21875 * (int)raw) >> 13) - 45000;
}

static u32 sht3x_extract_humidity(u16 raw)
{
	/*
	 * From datasheet:
	 * RH = 100 * SRH / 2^16
	 * Adapted for integer fixed point (3 digit) arithmetic.
	 */
	return (12500 * (int)raw) >> 13;
}

/* sysfs attributes */
static struct sht3x_data *sht3x_update_client(struct device *dev)
{
	struct sht3x_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned char buf[SHT3X_RESPONSE_LENGTH];
	u16 val;
	int ret;

	ret = sht3x_read_from_command(client, data,  data->command,
				      buf, sizeof(buf), data->wait_time);
	if (ret)
		return ERR_PTR(ret);

	val = be16_to_cpup((__be16 *) buf);
	data->temperature = sht3x_extract_temperature(val);
	val = be16_to_cpup((__be16 *) (buf + 3));
	data->humidity = sht3x_extract_humidity(val);

	return data;
}

static ssize_t temp1_input_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sht3x_data *data = sht3x_update_client(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->temperature);
}

static ssize_t humidity1_input_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sht3x_data *data = sht3x_update_client(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->humidity);
}

static int alert_read_raw(struct device *dev,
			  struct device_attribute *attr,
			  char *buffer, int length)
{
	int ret;
	u8 index;
	const struct sht3x_alert_commands *commands;
	struct sht3x_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	index = to_sensor_dev_attr(attr)->index;
	commands = &alert_commands[index];

	ret = sht3x_read_from_command(client, data, commands->read_command,
				      buffer, length, 0);

	return ret;
}

static ssize_t temp1_alert_read(struct device *dev,
				struct device_attribute *attr,
				int *temperature)
{
	int ret;
	u16 raw;
	char buffer[SHT3X_RESPONSE_LENGTH];

	ret = alert_read_raw(dev, attr, buffer, SHT3X_RESPONSE_LENGTH);
	if (ret)
		return ret;

	raw = be16_to_cpup((__be16 *) buffer);

	/*
	 * lower 9 bits are temperature MSB
	 */
	*temperature = sht3x_extract_temperature((raw & 0x01ff) << 7);

	return 0;
}


static ssize_t temp1_alert_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int ret;
	int temperature;

	ret = temp1_alert_read(dev, attr, &temperature);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", temperature);
}


static ssize_t humidity1_alert_read(struct device *dev,
				    struct device_attribute *attr,
				    u32 *humidity)
{
	int ret;
	u16 raw;
	char buffer[SHT3X_RESPONSE_LENGTH];

	ret = alert_read_raw(dev, attr, buffer, SHT3X_RESPONSE_LENGTH);
	if (ret)
		return ret;

	raw = be16_to_cpup((__be16 *) buffer);

	/*
	 * top 7 bits are the humidity MSB,
	 */
	*humidity = sht3x_extract_humidity(raw & 0xfe00);

	return 0;
}

static ssize_t humidity1_alert_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int ret;
	u32 humidity;

	ret = humidity1_alert_read(dev, attr, &humidity);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", humidity);
}

static size_t alert_store(struct device *dev,
			  struct device_attribute *attr,
			  size_t count,
			  int temperature,
			  u32 humidity)
{
	char buffer[SHT3X_CMD_LENGTH + SHT3X_WORD_LEN + SHT3X_CRC8_LEN];
	char *position = buffer;
	int ret;
	u16 raw;
	u8 index;
	struct sht3x_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	const struct sht3x_alert_commands *commands;

	index = to_sensor_dev_attr(attr)->index;
	commands = &alert_commands[index];

	memcpy(position, commands->write_command, SHT3X_CMD_LENGTH);
	position += SHT3X_CMD_LENGTH;
	/*
	 * ST = (T + 45) / 175 * 2^16
	 * SRH = RH / 100 * 2^16
	 * adapted for fixed point arithmetic and packed the same as
	 * in alert_show()
	 */
	raw = ((u32)(temperature + 45000) * 24543) >> (16 + 7);
	raw |= (((u32)humidity * 42950) >> 16) & 0xfe00;

	*(__be16 *)(position) = cpu_to_be16(raw);
	position += SHT3X_WORD_LEN;
	*position = crc8(sht3x_crc8_table,
			 (position - SHT3X_WORD_LEN),
			 SHT3X_WORD_LEN,
			 SHT3X_CRC8_INIT);


	ret = i2c_master_send(client, buffer, sizeof(buffer));
	if (ret != sizeof(buffer))
		return ret < 0 ? ret : -EIO;

	return count;

}

static ssize_t temp1_alert_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t count)
{
	int temperature;
	u32 humidity;
	int ret;

	ret = kstrtoint(buf, 0, &temperature);

	if (ret)
		return -EINVAL;

	temperature = clamp_val(temperature, -45000, 130000);

	/*
	 * use old humidity alert value since we can only update
	 * temperature and humidity alerts together
	 */
	ret = humidity1_alert_read(dev, attr, &humidity);
	if (ret)
		return ret;

	ret = alert_store(dev, attr, count, temperature, humidity);

	return ret;
}

static ssize_t humidity1_alert_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t count)
{
	int temperature;
	u32 humidity;
	int ret;

	ret = kstrtou32(buf, 0, &humidity);

	if (ret)
		return -EINVAL;

	humidity = clamp_val(humidity, 0, 100000);

	/*
	 * use old temperature alert value since we can only update
	 * temperature and humidity alerts together
	 */
	ret = temp1_alert_read(dev, attr, &temperature);
	if (ret)
		return ret;

	ret = alert_store(dev, attr, count, temperature, humidity);

	return ret;
}

static void sht3x_select_command(struct sht3x_data *data)
{
	/*
	 * In blocking mode (clock stretching mode) the I2C bus
	 * is blocked for other traffic, thus the call to i2c_master_recv()
	 * will wait until the data is ready. For non blocking mode, we
	 * have to wait ourselves.
	 */
	if (data->mode > 0) {
		data->command = sht3x_cmd_measure_periodic_mode;
		data->wait_time = 0;
	} else if (data->setup.blocking_io) {
		data->command = data->setup.high_precision ?
				sht3x_cmd_measure_blocking_hpm :
				sht3x_cmd_measure_blocking_lpm;
		data->wait_time = 0;
	} else {
		if (data->setup.high_precision) {
			data->command = sht3x_cmd_measure_nonblocking_hpm;
			data->wait_time = SHT3X_NONBLOCKING_WAIT_TIME_HPM;
		} else {
			data->command = sht3x_cmd_measure_nonblocking_lpm;
			data->wait_time = SHT3X_NONBLOCKING_WAIT_TIME_LPM;
		}
	}
}

static ssize_t frequency_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf) {
	struct sht3x_data *data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", mode_to_frequency[data->mode]);
}

static ssize_t frequency_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count) {
	u16 frequency;
	int mode;
	int ret;
	const char *command;
	struct sht3x_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	ret = kstrtou16(buf, 0, &frequency);

	if (ret)
		return -EINVAL;

	mode = find_index(mode_to_frequency, sizeof(mode_to_frequency),
			  frequency);

	/* invalid frequency */
	if (mode < 0)
		return -EINVAL;

	/* mode did not change */
	if (mode == data->mode)
		return count;

	/* abort periodic measure */
	ret = i2c_master_send(client, sht3x_cmd_break, SHT3X_CMD_LENGTH);
	if (ret != SHT3X_CMD_LENGTH)
		return ret < 0 ? ret : -EIO;
	data->mode = 0;

	if (mode > 0) {
		if (data->setup.high_precision)
			command = periodic_measure_commands_hpm[mode - 1];
		else
			command = periodic_measure_commands_lpm[mode - 1];

		/* select mode */
		ret = i2c_master_send(client, command, SHT3X_CMD_LENGTH);
		if (ret != SHT3X_CMD_LENGTH)
			return ret < 0 ? ret : -EIO;
	}

	/* select mode and command */
	data->mode = mode;
	sht3x_select_command(data);

	return count;
}

static ssize_t soft_reset(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count) {
	int ret;
	struct sht3x_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	/* break if in periodic measure mode */
	if (data->mode > 0) {
		ret = i2c_master_send(client, sht3x_cmd_break,
				      SHT3X_CMD_LENGTH);
		if (ret != SHT3X_CMD_LENGTH)
			return ret < 0 ? ret : -EIO;
		data->mode = 0;
	}

	/* clear status register */
	ret = i2c_master_send(client, sht3x_cmd_clear_status_reg,
			      SHT3X_CMD_LENGTH);
	if (ret != SHT3X_CMD_LENGTH)
		return ret < 0 ? ret : -EIO;

	/* soft reset */
	ret = i2c_master_send(client, sht3x_cmd_soft_reset, SHT3X_CMD_LENGTH);
	if (ret != SHT3X_CMD_LENGTH)
		return ret < 0 ? ret : -EIO;

	return count;
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, temp1_input_show, NULL, 0);
static SENSOR_DEVICE_ATTR(humidity1_input, S_IRUGO, humidity1_input_show,
			  NULL, 0);
static SENSOR_DEVICE_ATTR(temp1_max, S_IRUGO | S_IWUSR, temp1_alert_show,
			  temp1_alert_store, 0);
static SENSOR_DEVICE_ATTR(humidity1_max, S_IRUGO | S_IWUSR,
			  humidity1_alert_show, humidity1_alert_store, 0);
static SENSOR_DEVICE_ATTR(temp1_max_hyst, S_IRUGO | S_IWUSR, temp1_alert_show,
			  temp1_alert_store, 1);
static SENSOR_DEVICE_ATTR(humidity1_max_hyst, S_IRUGO | S_IWUSR,
			  humidity1_alert_show, humidity1_alert_store, 1);
static SENSOR_DEVICE_ATTR(temp1_min, S_IRUGO | S_IWUSR, temp1_alert_show,
			  temp1_alert_store, 2);
static SENSOR_DEVICE_ATTR(humidity1_min, S_IRUGO | S_IWUSR,
			  humidity1_alert_show, humidity1_alert_store, 2);
static SENSOR_DEVICE_ATTR(temp1_min_hyst, S_IRUGO | S_IWUSR,
			  temp1_alert_show, temp1_alert_store, 3);
static SENSOR_DEVICE_ATTR(humidity1_min_hyst, S_IRUGO | S_IWUSR,
			  humidity1_alert_show, humidity1_alert_store, 3);
static SENSOR_DEVICE_ATTR(frequency, S_IRUGO | S_IWUSR, frequency_show,
			  frequency_store, 0);
static SENSOR_DEVICE_ATTR(soft_reset, S_IWUSR, NULL, soft_reset, 0);

static struct attribute *sht3x_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_humidity1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_max.dev_attr.attr,
	&sensor_dev_attr_temp1_max_hyst.dev_attr.attr,
	&sensor_dev_attr_humidity1_max.dev_attr.attr,
	&sensor_dev_attr_humidity1_max_hyst.dev_attr.attr,
	&sensor_dev_attr_temp1_min.dev_attr.attr,
	&sensor_dev_attr_temp1_min_hyst.dev_attr.attr,
	&sensor_dev_attr_humidity1_min.dev_attr.attr,
	&sensor_dev_attr_humidity1_min_hyst.dev_attr.attr,
	&sensor_dev_attr_frequency.dev_attr.attr,
	&sensor_dev_attr_soft_reset.dev_attr.attr,
	NULL
};

static struct attribute *sts3x_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	NULL
};

ATTRIBUTE_GROUPS(sht3x);
ATTRIBUTE_GROUPS(sts3x);

static int sht3x_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	int ret;
	struct sht3x_data *data;
	struct device *hwmon_dev;
	struct i2c_adapter *adap = client->adapter;
	struct device *dev = &client->dev;
	const struct attribute_group **attribute_groups;

	/*
	 * we require full i2c support since the sht3x uses multi-byte read and
	 * writes as well as multi-byte commands which are not supported by
	 * the smbus protocol
	 */
	if (!i2c_check_functionality(adap, I2C_FUNC_I2C))
		return -ENODEV;

	ret = i2c_master_send(client, sht3x_cmd_clear_status_reg,
			      SHT3X_CMD_LENGTH);
	if (ret != SHT3X_CMD_LENGTH)
		return ret < 0 ? ret : -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->setup.blocking_io = false;
	data->setup.high_precision = true;
	data->mode = 0;
	data->client = client;
	crc8_populate_msb(sht3x_crc8_table, SHT3X_CRC8_POLYNOMIAL);

	if (client->dev.platform_data)
		data->setup = *(struct sht3x_platform_data *)dev->platform_data;
	sht3x_select_command(data);
	mutex_init(&data->update_lock);

	if (id->driver_data == SHT3X_ID_STS)
		attribute_groups = sts3x_groups;
	else
		attribute_groups = sht3x_groups;

	hwmon_dev = devm_hwmon_device_register_with_groups(dev,
			client->name,
			data, attribute_groups);

	if (IS_ERR(hwmon_dev))
		dev_dbg(dev, "unable to register hwmon device\n");

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

/* device ID table */
static const struct i2c_device_id sht3x_id[] = {
	{"sht3x", SHT3X_ID_SHT},
	{"sts3x", SHT3X_ID_STS},
	{}
};

MODULE_DEVICE_TABLE(i2c, sht3x_id);

static struct i2c_driver sht3x_i2c_driver = {
	.driver.name = "sht3x",
	.probe       = sht3x_probe,
	.id_table    = sht3x_id,
};

module_i2c_driver(sht3x_i2c_driver);

MODULE_AUTHOR("David Frey <david.frey@sensirion.com>");
MODULE_AUTHOR("Pascal Sachs <pascal.sachs@sensirion.com>");
MODULE_DESCRIPTION("Sensirion SHT3x humidity and temperature sensor driver");
MODULE_LICENSE("GPL");
