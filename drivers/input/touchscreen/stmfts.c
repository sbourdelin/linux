/*
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 * Author: Andi Shyti <andi.shyti@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * STMicroelectronics FTS Touchscreen device driver
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

/* I2C commands */
#define STMFTS_READ_INFO			0x80
#define STMFTS_READ_STATUS			0x84
#define STMFTS_READ_ONE_EVENT			0x85
#define STMFTS_SLEEP_IN				0x91
#define STMFTS_SLEEP_OUT			0x91
#define STMFTS_MS_MT_SENSE_OFF			0x92
#define STMFTS_MS_MT_SENSE_ON			0x93
#define STMFTS_SS_HOVER_SENSE_OFF		0x94
#define STMFTS_SS_HOVER_SENSE_ON		0x95
#define STMFTS_MS_KEY_SENSE_OFF			0x9a
#define STMFTS_MS_KEY_SENSE_ON			0x9b
#define STMFTS_SYSTEM_RESET			0xa0
#define STMFTS_CLEAR_EVENT_STACK		0xa1
#define STMFTS_FULL_FORCE_CALIBRATION		0xa2
#define STMFTS_MS_CX_TUNING			0xa3
#define STMFTS_SS_CX_TUNING			0xa4

/* events */
#define STMFTS_EV_NO_EVENT			0x00
#define STMFTS_EV_MULTI_TOUCH_DETECTED		0x02
#define STMFTS_EV_MULTI_TOUCH_ENTER		0x03
#define STMFTS_EV_MULTI_TOUCH_LEAVE		0x04
#define STMFTS_EV_MULTI_TOUCH_MOTION		0x05
#define STMFTS_EV_HOVER_ENTER			0x07
#define STMFTS_EV_HOVER_LEAVE			0x08
#define STMFTS_EV_HOVER_MOTION			0x09
#define STMFTS_EV_KEY_STATUS			0x0e
#define STMFTS_EV_ERROR				0x0f
#define STMFTS_EV_CONTROLLER_READY		0x10
#define STMFTS_EV_SLEEP_OUT_CONTROLLER_READY	0x11
#define STMFTS_EV_STATUS			0x16

/* multi touch related event masks */
#define STMFTS_MASK_EVENT_ID			0x0f
#define STMFTS_MASK_TOUCH_ID			0xf0
#define STMFTS_MASK_LEFT_EVENT			0x0f
#define STMFTS_MASK_X_MSB			0x0f
#define STMFTS_MASK_Y_LSB			0xf0

/* key related event masks */
#define STMFTS_MASK_KEY_NO_TOUCH		0x00
#define STMFTS_MASK_KEY_BACK			0x01
#define STMFTS_MASK_KEY_MENU			0x02

#define STMFTS_EVENT_SIZE	8
#define STMFTS_MAX_FINGERS	10
#define STMFTS_DEV_NAME		"stmfts"

enum stmfts_regulators {
	STMFTS_REGULATOR_VDD,
	STMFTS_REGULATOR_AVDD,
};

struct stmfts_data {
	struct i2c_client *client;
	struct input_dev *input_touch;
	struct input_dev *input_key;
	struct led_classdev led_cdev;
	struct mutex mutex;
	u32 x_size;
	u32 y_size;

	struct regulator_bulk_data regulators[2];

	/* ledvdd will be used also to check
	 * whether the LED is supported
	 */
	struct regulator *ledvdd;

	bool use_key;
	bool signal;
	bool led_status;
	u8 users;

	u16 chip_id;
	u8 chip_ver;
	u16 fw_ver;
	u8 config_id;
	u8 config_ver;
	u8 in_touch;

	wait_queue_head_t wq;

	bool hover_enabled;
};

static void stmfts_brightness_set(struct led_classdev *led_cdev,
					enum led_brightness value)
{
	struct stmfts_data *sdata = container_of(led_cdev,
					struct stmfts_data, led_cdev);

	if (value == sdata->led_status)
		return;

	if (!value) {
		regulator_disable(sdata->ledvdd);
	} else {
		int err = regulator_enable(sdata->ledvdd);

		if (err)
			dev_warn(&sdata->client->dev,
				"failed to disable ledvdd regulator\n");
	}

	sdata->led_status = value;
}

static enum led_brightness stmfts_brightness_get(struct led_classdev *led_cdev)
{
	struct stmfts_data *sdata = container_of(led_cdev,
						struct stmfts_data, led_cdev);

	return !!regulator_is_enabled(sdata->ledvdd);
}

