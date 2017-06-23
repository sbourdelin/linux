/*
 * Goodix GTx5 Touchscreen Dirver
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
#include <linux/ctype.h>
#include <linux/i2c.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include "gtx5_core.h"

#define TS_DRIVER_NAME		"gtx5_i2c"
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
/* set defalut irq flags as Falling edge */
#define DEFAULT_IRQ_FLAGS 2
#if TS_CFG_MAX_LEN > GTX5_CFG_MAX_SIZE
#error GTX5_CFG_MAX_SIZE too small, please fix.
#endif

#ifdef CONFIG_OF
/*
 * gtx5_parse_dt_resolution - parse resolution from dt
 * @dev: device
 * @board_data: pointer to board data structure
 * return: 0 - no error, <0 error
 */
static int gtx5_parse_dt_resolution(struct device *dev,
				      struct gtx5_ts_board_data *board_data)
{
	int r, err = 0;

	r = device_property_read_u32(dev, "touchscreen-max-id",
				     &board_data->panel_max_id);
	if (r || board_data->panel_max_id > GTX5_MAX_TOUCH)
		board_data->panel_max_id = GTX5_MAX_TOUCH;

	r = device_property_read_u32(dev, "touchscreen-size-x",
				     &board_data->panel_max_x);
	if (r)
		err = -ENOENT;

	r = device_property_read_u32(dev, "touchscreen-size-y",
				     &board_data->panel_max_y);
	if (r)
		err = -ENOENT;

	r = device_property_read_u32(dev, "touchscreen-max-w",
				     &board_data->panel_max_w);
	if (r)
		err = -ENOENT;

	board_data->swap_axis = device_property_read_bool(dev,
					"touchscreen-swapped-x-y");

	return err;
}

/**
 * gtx5_parse_dt - parse board data from dt
 * @dev: pointer to device
 * @board_data: pointer to board data structure
 * return: 0 - no error, <0 error
 */
static int gtx5_parse_dt(struct device *dev,
			   struct gtx5_ts_board_data *board_data)
{
	int r;

	if (!board_data) {
		dev_err(dev, "Invalid board data\n");
		return -EINVAL;
	}

	r = device_property_read_u32(dev, "irq-flags",
				     &board_data->irq_flags);
	if (r) {
		dev_info(dev, "Use default irq flags:falling_edge\n");
		board_data->irq_flags = DEFAULT_IRQ_FLAGS;
	}

	board_data->avdd_name = "vtouch";
	r = device_property_read_u32(dev, "power-on-delay-us",
				     &board_data->power_on_delay_us);
	if (!r) {
		/* 1000ms is too large, maybe you have pass a wrong value */
		if (board_data->power_on_delay_us > 1000 * 1000) {
			dev_warn(dev, "Power on delay time exceed 1s\n");
			board_data->power_on_delay_us = 0;
		}
	}

	r = device_property_read_u32(dev, "power-off-delay-us",
				     &board_data->power_off_delay_us);
	if (!r) {
		/* 1000ms is too large, maybe you have pass a wrong value */
		if (board_data->power_off_delay_us > 1000 * 1000) {
			dev_warn(dev, "Power off delay time exceed 1s\n");
			board_data->power_off_delay_us = 0;
		}
	}

	/* get xyz resolutions */
	r = gtx5_parse_dt_resolution(dev, board_data);
	if (r < 0) {
		dev_err(dev, "Failed to parse resolutions:%d\n", r);
		return r;
	}

	/* parse key map */
	r = device_property_read_u32_array(dev, "panel-key-map",
					   NULL, GTX5_MAX_KEY);
	if (r > 0 && r <= GTX5_MAX_KEY) {
		board_data->panel_max_key = r;
		r = device_property_read_u32_array(dev,
				"panel-key-map",
				&board_data->panel_key_map[0],
				board_data->panel_max_key);
		if (r)
			dev_err(dev, "Failed get key map info\n");
	} else {
		dev_info(dev, "No key map found\n");
	}

	dev_dbg(dev, "[DT]id:%d, x:%d, y:%d, w:%d\n",
		board_data->panel_max_id,
		board_data->panel_max_x,
		board_data->panel_max_y,
		board_data->panel_max_w);
	return 0;
}

/**
 * gtx5_parse_dt_cfg - pares config data from devicetree dev
 * @dev: pointer to device
 * @cfg_type: config type such as normal_config, highsense_cfg ...
 * @config: pointer to config data structure
 * @sensor_id: sensor id
 * return: 0 - no error, <0 error
 */
