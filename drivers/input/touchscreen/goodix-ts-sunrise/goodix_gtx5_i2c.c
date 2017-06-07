/*
 * Goodix GTx5 I2C Dirver
 * Hardware interface layer of touchdriver architecture.
 *
 * Copyright (C) 2015 - 2016 Goodix, Inc.
 * Authors:  Wang Yafei <wangyafei@goodix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include "goodix_ts_core.h"

#define TS_DT_COMPATIBLE	"goodix,gtx5"
#define TS_DRIVER_NAME		"goodix_i2c"
#define I2C_MAX_TRANSFER_SIZE	256
#define TS_ADDR_LENGTH		2

#define TS_REG_COORDS_BASE	0x824E
#define TS_REG_CMD		0x8040
#define TS_REG_REQUEST		0x8044
#define TS_REG_VERSION		0x8240
#define TS_REG_CFG_BASE		0x8050

#define CFG_XMAX_OFFSET (0x8052 - 0x8050)
#define CFG_YMAX_OFFSET	(0x8054 - 0x8050)

#define REQUEST_HANDLED	0x00
#define REQUEST_CONFIG	0x01
#define REQUEST_BAKREF	0x02
#define REQUEST_RESET	0x03
#define REQUEST_MAINCLK	0x04
#define REQUEST_IDLE	0x05

#define TS_MAX_SENSORID	5
#define TS_CFG_MAX_LEN	495
#if TS_CFG_MAX_LEN > GOODIX_CFG_MAX_SIZE
#error GOODIX_CFG_MAX_SIZE too small, please fix.
#endif

#ifdef CONFIG_OF
/*
 * goodix_parse_dt_resolution - parse resolution from dt
 * @node: devicetree node
 * @board_data: pointer to board data structure
 * return: 0 - no error, <0 error
 */
static int goodix_parse_dt_resolution(struct device_node *node,
		struct goodix_ts_board_data *board_data)
{
	int r, err;

	r = of_property_read_u32(node, "goodix,panel-max-id",
				&board_data->panel_max_id);
	if (r) {
		err = -ENOENT;
	} else {
		if (board_data->panel_max_id > GOODIX_MAX_TOUCH)
			board_data->panel_max_id = GOODIX_MAX_TOUCH;
	}

	r = of_property_read_u32(node, "goodix,panel-max-x",
				 &board_data->panel_max_x);
	if (r)
		err = -ENOENT;

	r = of_property_read_u32(node, "goodix,panel-max-y",
				&board_data->panel_max_y);
	if (r)
		err = -ENOENT;

	r = of_property_read_u32(node, "goodix,panel-max-w",
				&board_data->panel_max_w);
	if (r)
		err = -ENOENT;

	r = of_property_read_u32(node, "goodix,panel-max-p",
				&board_data->panel_max_p);
	if (r)
		err = -ENOENT;

	board_data->swap_axis = of_property_read_bool(node,
			"goodix,swap-axis");

	return 0;
}

/**
 * goodix_parse_dt - parse board data from dt
 * @dev: pointer to device
 * @board_data: pointer to board data structure
 * return: 0 - no error, <0 error
 */
static int goodix_parse_dt(struct device_node *node,
	struct goodix_ts_board_data *board_data)
{
	struct property *prop;
	int r;

	if (!board_data) {
		ts_err("Invalid board data");
		return -EINVAL;
	}

	r = of_property_read_u32(node, "goodix,irq-flags",
			&board_data->irq_flags);
	if (r) {
		ts_err("Invalid irq-flags");
		return -EINVAL;
	}

	board_data->avdd_name = "vtouch";
	r = of_property_read_u32(node, "goodix,power-on-delay-us",
				&board_data->power_on_delay_us);
	if (!r) {
		/* 1000ms is too large, maybe you have pass a wrong value */
		if (board_data->power_on_delay_us > 1000 * 1000) {
			ts_err("Power on delay time exceed 1s, please check");
			board_data->power_on_delay_us = 0;
		}
	}

	r = of_property_read_u32(node, "goodix,power-off-delay-us",
				&board_data->power_off_delay_us);
	if (!r) {
		/* 1000ms is too large, maybe you have pass a wrong value */
		if (board_data->power_off_delay_us > 1000 * 1000) {
			ts_err("Power off delay time exceed 1s, please check");
			board_data->power_off_delay_us = 0;
		}
	}