static void stmfts_parse_event(struct stmfts_data *sdata, u8 event[])
{
	int ret;
	u8 id, t_id = 0;
	u16 x, y, z, maj, min, orientation, area;

	id = event[0];

	do {
		mutex_lock(&sdata->mutex);
		if (sdata->in_touch) {
			id = event[0] & STMFTS_MASK_EVENT_ID;
			t_id = (event[0] & STMFTS_MASK_TOUCH_ID) >> 4;
		} else {
			id = event[0];
			t_id = 0;
		}

		switch (id) {
		case STMFTS_EV_NO_EVENT:
			break;

		case STMFTS_EV_MULTI_TOUCH_ENTER:
		case STMFTS_EV_MULTI_TOUCH_LEAVE:
		case STMFTS_EV_MULTI_TOUCH_MOTION:
			if (id == STMFTS_EV_MULTI_TOUCH_ENTER) {
				if (!(sdata->in_touch++))
					input_mt_report_slot_state(
							sdata->input_touch,
							MT_TOOL_FINGER, true);
			} else if (id == STMFTS_EV_MULTI_TOUCH_LEAVE) {
				if (!(--sdata->in_touch))
					input_mt_report_slot_state(
							sdata->input_touch,
							MT_TOOL_FINGER, false);
			}

			x = event[1] | ((event[2] & STMFTS_MASK_X_MSB) << 8);
			y = (event[2] >> 4) | (event[3] << 4);

			maj = event[4];
			min = event[5];
			orientation = event[6];
			area = event[7];

			input_mt_slot(sdata->input_touch, t_id);
			input_report_abs(sdata->input_touch,
					ABS_MT_POSITION_X, x);
			input_report_abs(sdata->input_touch,
					ABS_MT_POSITION_Y, y);
			input_report_abs(sdata->input_touch,
					ABS_MT_TOUCH_MAJOR, maj);
			input_report_abs(sdata->input_touch,
					ABS_MT_TOUCH_MINOR, min);
			input_report_abs(sdata->input_touch,
					ABS_MT_PRESSURE, area);
			input_report_abs(sdata->input_touch,
					ABS_MT_ORIENTATION, orientation);
			input_sync(sdata->input_touch);

			break;

		case STMFTS_EV_HOVER_ENTER:
		case STMFTS_EV_HOVER_LEAVE:
		case STMFTS_EV_HOVER_MOTION:
			x = (event[2] << 4) | (event[4] >> 4);
			y = (event[3] << 4) | (event[4] & STMFTS_MASK_Y_LSB);
			z = event[5];
			orientation = event[6] & STMFTS_MASK_Y_LSB;

			input_report_abs(sdata->input_touch, ABS_X, x);
			input_report_abs(sdata->input_touch, ABS_Y, y);
			input_report_abs(sdata->input_touch, ABS_Z, z);
			input_sync(sdata->input_touch);

			break;

		case STMFTS_EV_KEY_STATUS:
			if (!event[2]) {
				input_report_key(sdata->input_key, KEY_BACK, 0);
				input_report_key(sdata->input_key, KEY_MENU, 0);
			} else {
				if (event[2] == STMFTS_MASK_KEY_BACK)
					input_report_key(sdata->input_key,
								KEY_BACK, 1);
				else if (event[2] == STMFTS_MASK_KEY_MENU)
					input_report_key(sdata->input_key,
								KEY_MENU, 1);
				else /* quite impossible */
					break;
			}
			input_sync(sdata->input_key);
			break;

		case STMFTS_EV_STATUS:
			sdata->signal = true;
			wake_up_interruptible(&sdata->wq);
			break;

		case STMFTS_EV_ERROR:
			dev_err(&sdata->client->dev,
				"error code: 0x%x%x%x%x%x%x",
				event[6], event[5], event[4],
				event[3], event[2], event[1]);
			break;

		default:
			dev_err(&sdata->client->dev,
				"unknown event 0x%x\n", event[0]);
		}
		mutex_unlock(&sdata->mutex);

		ret = i2c_smbus_read_i2c_block_data(sdata->client,
						STMFTS_READ_ONE_EVENT,
						STMFTS_EVENT_SIZE, event);
		if (ret) {
			i2c_smbus_write_byte(sdata->client,
						STMFTS_CLEAR_EVENT_STACK);
			break;
		}

	} while (event[0]);
}

static irqreturn_t stmfts_irq_handler(int irq, void *dev)
{
	struct stmfts_data *sdata = dev;
	int ret;
	u8 event[STMFTS_EVENT_SIZE];

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					STMFTS_READ_ONE_EVENT,
					STMFTS_EVENT_SIZE, event);

	switch (event[0]) {
	case STMFTS_EV_CONTROLLER_READY:
	case STMFTS_EV_SLEEP_OUT_CONTROLLER_READY:
		sdata->signal = true;
		wake_up_interruptible(&sdata->wq);
		break;
	default:
		stmfts_parse_event(sdata, event);
	}

	return IRQ_HANDLED;
}