static int gtx5_parse_dt_cfg(struct gtx5_ts_device *ts_dev,
			       char *cfg_type, struct gtx5_ts_config *config,
			       unsigned int sensor_id)
{
	int r, len;
	char sub_node_name[24] = {0};
	struct fwnode_handle *fwnode;
	struct device *dev = ts_dev->dev;
	struct gtx5_ts_board_data *ts_bdata = ts_dev->board_data;

	u16 checksum;

	if (sensor_id > TS_MAX_SENSORID) {
		dev_err(dev, "Invalid sensor id\n");
		return -EINVAL;
	}

	if (config->initialized) {
		dev_dbg(dev, "Config already initialized\n");
		return 0;
	}

	/*
	 * config data are located in child node called
	 * 'sensorx', x is the sensor ID got from touch
	 * device.
	 */
	snprintf(sub_node_name, sizeof(sub_node_name),
		 "sensor%u", sensor_id);
	fwnode = device_get_named_child_node(dev, "sub_node_name");
	if (!fwnode) {
		dev_dbg(dev, "Child property[%s] not found\n",
			sub_node_name);
		return -EINVAL;
	}

	len = fwnode_property_read_u8_array(fwnode, cfg_type,
					    NULL, TS_CFG_MAX_LEN);
	if (len <= 0 || len % 2 != 1) {
		dev_err(dev, "Invalid cfg type%s, size:%u\n", cfg_type, len);
		return -EINVAL;
	}

	config->length = len;

	mutex_init(&config->lock);
	mutex_lock(&config->lock);

	r = fwnode_property_read_u8_array(fwnode, cfg_type,
					  config->data, TS_CFG_MAX_LEN);
	if (r) {
		mutex_unlock(&config->lock);
		return r;
	}

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

	dev_dbg(dev, "Config name:%s,ver:%02xh,size:%d,checksum:%04xh\n",
		config->name, config->data[0],
		config->length, checksum);
	return 0;
}
#endif

/**
 * gtx5_i2c_read - read device register through i2c bus
 * @dev: pointer to device data
 * @addr: register address
 * @data: read buffer
 * @len: bytes to read
 * return: 0 - read ok, < 0 - i2c transter error
 */