	/* get xyz resolutions */
	r = goodix_parse_dt_resolution(node, board_data);
	if (r < 0) {
		ts_err("Failed to parse resolutions:%d", r);
		return r;
	}

	/* key map */
	prop = of_find_property(node, "goodix,panel-key-map", NULL);
	if (prop && prop->length) {
		if (prop->length / sizeof(u32) > GOODIX_MAX_KEY) {
			ts_err("Size of panel-key-map is invalid");
			return r;
		}

		board_data->panel_max_key = prop->length / sizeof(u32);
		r = of_property_read_u32_array(node,
				"goodix,panel-key-map",
				&board_data->panel_key_map[0],
				board_data->panel_max_key);
		if (r)
			return r;
	}

	ts_debug("[DT]id:%d, x:%d, y:%d, w:%d, p:%d",
				board_data->panel_max_id,
				board_data->panel_max_x,
				board_data->panel_max_y,
				board_data->panel_max_w,
				board_data->panel_max_p);
	return 0;
}

/**
 * goodix_parse_dt_cfg - pares config data from devicetree node
 * @dev: pointer to device
 * @cfg_type: config type such as normal_config, highsense_cfg ...
 * @config: pointer to config data structure
 * @sensor_id: sensor id
 * return: 0 - no error, <0 error
 */
static int goodix_parse_dt_cfg(struct goodix_ts_device *dev,
		char *cfg_type, struct goodix_ts_config *config,
		unsigned int sensor_id)
{
	struct device_node *node = dev->dev->of_node;
	struct goodix_ts_board_data *ts_bdata = dev->board_data;
	struct property *prop = NULL;
	char of_node_name[24];
	unsigned int len = 0;
	u16 checksum;

	BUG_ON(config == NULL);
	if (sensor_id > TS_MAX_SENSORID) {
		ts_err("Invalid sensor id");
		return -EINVAL;
	}

	if (config->initialized) {
		ts_info("Config already initialized");
		return 0;
	}

	/*
	 * config data are located in child node called
	 * 'sensorx', x is the sensor ID got from touch
	 * device.
	 */
	snprintf(of_node_name, sizeof(of_node_name),
			"sensor%u", sensor_id);
	node = of_get_child_by_name(node, of_node_name);
	if (!node) {
		ts_err("Child property[%s] not found",
				of_node_name);
		return -EINVAL;
	}

	prop = of_find_property(node, cfg_type, &len);
	if (!prop || !prop->value || len == 0
			|| len > TS_CFG_MAX_LEN || len % 2 != 1) {
		ts_err("Invalid cfg type%s, size:%u", cfg_type, len);
		return -EINVAL;
	}

	config->length = len;

	mutex_init(&config->lock);
	mutex_lock(&config->lock);

	memcpy(config->data, prop->value, len);

	/* modify max-x max-y resolution, little-endian */
	config->data[CFG_XMAX_OFFSET] = (u8)ts_bdata->panel_max_x;
	config->data[CFG_XMAX_OFFSET + 1] = (u8)(ts_bdata->panel_max_x >> 8);
	config->data[CFG_YMAX_OFFSET] = (u8)ts_bdata->panel_max_y;
	config->data[CFG_YMAX_OFFSET + 1] = (u8)(ts_bdata->panel_max_y >> 8);

	/*
	 * checksum: u16 little-endian format
	 * the last byte of config is the config update flag
	 */
	checksum = checksum_le16(config->data, len - 3);
	checksum = 0 - checksum;
	config->data[len - 3] = (u8)checksum;
	config->data[len - 2] = (u8)(checksum >> 8 & 0xff);
	config->data[len - 1] = 0x01;

	strlcpy(config->name, cfg_type, sizeof(config->name));
	config->reg_base = TS_REG_CFG_BASE;
	config->delay = 0;
	config->initialized = true;
	mutex_unlock(&config->lock);

	ts_info("Config name:%s,ver:%02xh,size:%d,checksum:%04xh",
			config->name, config->data[0],
			config->length, checksum);
	return 0;
}

/**
 * goodix_parse_customize_params - parse sensor independent params
 * @dev: pointer to device data
 * @board_data: board data
 * @sensor_id: sensor ID
 * return: 0 - read ok, < 0 - i2c transter error
 */