static int stmfts_write_and_wait(struct stmfts_data *sdata, const u8 cmd)
{
	int err;

	err = i2c_smbus_write_byte(sdata->client, cmd);
	if (err)
		return err;

	sdata->signal = false;
	err = wait_event_interruptible_timeout(sdata->wq, sdata->signal,
						msecs_to_jiffies(1000));

	return !err ? -ETIMEDOUT : 0;
}

static void stmfts_sleep_in(struct stmfts_data *sdata)
{
	mutex_lock(&sdata->mutex);

	sdata->users--;
	if (!sdata->users)
		i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_IN);

	mutex_unlock(&sdata->mutex);
}

static int stmfts_sleep_out(struct stmfts_data *sdata, u8 cmd)
{
	int ret = 0;

	mutex_lock(&sdata->mutex);

	if (!sdata->users)
		ret = stmfts_write_and_wait(sdata, STMFTS_SLEEP_OUT);

	/* if sleep out succeeds users increments, otherwise not */
	sdata->users += !ret;

	mutex_unlock(&sdata->mutex);

	ret = i2c_smbus_write_byte(sdata->client, cmd);
	if (ret)
		stmfts_sleep_in(sdata);

	return ret;
}

static int stmfts_input_touch_open(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);

	return stmfts_sleep_out(sdata, STMFTS_MS_MT_SENSE_ON);
}

static void stmfts_input_touch_close(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);

	i2c_smbus_write_byte(sdata->client, STMFTS_MS_MT_SENSE_OFF);

	stmfts_sleep_in(sdata);
}

static int stmfts_input_key_open(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);

	return stmfts_sleep_out(sdata, STMFTS_MS_KEY_SENSE_ON);
}

static void stmfts_input_key_close(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);

	i2c_smbus_write_byte(sdata->client, STMFTS_MS_KEY_SENSE_OFF);
	stmfts_sleep_in(sdata);
}

static ssize_t stmfts_sysfs_hwid(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sprintf(buf, "ST-Microelectronics FTS 0x%x version %u\n",
					sdata->chip_id, sdata->chip_ver);
}

static ssize_t stmfts_sysfs_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", sdata->fw_ver);
}

static ssize_t stmfts_sysfs_config_id(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sprintf(buf, "0x%x version %u\n",
			sdata->config_id, sdata->config_ver);
}

static ssize_t stmfts_sysfs_read_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);
	u8 status[4];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					STMFTS_READ_STATUS, 4, status);

	return sprintf(buf, "0x%x\n", status[0]);
}

static ssize_t stmfts_sysfs_hover_enable_read(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", sdata->hover_enabled);
}

static ssize_t stmfts_sysfs_hover_enable_write(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	unsigned long value;
	int err;
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	if (kstrtoul(buf, 0, &value))
		return -EINVAL;

	if (value & sdata->hover_enabled)
		return len;

	if (value) {
		err = stmfts_sleep_out(sdata, STMFTS_SS_HOVER_SENSE_ON);
		sdata->hover_enabled = !err;
	} else {
		err = i2c_smbus_write_byte(sdata->client,
					STMFTS_SS_HOVER_SENSE_OFF);
		stmfts_sleep_in(sdata);
		sdata->hover_enabled = !!err;
	}

	return len;
}

static DEVICE_ATTR(hwid, 0444, stmfts_sysfs_hwid, NULL);
static DEVICE_ATTR(fw_ver, 0444, stmfts_sysfs_fw_ver, NULL);
static DEVICE_ATTR(config_id, 0444, stmfts_sysfs_config_id, NULL);
static DEVICE_ATTR(status, 0444, stmfts_sysfs_read_status, NULL);
static DEVICE_ATTR(hover_enable, 0644, stmfts_sysfs_hover_enable_read,
					stmfts_sysfs_hover_enable_write);

static struct attribute *stmfts_sysfs_attrs[] = {
	&dev_attr_hwid.attr,
	&dev_attr_fw_ver.attr,
	&dev_attr_config_id.attr,
	&dev_attr_status.attr,
	&dev_attr_hover_enable.attr,
	NULL
};

static struct attribute_group stmfts_attribute_group = {
	.attrs = stmfts_sysfs_attrs
};

