/*
 * TM2 touchkey device driver
 *
 * Copyright 2005 Phil Blundell
 * Copyright 2016 Samsung Electronics Co., Ltd.
 *
 * Author: Beomho Seo <beomho.seo@samsung.com>
 * Author: Jaechul Lee <jcsing.lee@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>

#define TM2_TOUCHKEY_DEV_NAME		"tm2-touchkey"
#define TM2_TOUCHKEY_KEYCODE_REG	0x03
#define TM2_TOUCHKEY_BASE_REG		0x00
#define TM2_TOUCHKEY_CMD_LED_ON		0x10
#define TM2_TOUCHKEY_CMD_LED_OFF	0x20
#define TM2_TOUCHKEY_BIT_PRESS_EV	BIT(3)
#define TM2_TOUCHKEY_BIT_KEYCODE	GENMASK(2, 0)
#define TM2_TOUCHKEY_LED_VOLTAGE_MIN	2500000
#define TM2_TOUCHKEY_LED_VOLTAGE_MAX	3300000

enum {
	TM2_TOUCHKEY_KEY_MENU = 0x1,
	TM2_TOUCHKEY_KEY_BACK,
};

struct tm2_touchkey_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct led_classdev led_dev;
	struct regulator_bulk_data regulators[2];

	u8 keycode_type;
	u8 pressed;
};

static void tm2_touchkey_led_brightness_set(struct led_classdev *led_dev,
						enum led_brightness brightness)
{
	struct tm2_touchkey_data *touchkey =
	    container_of(led_dev, struct tm2_touchkey_data, led_dev);
	u32 volt;
	u8 data;

	if (brightness == LED_OFF) {
		volt = TM2_TOUCHKEY_LED_VOLTAGE_MIN;
		data = TM2_TOUCHKEY_CMD_LED_OFF;
	} else {
		volt = TM2_TOUCHKEY_LED_VOLTAGE_MAX;
		data = TM2_TOUCHKEY_CMD_LED_ON;
	}

	regulator_set_voltage(touchkey->regulators[1].consumer, volt, volt);
	i2c_smbus_write_byte_data(touchkey->client,
						TM2_TOUCHKEY_BASE_REG, data);
}

static int tm2_touchkey_power_enable(struct tm2_touchkey_data *touchkey)
{
	int ret = 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(touchkey->regulators),
						touchkey->regulators);
	if (ret)
		return ret;

	/* waiting for device initialization, at least 150ms */
	msleep(150);

	return 0;
}

static void tm2_touchkey_power_disable(void *data)
{
	struct tm2_touchkey_data *touchkey = data;

	regulator_bulk_disable(ARRAY_SIZE(touchkey->regulators),
						touchkey->regulators);
}

static irqreturn_t tm2_touchkey_irq_handler(int irq, void *devid)
{
	struct tm2_touchkey_data *touchkey = devid;
	u32 data;

	data = i2c_smbus_read_byte_data(touchkey->client,
					TM2_TOUCHKEY_KEYCODE_REG);

	if (data < 0) {
		dev_err(&touchkey->client->dev, "Failed to read i2c data\n");
		return IRQ_HANDLED;
	}

	touchkey->keycode_type = data & TM2_TOUCHKEY_BIT_KEYCODE;
	touchkey->pressed = !(data & TM2_TOUCHKEY_BIT_PRESS_EV);

	if (touchkey->keycode_type != TM2_TOUCHKEY_KEY_MENU &&
	    touchkey->keycode_type != TM2_TOUCHKEY_KEY_BACK) {
		dev_warn(&touchkey->client->dev, "Skip unhandled keycode(%d)\n",
							touchkey->keycode_type);
		return IRQ_HANDLED;
	}

	if (!touchkey->pressed) {
		input_report_key(touchkey->input_dev, KEY_PHONE, 0);
		input_report_key(touchkey->input_dev, KEY_BACK, 0);
	} else {
		if (touchkey->keycode_type == TM2_TOUCHKEY_KEY_MENU)
			input_report_key(touchkey->input_dev,
					 KEY_PHONE, 1);
		else
			input_report_key(touchkey->input_dev,
					 KEY_BACK, 1);
	}
	input_sync(touchkey->input_dev);

	return IRQ_HANDLED;
}