static int goodix_parse_customize_params(struct goodix_ts_device *dev,
				struct goodix_ts_board_data *board_data,
				unsigned int sensor_id)
{
	struct device_node *node = dev->dev->of_node;
	char of_node_name[24];
	int r;

	if (sensor_id > TS_MAX_SENSORID || node == NULL) {
		ts_err("Invalid sensor id");
		return -EINVAL;
	}

	/* parse sensor independent parameters */
	snprintf(of_node_name, sizeof(of_node_name),
			"sensor%u", sensor_id);
	node = of_find_node_by_name(dev->dev->of_node, of_node_name);
	if (!node) {
		ts_err("Child property[%s] not found", of_node_name);
		return -EINVAL;
	}

	/* sensor independent resolutions */
	r = goodix_parse_dt_resolution(node, board_data);
	return r;
}
#endif

/**
 * goodix_i2c_read - read device register through i2c bus
 * @dev: pointer to device data
 * @addr: register address
 * @data: read buffer
 * @len: bytes to read
 * return: 0 - read ok, < 0 - i2c transter error
 */
static int goodix_i2c_read(struct goodix_ts_device *dev, unsigned int reg,
	unsigned char *data, unsigned int len)
{
	struct i2c_client *client = to_i2c_client(dev->dev);
	unsigned int transfer_length = 0;
	unsigned int pos = 0, address = reg;
	unsigned char get_buf[64], addr_buf[2];
	int retry, r = 0;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = !I2C_M_RD,
			.buf = &addr_buf[0],
			.len = TS_ADDR_LENGTH,
		}, {
			.addr = client->addr,
			.flags = I2C_M_RD,
		}
	};

	if (likely(len < sizeof(get_buf))) {
		/* code optimize, use stack memory */
		msgs[1].buf = &get_buf[0];
	} else {
		msgs[1].buf = kzalloc(len > I2C_MAX_TRANSFER_SIZE
				? I2C_MAX_TRANSFER_SIZE : len, GFP_KERNEL);
		if (!msgs[1].buf)
			return -ENOMEM;
	}

	while (pos != len) {
		if (unlikely(len - pos > I2C_MAX_TRANSFER_SIZE))
			transfer_length = I2C_MAX_TRANSFER_SIZE;
		else
			transfer_length = len - pos;

		msgs[0].buf[0] = (address >> 8) & 0xFF;
		msgs[0].buf[1] = address & 0xFF;
		msgs[1].len = transfer_length;

		for (retry = 0; retry < GOODIX_BUS_RETRY_TIMES; retry++) {
			if (likely(i2c_transfer(client->adapter, msgs, 2) == 2)) {
				memcpy(&data[pos], msgs[1].buf, transfer_length);
				pos += transfer_length;
				address += transfer_length;
				break;
			}
			ts_info("I2c read retry[%d]:0x%x", retry + 1, reg);
			msleep(20);
		}
		if (unlikely(retry == GOODIX_BUS_RETRY_TIMES)) {
			ts_err("I2c read failed,dev:%02x,reg:%04x,size:%u",
					client->addr, reg, len);
			r = -EBUS;
			goto read_exit;
		}
	}

read_exit:
	if (unlikely(len >= sizeof(get_buf)))
		kfree(msgs[1].buf);
	return r;
}

/**
 * goodix_i2c_write - write device register through i2c bus
 * @dev: pointer to device data
 * @addr: register address
 * @data: write buffer
 * @len: bytes to write
 * return: 0 - write ok; < 0 - i2c transter error.
 */