static int stmfts_parse_dt(struct stmfts_data *sdata)
{
	int ret;
	struct device_node *np = sdata->client->dev.of_node;

	if (!np)
		return -ENOENT;

	ret = of_property_read_u32(np, "touchscreen-size-x", &sdata->x_size);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "touchscreen-size-y", &sdata->y_size);
	if (ret)
		return ret;

	sdata->use_key = of_property_read_bool(np, "touch-key-connected");

	return 0;
}

static int stmfts_power_on(struct stmfts_data *sdata)
{
	int err;
	u8 reg[8];

	err = regulator_bulk_enable(ARRAY_SIZE(sdata->regulators),
							sdata->regulators);
	if (err)
		return err;

	/*
	 * the datasheet does not specify the power on time, but considering
	 * that the reset time is < 10ms, I sleep 20ms to be sure
	 */
	msleep(20);

	err = i2c_smbus_read_i2c_block_data(sdata->client,
					STMFTS_READ_INFO, 8, reg);
	if (err < 0)
		return err;
	if (err != 8)
		return -EIO;

	sdata->chip_id = (reg[6] << 8) | reg[7];
	sdata->chip_ver = reg[0];
	sdata->fw_ver = (reg[2] << 8) | reg[3];
	sdata->config_id = reg[4];
	sdata->config_ver = reg[5];

	enable_irq(sdata->client->irq);
	err = stmfts_write_and_wait(sdata, STMFTS_SYSTEM_RESET);
	if (err)
		return err;

	err = stmfts_write_and_wait(sdata, STMFTS_SLEEP_OUT);
	if (err)
		return err;

	/* optional tuning */
	err = stmfts_write_and_wait(sdata, STMFTS_MS_CX_TUNING);
	if (err)
		dev_warn(&sdata->client->dev, "failed to perform mutual auto tune\n");

	/* optional tuning */
	err = stmfts_write_and_wait(sdata, STMFTS_SS_CX_TUNING);
	if (err)
		dev_warn(&sdata->client->dev, "failed to perform self auto tune\n");

	err = stmfts_write_and_wait(sdata, STMFTS_FULL_FORCE_CALIBRATION);
	if (err)
		return err;

	/* at this point no one is using the touchscreen
	 * and I don't really care about the return value
	 */
	i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_IN);

	return 0;
}

static void stmfts_power_off(void *data)
{
	struct stmfts_data *sdata = data;

	disable_irq(sdata->client->irq);
	regulator_bulk_disable(ARRAY_SIZE(sdata->regulators),
						sdata->regulators);
}

static int stmfts_enable_key(struct stmfts_data *sdata)
{
	int err;

	sdata->input_key = devm_input_allocate_device(&sdata->client->dev);
	if (!sdata->input_key)
		return -ENOMEM;

	sdata->input_key->name = "stmfts_key";
	sdata->input_key->id.bustype = BUS_I2C;
	sdata->input_key->open = stmfts_input_key_open;
	sdata->input_key->close = stmfts_input_key_close;

	input_set_capability(sdata->input_key, EV_KEY, KEY_MENU);
	input_set_capability(sdata->input_key, EV_KEY, KEY_BACK);

	input_set_drvdata(sdata->input_key, sdata);
	err = input_register_device(sdata->input_key);
	if (err)
		return err;

	/* get the regulator for powering the leds on */
	sdata->ledvdd = devm_regulator_get(&sdata->client->dev, "ledvdd");
	if (IS_ERR(sdata->ledvdd))
		/* there is no LED connected to the touch key */
		sdata->ledvdd = NULL;

	sdata->led_cdev.name = STMFTS_DEV_NAME;
	sdata->led_cdev.max_brightness = LED_ON;
	sdata->led_cdev.brightness = LED_OFF;
	sdata->led_cdev.brightness_set = stmfts_brightness_set;
	sdata->led_cdev.brightness_get = stmfts_brightness_get;

	err = devm_led_classdev_register(&sdata->client->dev, &sdata->led_cdev);
	if (err) {
		dev_warn(&sdata->client->dev, "unable to register led, led might not work\n");
		sdata->ledvdd = NULL;

		/* I don't want to prevent using the touch key
		 * only because the LEDs don't get registered
		 */
		err = 0;
	}

	return err;
}