static int gtx5_i2c_read(struct gtx5_ts_device *dev, unsigned int reg,
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

		for (retry = 0; retry < GTX5_BUS_RETRY_TIMES; retry++) {
			if (likely(i2c_transfer(client->adapter, msgs, 2) == 2)) {
				memcpy(&data[pos], msgs[1].buf, transfer_length);
				pos += transfer_length;
				address += transfer_length;
				break;
			}
			dev_info(&client->dev, "I2c read retry[%d]:0x%x\n",
				 retry + 1, reg);
			msleep(20);
		}
		if (unlikely(retry == GTX5_BUS_RETRY_TIMES)) {
			dev_err(&client->dev,
				"I2c read failed,dev:%02x,reg:%04x,size:%u\n",
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
 * gtx5_i2c_write - write device register through i2c bus
 * @ts_dev: pointer to gtx5 device data
 * @addr: register address
 * @data: write buffer
 * @len: bytes to write
 * return: 0 - write ok; < 0 - i2c transter error.
 */
static int gtx5_i2c_write(struct gtx5_ts_device *ts_dev,
			    unsigned int reg,
			    unsigned char *data,
			    unsigned int len)
{
	struct i2c_client *client = to_i2c_client(ts_dev->dev);
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

		for (retry = 0; retry < GTX5_BUS_RETRY_TIMES; retry++) {
			if (likely(i2c_transfer(client->adapter, &msg, 1) == 1)) {
				pos += transfer_length;
				address += transfer_length;
				break;
			}
			dev_info(&client->dev, "I2c write retry[%d]\n", retry + 1);
			msleep(20);
		}
		if (unlikely(retry == GTX5_BUS_RETRY_TIMES)) {
			dev_err(&client->dev,
				"I2c write failed,dev:%02x,reg:%04x,size:%u",
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

static int gtx5_read_version(struct gtx5_ts_device *ts_dev,
			       struct gtx5_ts_version *version)
{
	u8 buffer[12];
	int r;

	r = gtx5_i2c_read(ts_dev, TS_REG_VERSION,
			    buffer, sizeof(buffer));
	if (r < 0) {
		dev_err(ts_dev->dev, "Read chip version failed\n");
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
			version->vid = get_unaligned_be16(&buffer[5]);
			version->sensor_id = buffer[10] & 0x0F;
			version->valid = true;

			if (version->cid)
				dev_info(ts_dev->dev,
					 "PID:%s,CID: %c,VID:%04x,SensorID:%u\n",
					 version->pid, version->cid + 'A' - 1,
					 version->vid, version->sensor_id);
			else
				dev_info(ts_dev->dev,
					 "PID:%s,VID:%04x,SensorID:%u\n",
					 version->pid, version->vid,
					 version->sensor_id);
		}
	} else {
		dev_warn(ts_dev->dev, "Checksum error:%*ph\n",
			 (int)sizeof(buffer), buffer);
		/* mark this version is invalid */
		if (version)
			version->valid = false;
		r = -EINVAL;
	}

	return r;
}

/**
 * gtx5_send_config - send config data to device.
 * @ts_dev: pointer to gtx5 device data
 * @config: pointer to config data struct to be send
 * @return: 0 - succeed, < 0 - failed
 */
static int gtx5_send_config(struct gtx5_ts_device *ts_dev,
			      struct gtx5_ts_config *config)
{
	int r = 0;

	if (!config || !config->data) {
		dev_warn(ts_dev->dev, "Null config data\n");
		return -EINVAL;
	}

	dev_dbg(ts_dev->dev, "Send %s,ver:%02xh,size:%d\n",
		config->name, config->data[0],
		config->length);

	mutex_lock(&config->lock);
	r = gtx5_i2c_write(ts_dev, config->reg_base,
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

static inline int gtx5_cmds_init(struct gtx5_ts_device *ts_dev)
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
 * gtx5_hw_init - hardware initialize
 *   Called by touch core module when bootup
 * @ts_dev: pointer to touch device
 * return: 0 - no error, <0 error
 */
static int gtx5_hw_init(struct gtx5_ts_device *ts_dev)
{
	int r;

	gtx5_cmds_init(ts_dev);

	/* gtx5_hw_init may be called many times */
	if (!ts_dev->normal_cfg) {
		ts_dev->normal_cfg = devm_kzalloc(ts_dev->dev,
				sizeof(*ts_dev->normal_cfg), GFP_KERNEL);
		if (!ts_dev->normal_cfg) {
			dev_err(ts_dev->dev,
				"Failed to alloc memory for normal cfg\n");
			return -ENOMEM;
		}
	}

	/* read chip version: PID/VID/sensor ID,etc.*/
	r = gtx5_read_version(ts_dev, &ts_dev->chip_version);
	if (r < 0)
		return r;

#ifdef CONFIG_OF
	/* parse normal-cfg from devicetree node */
	r = gtx5_parse_dt_cfg(ts_dev, "normal-cfg",
				ts_dev->normal_cfg,
				ts_dev->chip_version.sensor_id);
	if (r < 0) {
		dev_warn(ts_dev->dev, "Failed to obtain normal-cfg\n");
		return r;
	}
#endif

	ts_dev->normal_cfg->delay = 500;
	/* send normal-cfg to firmware */
	r = gtx5_send_config(ts_dev, ts_dev->normal_cfg);

	return r;
}

/**
 * gtx5_hw_reset - reset device
 *
 * @dev: pointer to touch device
 * Returns 0 - succeed,<0 - failed
 */
static void gtx5_hw_reset(struct gtx5_ts_device *dev)
{
	dev_dbg(dev->dev, "HW reset\n");

	if (!dev->board_data->reset_gpiod) {
		msleep(80);
		return;
	}
	gpiod_direction_output(dev->board_data->reset_gpiod, 0);
	usleep_range(200, 210);
	gpiod_direction_output(dev->board_data->reset_gpiod, 1);
	msleep(80);
}

/**
 * gtx5_request_handler - handle firmware request
 *
 * @dev: pointer to touch device
 * @request_data: requset information
 * Returns 0 - succeed,<0 - failed
 */
static int gtx5_request_handler(struct gtx5_ts_device *dev,
		struct gtx5_request_data *request_data) {
	unsigned char buffer[1];
	int r;

	r = gtx5_i2c_read(dev, TS_REG_REQUEST, buffer, 1);
	if (r < 0)
		return r;

	switch (buffer[0]) {
	case REQUEST_CONFIG:
		dev_dbg(dev->dev, "HW request config\n");
		gtx5_send_config(dev, dev->normal_cfg);
		goto clear_requ;
	case REQUEST_BAKREF:
		dev_dbg(dev->dev, "HW request bakref\n");
		goto clear_requ;
	case REQUEST_RESET:
		dev_dbg(dev->dev, "HW requset reset\n");
		goto clear_requ;
	case REQUEST_MAINCLK:
		dev_dbg(dev->dev, "HW request mainclk\n");
		goto clear_requ;
	default:
		dev_dbg(dev->dev, "Unknown hw request:%d\n", buffer[0]);
		return 0;
	}

clear_requ:
	buffer[0] = 0x00;
	r = gtx5_i2c_write(dev, TS_REG_REQUEST, buffer, 1);
	return r;
}

/**
 * gtx5_eventt_handler - handle firmware event
 *
 * @dev: pointer to touch device
 * @ts_event: pointer to touch event structure
 * Returns 0 - succeed,<0 - failed
 */
static int gtx5_event_handler(struct gtx5_ts_device *dev,
				struct gtx5_ts_event *ts_event)
{
#define BYTES_PER_COORD 8
	struct gtx5_touch_data *touch_data =
			&ts_event->event_data.touch_data;
	struct gtx5_ts_coords *coords = &touch_data->coords[0];
	int max_touch_num = dev->board_data->panel_max_id;
	unsigned char buffer[2 + BYTES_PER_COORD * max_touch_num];
	unsigned char coord_sta;
	int touch_num = 0, i, r;
	unsigned char chksum = 0;

	r = gtx5_i2c_read(dev, TS_REG_COORDS_BASE,
			    buffer, 3 + BYTES_PER_COORD/* * 1*/);
	if (unlikely(r < 0))
		return r;

	/* buffer[0]: event state */
	coord_sta = buffer[0];
	if (unlikely(coord_sta == 0x00)) {
		/* handle request event */
		ts_event->event_type = EVENT_REQUEST;
		gtx5_request_handler(dev,
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
		r = gtx5_i2c_read(dev,
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
		dev_warn(dev->dev, "Checksum error:%X\n", chksum);
		r = -EINVAL;
		goto exit_clean_sta;
	}

	memset(touch_data->coords, 0x00, sizeof(touch_data->coords));
	for (i = 0; i < touch_num; i++) {
		coords->id = buffer[i * BYTES_PER_COORD + 1] & 0x0f;
		coords->x = get_unaligned_le16(&buffer[i * BYTES_PER_COORD + 2]);
		coords->y = get_unaligned_le16(&buffer[i * BYTES_PER_COORD + 4]);
		coords->w = get_unaligned_le16(&buffer[i * BYTES_PER_COORD + 6]);

		dev_dbg(dev->dev, "D:[%d](%d, %d)[%d]\n",
			coords->id, coords->x, coords->y, coords->w);
		coords++;
	}

	touch_data->touch_num = touch_num;
	/* mark this event as touch event */
	ts_event->event_type = EVENT_TOUCH;
	r = 0;

exit_clean_sta:
	/* handshake */
	buffer[0] = 0x00;
	gtx5_i2c_write(dev, TS_REG_COORDS_BASE, buffer, 1);
	return r;
}

/**
 * gtx5_send_command - seng cmd to firmware
 *
 * @dev: pointer to device
 * @cmd: pointer to command struct which cotain command data
 * Returns 0 - succeed,<0 - failed
 */
int gtx5_send_command(struct gtx5_ts_device *dev,
			struct gtx5_ts_cmd *cmd)
{
	int ret;

	if (!cmd || !cmd->initialized)
		return -EINVAL;
	ret = gtx5_i2c_write(dev, cmd->cmd_reg, cmd->cmds,
			       cmd->length);
	return ret;
}

/**
 * gtx5_hw_suspend - Let touch device stay in lowpower mode.
 * @dev: pointer to gtx5 touch device
 * @return: 0 - succeed, < 0 - failed
 */
static int gtx5_hw_suspend(struct gtx5_ts_device *dev)
{
	struct gtx5_ts_cmd *sleep_cmd =
			&dev->sleep_cmd;
	int r = 0;

	if (sleep_cmd->initialized) {
		r = gtx5_send_command(dev, sleep_cmd);
		if (!r)
			dev_dbg(dev->dev, "Chip in sleep mode\n");
	} else {
		dev_dbg(dev->dev, "Uninitialized sleep command\n");
	}

	return r;
}

/**
 * gtx5_hw_resume - Let touch device stay in active  mode.
 * @dev: pointer to gtx5 touch device
 * @return: 0 - succeed, < 0 - failed
 */
static int gtx5_hw_resume(struct gtx5_ts_device *dev)
{
	struct gtx5_ts_version ver;
	int r, retry = GTX5_BUS_RETRY_TIMES;

	for (; retry--;) {
		gtx5_hw_reset(dev);
		r = gtx5_read_version(dev, &ver);
		if (!r)
			break;
	}

	return r;
}

/* hardware opeation funstions */
static const struct gtx5_ts_hw_ops hw_i2c_ops = {
	.init = gtx5_hw_init,
	.read = gtx5_i2c_read,
	.write = gtx5_i2c_write,
	.reset = gtx5_hw_reset,
	.event_handler = gtx5_event_handler,
	.send_config = gtx5_send_config,
	.send_cmd = gtx5_send_command,
	.read_version = gtx5_read_version,
	.suspend = gtx5_hw_suspend,
	.resume = gtx5_hw_resume,
};

static struct platform_device *gtx5_pdev;
static void gtx5_pdev_release(struct device *dev)
{
	kfree(gtx5_pdev);
}

static int gtx5_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *dev_id)
{
	struct gtx5_ts_device *ts_device = NULL;
	struct gtx5_ts_board_data *ts_bdata = NULL;
	int r = 0;

	r = i2c_check_functionality(client->adapter,
				    I2C_FUNC_I2C);
	if (!r)
		return -EIO;

	/* board data */
	ts_bdata = devm_kzalloc(&client->dev,
			sizeof(struct gtx5_ts_board_data), GFP_KERNEL);
	if (!ts_bdata)
		return -ENOMEM;

#ifdef CONFIG_OF
	if (IS_ENABLED(CONFIG_OF) && client->dev.of_node) {
		/* parse devicetree property */
		r = gtx5_parse_dt(&client->dev, ts_bdata);
		if (r < 0)
			return r;
	} else
#endif
	{
		/* use platform data */
		dev_info(&client->dev, "use platform data\n");
		devm_kfree(&client->dev, ts_bdata);
		ts_bdata = client->dev.platform_data;
	}

	if (!ts_bdata)
		return -ENODEV;

	ts_device = devm_kzalloc(&client->dev,
				 sizeof(struct gtx5_ts_device), GFP_KERNEL);
	if (!ts_device)
		return -ENOMEM;

	ts_bdata->irq = client->irq;
	ts_device->name = "GTx5 TouchDevcie";
	ts_device->dev = &client->dev;
	ts_device->board_data = ts_bdata;
	ts_device->hw_ops = &hw_i2c_ops;

	/* ts core device */
	gtx5_pdev = kzalloc(sizeof(*gtx5_pdev), GFP_KERNEL);
	if (!gtx5_pdev)
		return -ENOMEM;

	gtx5_pdev->name = GTX5_CORE_DRIVER_NAME;
	gtx5_pdev->id = 0;
	gtx5_pdev->num_resources = 0;
	/*
	 * you could find this platform dev in
	 * /sys/devices/platform/gtx5_ts.0
	 * gtx5_pdev->dev.parent = &client->dev;
	 */
	gtx5_pdev->dev.platform_data = ts_device;
	gtx5_pdev->dev.release = gtx5_pdev_release;

	/* register platform device, then the gtx5_ts_core module will probe
	 * the touch device.
	 */
	r = platform_device_register(gtx5_pdev);
	return r;
}

static int gtx5_i2c_remove(struct i2c_client *client)
{
	platform_device_unregister(gtx5_pdev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id gtx5_of_matchs[] = {
	{.compatible = "goodix,gt7589"},
	{.compatible = "goodix,gt8589"},
	{.compatible = "goodix,gt9589"},
	{},
};
MODULE_DEVICE_TABLE(of, gtx5_of_matchs);
#endif

static const struct i2c_device_id gtx5_id_table[] = {
	{TS_DRIVER_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, gtx5_id_table);

static struct i2c_driver gtx5_i2c_driver = {
	.driver = {
		.name = TS_DRIVER_NAME,
		.of_match_table = of_match_ptr(gtx5_of_matchs),
	},
	.probe = gtx5_i2c_probe,
	.remove = gtx5_i2c_remove,
	.id_table = gtx5_id_table,
};
module_i2c_driver(gtx5_i2c_driver);

MODULE_DESCRIPTION("Goodix GTx5 Touchscreen Hardware Module");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL v2");