static int goodix_i2c_write(struct goodix_ts_device *dev, unsigned int reg,
		unsigned char *data, unsigned int len)
{
	struct i2c_client *client = to_i2c_client(dev->dev);
	unsigned int pos = 0, transfer_length = 0;
	unsigned int address = reg;
	unsigned char put_buf[64];
	int retry, r = 0;
	struct i2c_msg msg = {
			.addr = client->addr,
			.flags = !I2C_M_RD,
	};

	if (likely(len + TS_ADDR_LENGTH < sizeof(put_buf))) {
		/* code optimize,use stack memory*/
		msg.buf = &put_buf[0];
	} else {
		msg.buf = kmalloc(len + TS_ADDR_LENGTH > I2C_MAX_TRANSFER_SIZE
			? I2C_MAX_TRANSFER_SIZE : len + TS_ADDR_LENGTH, GFP_KERNEL);
		if (!msg.buf)
			return -ENOMEM;
	}

	while (pos != len) {
		if (unlikely(len - pos > I2C_MAX_TRANSFER_SIZE - TS_ADDR_LENGTH))
			transfer_length = I2C_MAX_TRANSFER_SIZE - TS_ADDR_LENGTH;
		else
			transfer_length = len - pos;

		msg.buf[0] = (unsigned char)((address >> 8) & 0xFF);
		msg.buf[1] = (unsigned char)(address & 0xFF);
		msg.len = transfer_length + 2;
		memcpy(&msg.buf[2], &data[pos], transfer_length);

		for (retry = 0; retry < GOODIX_BUS_RETRY_TIMES; retry++) {
			if (likely(i2c_transfer(client->adapter, &msg, 1) == 1)) {
				pos += transfer_length;
				address += transfer_length;
				break;
			}
			ts_info("I2c write retry[%d]", retry + 1);
			msleep(20);
		}
		if (unlikely(retry == GOODIX_BUS_RETRY_TIMES)) {
			ts_err("I2c write failed,dev:%02x,reg:%04x,size:%u",
					client->addr, reg, len);
			r = -EBUS;
			goto write_exit;
		}
	}

write_exit:
	if (likely(len + TS_ADDR_LENGTH >= sizeof(put_buf)))
		kfree(msg.buf);
	return r;
}

static int goodix_read_version(struct goodix_ts_device *dev,
		struct goodix_ts_version *version)
{
	u8 buffer[12];
	int r;

	r = goodix_i2c_read(dev, TS_REG_VERSION,
			buffer, sizeof(buffer));
	if (r < 0) {
		ts_err("Read chip version failed");
		if (version)
			version->valid = false;
		return r;
	}

	/* if checksum is right and first 4 bytes are not invalid value */
	if (checksum_u8(buffer, sizeof(buffer)) == 0 &&
			isalnum(buffer[0]) && isalnum(buffer[1]) &&
			isalnum(buffer[2]) && isalnum(buffer[3])) {
		if (version) {
			memcpy(&version->pid[0], buffer, 4);
			version->pid[4] = '\0';
			version->cid = buffer[4];
			/* vid = main version + minor version */
			version->vid = (buffer[5] << 8) + buffer[6];
			version->sensor_id = buffer[10] & 0x0F;
			version->valid = true;

			if (version->cid)
				ts_info("PID:%s,CID: %c,VID:%04x,SensorID:%u",
					version->pid, version->cid + 'A' - 1,
					version->vid, version->sensor_id);
			else
				ts_info("PID:%s,VID:%04x,SensorID:%u",
					version->pid, version->vid,
					version->sensor_id);
		}
	} else {
		ts_err("Checksum error:%*ph", (int)sizeof(buffer), buffer);
		/* mark this version is invalid */
		if (version)
			version->valid = false;
		r = -EINVAL;
	}

	return r;
}

/**
 * goodix_send_config - send config data to device.
 * @dev: pointer to device
 * @config: pointer to config data struct to be send
 * @return: 0 - succeed, < 0 - failed
 */
static int goodix_send_config(struct goodix_ts_device *dev,
		struct goodix_ts_config *config)
{
	int r = 0;

	if (!config || !config->data) {
		ts_err("Null config data");
		return -EINVAL;
	}

	ts_info("Send %s,ver:%02xh,size:%d",
		config->name, config->data[0],
		config->length);

	mutex_lock(&config->lock);
	r = goodix_i2c_write(dev, config->reg_base,
			config->data, config->length);
	if (r)
		goto exit;

	/* make sure the firmware accept the config data*/
	if (config->delay)
		msleep(config->delay);
exit:
	mutex_unlock(&config->lock);
	return r;
}

static inline int goodix_cmds_init(struct goodix_ts_device *ts_dev)
{
	/* low power mode command */
	ts_dev->sleep_cmd.cmd_reg = TS_REG_CMD;
	ts_dev->sleep_cmd.length = 3;
	ts_dev->sleep_cmd.cmds[0] = 0x05;
	ts_dev->sleep_cmd.cmds[1] = 0x0;
	ts_dev->sleep_cmd.cmds[2] = 0 - 0x05;
	ts_dev->sleep_cmd.initialized = true;

	return 0;
}