static int stmfts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err;
	struct stmfts_data *sdata;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
						I2C_FUNC_SMBUS_BYTE_DATA |
						I2C_FUNC_SMBUS_I2C_BLOCK))
		return -ENODEV;

	sdata = devm_kzalloc(&client->dev, sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	i2c_set_clientdata(client, sdata);

	mutex_init(&sdata->mutex);

	sdata->regulators[STMFTS_REGULATOR_VDD].supply = "vdd";
	sdata->regulators[STMFTS_REGULATOR_AVDD].supply = "avdd";
	err = devm_regulator_bulk_get(&client->dev,
			ARRAY_SIZE(sdata->regulators), sdata->regulators);
	if (err)
		return err;

	err = devm_add_action_or_reset(&client->dev, stmfts_power_off, sdata);
	if (err)
		return err;

	sdata->client = client;

	err = stmfts_parse_dt(sdata);
	if (err)
		return err;

	init_waitqueue_head(&sdata->wq);

	err = sysfs_create_group(&sdata->client->dev.kobj,
					&stmfts_attribute_group);
	if (err)
		return err;

	err = devm_request_threaded_irq(&client->dev, client->irq,
					NULL, stmfts_irq_handler,
					IRQF_ONESHOT | IRQF_TRIGGER_LOW,
					"stmfts_irq", sdata);
	if (err)
		return err;

	disable_irq(sdata->client->irq);

	dev_info(&client->dev, "initializing ST-Microelectronics FTS...\n");
	err = stmfts_power_on(sdata);
	if (err)
		return err;

	sdata->input_touch = devm_input_allocate_device(&client->dev);
	if (!sdata->input_touch)
		return -ENOMEM;

	sdata->input_touch->name = STMFTS_DEV_NAME;
	sdata->input_touch->id.bustype = BUS_I2C;
	sdata->input_touch->open = stmfts_input_touch_open;
	sdata->input_touch->close = stmfts_input_touch_close;

	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_POSITION_Y);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_TOUCH_MAJOR);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_TOUCH_MINOR);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_ORIENTATION);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_PRESSURE);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_MT_POSITION_X);

	input_set_abs_params(sdata->input_touch,
				ABS_MT_POSITION_X, 0, sdata->x_size, 0, 0);
	input_set_abs_params(sdata->input_touch,
				ABS_MT_POSITION_Y, 0, sdata->y_size, 0, 0);
	input_set_abs_params(sdata->input_touch,
				ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input_touch,
				ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input_touch,
				ABS_MT_ORIENTATION, 0, 255, 0, 0);
	input_set_abs_params(sdata->input_touch,
				ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_mt_init_slots(sdata->input_touch,
				STMFTS_MAX_FINGERS, INPUT_MT_DIRECT);

	/* for hover features */
	input_set_capability(sdata->input_touch, EV_ABS, ABS_X);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_Y);
	input_set_capability(sdata->input_touch, EV_ABS, ABS_Z);
	input_set_abs_params(sdata->input_touch, ABS_X, 0, sdata->x_size, 0, 0);
	input_set_abs_params(sdata->input_touch, ABS_Y, 0, sdata->y_size, 0, 0);
	input_set_abs_params(sdata->input_touch, ABS_Z, 0, 255, 0, 0);

	input_set_drvdata(sdata->input_touch, sdata);
	err = input_register_device(sdata->input_touch);
	if (err)
		return err;

	if (sdata->use_key) {
		err = stmfts_enable_key(sdata);
		if (err)
			dev_warn(&client->dev, "failed to enable touchkey\n");
	}

	return 0;
}

static int stmfts_remove(struct i2c_client *client)
{
	struct stmfts_data *sdata = i2c_get_clientdata(client);

	sysfs_remove_group(&sdata->client->dev.kobj, &stmfts_attribute_group);

	return 0;
}

static int __maybe_unused stmfts_suspend(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	stmfts_power_off(sdata);

	return 0;
}

static int __maybe_unused stmfts_resume(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return stmfts_power_on(sdata);
}

static SIMPLE_DEV_PM_OPS(stmfts_pm_ops, stmfts_suspend, stmfts_resume);

static const struct of_device_id stmfts_of_match[] = {
	{ .compatible = "st,stmfts", },
	{ },
};
MODULE_DEVICE_TABLE(of, stmfts_of_match);

static const struct i2c_device_id stmfts_id[] = {
	{ "stmfts", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, stmfts_id);

static struct i2c_driver stmfts_driver = {
	.driver = {
		.name = STMFTS_DEV_NAME,
		.of_match_table = of_match_ptr(stmfts_of_match),
		.pm = &stmfts_pm_ops,
	},
	.probe = stmfts_probe,
	.remove = stmfts_remove,
	.id_table = stmfts_id,
};

module_i2c_driver(stmfts_driver);

MODULE_AUTHOR("Andi Shyti <andi.shyti@samsung.com>");
MODULE_DESCRIPTION("STMicroelectronics FTS Touch Screen");
MODULE_LICENSE("GPL v2");