static int tm2_touchkey_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct tm2_touchkey_data *touchkey;
	int ret;

	ret = i2c_check_functionality(client->adapter,
				      I2C_FUNC_SMBUS_BYTE |
				      I2C_FUNC_SMBUS_BYTE_DATA);
	if (!ret) {
		dev_err(&client->dev, "No I2C functionality found\n");
		return -ENODEV;
	}

	touchkey = devm_kzalloc(&client->dev, sizeof(*touchkey), GFP_KERNEL);
	if (!touchkey)
		return -ENOMEM;

	touchkey->client = client;
	i2c_set_clientdata(client, touchkey);

	/* regulators */
	touchkey->regulators[0].supply = "vcc";
	touchkey->regulators[1].supply = "vdd";
	ret = devm_regulator_bulk_get(&client->dev,
					ARRAY_SIZE(touchkey->regulators),
					touchkey->regulators);
	if (ret) {
		dev_err(&client->dev, "Failed to get regulators\n");
		return ret;
	}

	/* power */
	ret = tm2_touchkey_power_enable(touchkey);
	if (ret) {
		dev_err(&client->dev, "Failed to enable power\n");
		return ret;
	}

	ret = devm_add_action_or_reset(&client->dev,
					tm2_touchkey_power_disable, touchkey);
	if (ret)
		return ret;

	/* input device */
	touchkey->input_dev = devm_input_allocate_device(&client->dev);
	if (!touchkey->input_dev) {
		dev_err(&client->dev, "Failed to alloc input device\n");
		return -ENOMEM;
	}
	touchkey->input_dev->name = TM2_TOUCHKEY_DEV_NAME;
	touchkey->input_dev->id.bustype = BUS_I2C;

	set_bit(EV_KEY, touchkey->input_dev->evbit);
	input_set_capability(touchkey->input_dev, EV_KEY, KEY_PHONE);
	input_set_capability(touchkey->input_dev, EV_KEY, KEY_BACK);

	input_set_drvdata(touchkey->input_dev, touchkey);

	ret = input_register_device(touchkey->input_dev);
	if (ret) {
		dev_err(&client->dev, "Failed to register input device\n");
		return ret;
	}

	/* irq */
	ret = devm_request_threaded_irq(&client->dev,
					client->irq, NULL,
					tm2_touchkey_irq_handler,
					IRQF_ONESHOT, TM2_TOUCHKEY_DEV_NAME,
					touchkey);
	if (ret) {
		dev_err(&client->dev, "Failed to request threaded irq\n");
		return ret;
	}

	/* led device */
	touchkey->led_dev.name = TM2_TOUCHKEY_DEV_NAME;
	touchkey->led_dev.brightness = LED_FULL;
	touchkey->led_dev.max_brightness = LED_FULL;
	touchkey->led_dev.brightness_set = tm2_touchkey_led_brightness_set;

	ret = devm_led_classdev_register(&client->dev, &touchkey->led_dev);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register touchkey led\n");
		return ret;
	}

	return 0;
}

static int __maybe_unused tm2_touchkey_suspend(struct device *dev)
{
	struct tm2_touchkey_data *touchkey = dev_get_drvdata(dev);

	disable_irq(touchkey->client->irq);
	tm2_touchkey_power_disable(touchkey);

	return 0;
}

static int __maybe_unused tm2_touchkey_resume(struct device *dev)
{
	struct tm2_touchkey_data *touchkey = dev_get_drvdata(dev);
	int ret;

	enable_irq(touchkey->client->irq);
	ret = tm2_touchkey_power_enable(touchkey);
	if (ret)
		dev_err(dev, "Failed to enable power\n");

	return ret;
}

static SIMPLE_DEV_PM_OPS(tm2_touchkey_pm_ops, tm2_touchkey_suspend,
							tm2_touchkey_resume);

static const struct i2c_device_id tm2_touchkey_id_table[] = {
	{TM2_TOUCHKEY_DEV_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, tm2_touchkey_id_table);

static const struct of_device_id tm2_touchkey_of_match[] = {
	{.compatible = "samsung,tm2-touchkey",},
	{},
};
MODULE_DEVICE_TABLE(of, tm2_touchkey_of_match);

static struct i2c_driver tm2_touchkey_driver = {
	.driver = {
		.name = TM2_TOUCHKEY_DEV_NAME,
		.pm = &tm2_touchkey_pm_ops,
		.of_match_table = of_match_ptr(tm2_touchkey_of_match),
	},
	.probe = tm2_touchkey_probe,
	.id_table = tm2_touchkey_id_table,
};

module_i2c_driver(tm2_touchkey_driver);

MODULE_AUTHOR("Beomho Seo <beomho.seo@samsung.com>");
MODULE_AUTHOR("Jaechul Lee <jcsing.lee@samsung.com>");
MODULE_DESCRIPTION("Samsung touchkey driver");
MODULE_LICENSE("GPL v2");