/**
 * goodix_hw_init - hardware initialize
 *   Called by touch core module when bootup
 * @ts_dev: pointer to touch device
 * return: 0 - no error, <0 error
 */
static int goodix_hw_init(struct goodix_ts_device *ts_dev)
{
	int r;

	BUG_ON(!ts_dev);
	goodix_cmds_init(ts_dev);

	/* goodix_hw_init may be called many times */
	if (!ts_dev->normal_cfg) {
		ts_dev->normal_cfg = devm_kzalloc(ts_dev->dev,
				sizeof(*ts_dev->normal_cfg), GFP_KERNEL);
		if (!ts_dev->normal_cfg) {
			ts_err("Failed to alloc memory for normal cfg");
			return -ENOMEM;
		}
	}

	/* read chip version: PID/VID/sensor ID,etc.*/
	r = goodix_read_version(ts_dev, &ts_dev->chip_version);
	if (r < 0)
		return r;

#ifdef CONFIG_OF
	/* devicetree property like resolution(panel_max_xxx)
	 * may be different between sensors, here we try to parse
	 * parameters form sensor child node
	 */
	r = goodix_parse_customize_params(ts_dev,
			ts_dev->board_data,
			ts_dev->chip_version.sensor_id);
	if (r < 0)
		ts_info("Cann't find customized parameters");

	/* lonzo debug */
	ts_dev->chip_version.sensor_id = 0;

	/* parse normal-cfg from devicetree node */
	r = goodix_parse_dt_cfg(ts_dev, "normal-cfg",
			ts_dev->normal_cfg,
			ts_dev->chip_version.sensor_id);
	if (r < 0) {
		ts_err("Failed to obtain normal-cfg");
		return r;
	}
#endif

	ts_dev->normal_cfg->delay = 500;
	/* send normal-cfg to firmware */
	r = goodix_send_config(ts_dev, ts_dev->normal_cfg);

	return r;
}

/**
 * goodix_hw_reset - reset device
 *
 * @dev: pointer to touch device
 * Returns 0 - succeed,<0 - failed
 */
static int goodix_hw_reset(struct goodix_ts_device *dev)
{
	ts_info("HW reset");
	gpiod_direction_output(dev->board_data->reset_gpiod, 0);
	udelay(200);
	gpiod_direction_output(dev->board_data->reset_gpiod, 1);
	msleep(80);
	return 0;
}

/**
 * goodix_request_handler - handle firmware request
 *
 * @dev: pointer to touch device
 * @request_data: requset information
 * Returns 0 - succeed,<0 - failed
 */
static int goodix_request_handler(struct goodix_ts_device *dev,
		struct goodix_request_data *request_data) {
	unsigned char buffer[1];
	int r;

	r = goodix_i2c_read(dev, TS_REG_REQUEST, buffer, 1);
	if (r < 0)
		return r;

	switch (buffer[0]) {
	case REQUEST_CONFIG:
		ts_info("HW request config");
		goodix_send_config(dev, dev->normal_cfg);
		goto clear_requ;
	case REQUEST_BAKREF:
		ts_info("HW request bakref");
		goto clear_requ;
	case REQUEST_RESET:
		ts_info("HW requset reset");
		goto clear_requ;
	case REQUEST_MAINCLK:
		ts_info("HW request mainclk");
		goto clear_requ;
	default:
		ts_info("Unknown hw request:%d", buffer[0]);
		return 0;
	}

clear_requ:
	buffer[0] = 0x00;
	r = goodix_i2c_write(dev, TS_REG_REQUEST, buffer, 1);
	return r;
}

/**
 * goodix_eventt_handler - handle firmware event
 *
 * @dev: pointer to touch device
 * @ts_event: pointer to touch event structure
 * Returns 0 - succeed,<0 - failed
 */
static int goodix_event_handler(struct goodix_ts_device *dev,
		struct goodix_ts_event *ts_event)
{
#define BYTES_PER_COORD 8
	struct goodix_touch_data *touch_data =
			&ts_event->event_data.touch_data;
	struct goodix_ts_coords *coords = &touch_data->coords[0];
	int max_touch_num = dev->board_data->panel_max_id;
	unsigned char buffer[2 + BYTES_PER_COORD * max_touch_num];
	unsigned char coord_sta;
	int touch_num = 0, i, r;
	unsigned char chksum = 0;

