/*
 * VESA DDC/CI MCCS brightness driver
 *
 * Copyright (C) 2017 Miłosz Rachwał <me@milek7.pl>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/backlight.h>
#include <linux/delay.h>

static int maxbr;
module_param(maxbr, int, 0644);
MODULE_PARM_DESC(maxbr, "Override maximum brightness value specified by monitor");

static int ddcci_update_status(struct backlight_device *bl)
{
	struct i2c_client *client = bl_get_data(bl);

	char buf[] = { 0x51, 0x84, 0x03, 0x10, 0x00, 0x00, 0x00 };
	buf[4] = bl->props.brightness >> 8;
	buf[5] = bl->props.brightness;
	buf[6] = (client->addr << 1) ^ 0xC6 ^ buf[4] ^ buf[5];

	i2c_master_send(client, buf, 7);
	msleep(50);

	return 0;
}

static int ddcci_read(struct i2c_client *client, struct backlight_properties *props, int init)
{
	int i;
	char xor;

	char buf[11] = { 0x51, 0x82, 0x01, 0x10, 0x00 };
	buf[4] = (client->addr << 1) ^ 0xC2;

	i2c_master_send(client, buf, 5);
	msleep(40);
	i2c_master_recv(client, buf, 11);

	if (buf[3] != 0)
		goto fail;

	xor = 0x50;
	for (i = 0; i < 11; i++)
		xor ^= buf[i];

	if (xor)
		goto fail;

	if (init) {
		if (maxbr)
			props->max_brightness = maxbr;
		else
			props->max_brightness = (buf[6] << 8) | buf[7];
	}
	props->brightness = (buf[8] << 8) | buf[9];

	return 0;

fail:
	dev_err(&client->dev, "failed to read brightness");
	return -1;
}

static int ddcci_get_brightness(struct backlight_device *bl)
{
	struct i2c_client *client = bl_get_data(bl);

	ddcci_read(client, &bl->props, 0);
	return bl->props.brightness;
}

static const struct backlight_ops ddcci_ops = {
	.update_status = ddcci_update_status,
	.get_brightness = ddcci_get_brightness
};

static int ddcci_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct backlight_properties props;
	int retry;
	char name[20];

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_FIRMWARE;
	snprintf(name, 20, "ddcci_%s", dev_name(&client->dev));

	for (retry = 0; retry < 3; retry++) {
		if (ddcci_read(client, &props, 1) >= 0) {
			devm_backlight_device_register(&client->dev, name, &client->dev, client, &ddcci_ops, &props);
			return 0;
		}
	}

	return -1;
}

static int ddcci_remove(struct i2c_client *client)
{
	return 0;
}

static struct i2c_device_id ddcci_idtable[] = {
	{ "ddcci_bl", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ddcci_idtable);

static struct i2c_driver ddcci_driver = {
	.driver = { .name = "ddcci_bl" },
	.id_table = ddcci_idtable,
	.probe = ddcci_probe,
	.remove = ddcci_remove
};
module_i2c_driver(ddcci_driver);

MODULE_AUTHOR("Miłosz Rachwał <me@milek7.pl>");
MODULE_DESCRIPTION("VESA DDC/CI MCCS brightness driver");
MODULE_LICENSE("GPL");