	r = goodix_i2c_read(dev, TS_REG_COORDS_BASE,
			buffer, 3 + BYTES_PER_COORD/* * 1*/);
	if (unlikely(r < 0))
		return r;

	/* buffer[0]: event state */
	coord_sta = buffer[0];
	if (unlikely(coord_sta == 0x00)) {
		/* handle request event */
		ts_event->event_type = EVENT_REQUEST;
		goodix_request_handler(dev,
				&ts_event->event_data.request_data);
		goto exit_clean_sta;
	} else if (unlikely((coord_sta & 0x80) != 0x80)) {
		r = -EINVAL;
		return r;
	}

	/* bit7 of coord_sta is 1, touch data is ready */
	/* handle touch event */
	touch_data->key_value = (coord_sta >> 4) & 0x01;
	touch_num = coord_sta & 0x0F;
	if (unlikely(touch_num > max_touch_num)) {
		touch_num = -EINVAL;
		goto exit_clean_sta;
	} else if (unlikely(touch_num > 1)) {
		r = goodix_i2c_read(dev,
				TS_REG_COORDS_BASE + 3 + BYTES_PER_COORD,
				&buffer[3 + BYTES_PER_COORD],
				(touch_num - 1) * BYTES_PER_COORD);
		if (unlikely(r < 0))
			goto exit_clean_sta;
	}

	/* touch_num * BYTES_PER_COORD + 1(touch event state) + 1(checksum)
	 * + 1(key value)
	 */
	chksum = checksum_u8(&buffer[0], touch_num * BYTES_PER_COORD + 3);
	if (unlikely(chksum != 0)) {
		ts_err("Checksum error:%X", chksum);
		r = -EINVAL;
		goto exit_clean_sta;
	}

	memset(touch_data->coords, 0x00, sizeof(touch_data->coords));
	for (i = 0; i < touch_num; i++) {
		coords->id = buffer[i * BYTES_PER_COORD + 1] & 0x0f;
		coords->x = buffer[i * BYTES_PER_COORD + 2] |
						(buffer[i * BYTES_PER_COORD + 3] << 8);
		coords->y = buffer[i * BYTES_PER_COORD + 4] |
						(buffer[i * BYTES_PER_COORD + 5] << 8);
		coords->w = (buffer[i * BYTES_PER_COORD + 7] << 8)
							+ buffer[i * BYTES_PER_COORD + 6];
		coords->p = coords->w;

		ts_debug("D:[%d](%d, %d)[%d]", coords->id, coords->x, coords->y,
				coords->w);
		coords++;
	}

	touch_data->touch_num = touch_num;
	/* mark this event as touch event */
	ts_event->event_type = EVENT_TOUCH;
	r = 0;

exit_clean_sta:
	/* handshake */
	buffer[0] = 0x00;
	goodix_i2c_write(dev, TS_REG_COORDS_BASE, buffer, 1);
	return r;
}

/**
 * goodix_send_command - seng cmd to firmware
 *
 * @dev: pointer to device
 * @cmd: pointer to command struct which cotain command data
 * Returns 0 - succeed,<0 - failed
 */
int goodix_send_command(struct goodix_ts_device *dev,
		struct goodix_ts_cmd *cmd)
{
	int ret;

	if (!cmd || !cmd->initialized)
		return -EINVAL;
	ret = goodix_i2c_write(dev, cmd->cmd_reg, cmd->cmds,
			cmd->length);
	return ret;
}

/**
 * goodix_hw_suspend - Let touch deivce stay in lowpower mode.
 * @dev: pointer to goodix touch device
 * @return: 0 - succeed, < 0 - failed
 */
static int goodix_hw_suspend(struct goodix_ts_device *dev)
{
	struct goodix_ts_cmd *sleep_cmd =
			&dev->sleep_cmd;
	int r = 0;

	if (sleep_cmd->initialized) {
		r = goodix_send_command(dev, sleep_cmd);
		if (!r)
			ts_info("Chip in sleep mode");
	} else {
		ts_err("Uninitialized sleep command");
	}

	return r;
}

/**
 * goodix_hw_resume - Let touch deivce stay in active  mode.
 * @dev: pointer to goodix touch device
 * @return: 0 - succeed, < 0 - failed
 */
static int goodix_hw_resume(struct goodix_ts_device *dev)
{
	struct goodix_ts_version ver;
	int r, retry = GOODIX_BUS_RETRY_TIMES;

	for (; retry--;) {
		goodix_hw_reset(dev);
		r = goodix_read_version(dev, &ver);
		if (!r)
			break;
	}

	return r;
}

/* hardware opeation funstions */
static const struct goodix_ts_hw_ops hw_i2c_ops = {
	.init = goodix_hw_init,
	.read = goodix_i2c_read,
	.write = goodix_i2c_write,
	.reset = goodix_hw_reset,
	.event_handler = goodix_event_handler,
	.send_config = goodix_send_config,
	.send_cmd = goodix_send_command,
	.read_version = goodix_read_version,
	.suspend = goodix_hw_suspend,
	.resume = goodix_hw_resume,
};

static struct platform_device *goodix_pdev;
static void goodix_pdev_release(struct device *dev)
{
	kfree(goodix_pdev);
}

static int goodix_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *dev_id)
{
	struct goodix_ts_device *ts_device = NULL;
	struct goodix_ts_board_data *ts_bdata = NULL;
	int r = 0;

	r = i2c_check_functionality(client->adapter,
		I2C_FUNC_I2C);
	if (!r)
		return -EIO;

	/* board data */
	ts_bdata = devm_kzalloc(&client->dev,
			sizeof(struct goodix_ts_board_data), GFP_KERNEL);
	if (!ts_bdata)
		return -ENOMEM;

#ifdef CONFIG_OF
	if (IS_ENABLED(CONFIG_OF) && client->dev.of_node) {
		/* parse devicetree property */
		r = goodix_parse_dt(client->dev.of_node, ts_bdata);
		if (r < 0)
			return r;
	} else
#endif
	{
		/* use platform data */
		ts_info("Finally use platform data");
		devm_kfree(&client->dev, ts_bdata);
		ts_bdata = client->dev.platform_data;
	}

	if (!ts_bdata)
		return -ENODEV;

	ts_device = devm_kzalloc(&client->dev,
		sizeof(struct goodix_ts_device), GFP_KERNEL);
	if (!ts_device)
		return -ENOMEM;

	ts_device->name = "GTx5 TouchDevcie";
	ts_device->dev = &client->dev;
	ts_device->board_data = ts_bdata;
	ts_device->hw_ops = &hw_i2c_ops;

	/* ts core device */
	goodix_pdev = kzalloc(sizeof(struct platform_device), GFP_KERNEL);
	if (!goodix_pdev)
		return -ENOMEM;

	goodix_pdev->name = GOODIX_CORE_DRIVER_NAME;
	goodix_pdev->id = 0;
	goodix_pdev->num_resources = 0;
	/*
	 * you could find this platform dev in
	 * /sys/devices/platform/goodix_ts.0
	 * goodix_pdev->dev.parent = &client->dev;
	 */
	goodix_pdev->dev.platform_data = ts_device;
	goodix_pdev->dev.release = goodix_pdev_release;

	/* register platform device, then the goodix_ts_core module will probe
	 * the touch deivce.
	 */
	r = platform_device_register(goodix_pdev);
	return r;
}

static int goodix_i2c_remove(struct i2c_client *client)
{
	platform_device_unregister(goodix_pdev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id i2c_matchs[] = {
	{.compatible = TS_DT_COMPATIBLE,},
	{},
};
MODULE_DEVICE_TABLE(of, i2c_matchs);
#endif

static const struct i2c_device_id i2c_id_table[] = {
	{TS_DRIVER_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, i2c_id_table);

static struct i2c_driver goodix_i2c_driver = {
	.driver = {
		.name = TS_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(i2c_matchs),
	},
	.probe = goodix_i2c_probe,
	.remove = goodix_i2c_remove,
	.id_table = i2c_id_table,
};

static int __init goodix_i2c_init(void)
{
	ts_info("GTx5xx HW layer init");
	return i2c_add_driver(&goodix_i2c_driver);
}

static void __exit goodix_i2c_exit(void)
{
	i2c_del_driver(&goodix_i2c_driver);
}

module_init(goodix_i2c_init);
module_exit(goodix_i2c_exit);

MODULE_DESCRIPTION("Goodix GTx5 Touchscreen Hardware Module");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL v2");
