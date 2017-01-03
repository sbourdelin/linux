/*
 * Driver for keys on GPIO lines capable of generating interrupts.
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
#include <linux/workqueue.h>

#define TM2_TOUCHKEY_DEV_NAME			"tm2-touchkey"
#define TM2_TOUCHKEY_KEYCODE_REG			0x03
#define TM2_TOUCHKEY_BASE_REG			0x00
#define TM2_TOUCHKEY_CMD_LED_ON			0x10
#define TM2_TOUCHKEY_CMD_LED_OFF			0x20
#define TM2_TOUCHKEY_BIT_PRESS_EV			BIT(3)
#define TM2_TOUCHKEY_BIT_KEYCODE			GENMASK(2, 0)
#define TM2_TOUCHKEY_LED_VOLTAGE_MIN			2500000
#define TM2_TOUCHKEY_LED_VOLTAGE_MAX			3300000

enum {
	TM2_TOUCHKEY_KEY_MENU = 0x1,
	TM2_TOUCHKEY_KEY_BACK,
};

#define tm2_touchkey_power_enable(x) __tm2_touchkey_power_onoff(x, 1)
#define tm2_touchkey_power_disable(x) __tm2_touchkey_power_onoff(x, 0)

struct tm2_touchkey_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct led_classdev led_dev;

	u8 keycode_type;
	u8 pressed;
	struct work_struct irq_work;

	bool power_onoff;
	struct regulator *regulator_vcc;	/* 1.8V */
	struct regulator *regulator_vdd;	/* 3.3V */
};

static void tm2_touchkey_led_brightness_set(struct led_classdev *led_dev,
						enum led_brightness brightness)
{
	struct tm2_touchkey_data *samsung_touchkey =
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

	regulator_set_voltage(samsung_touchkey->regulator_vdd, volt, volt);
	i2c_smbus_write_byte_data(samsung_touchkey->client,
				  TM2_TOUCHKEY_BASE_REG, data);
}

static int __tm2_touchkey_power_onoff(struct tm2_touchkey_data
					  *samsung_touchkey, bool onoff)
{
	int ret = 0;

	if (samsung_touchkey->power_onoff == onoff)
		return ret;

	if (onoff) {
		ret = regulator_enable(samsung_touchkey->regulator_vcc);
		if (ret)
			return ret;

		ret = regulator_enable(samsung_touchkey->regulator_vdd);
		if (ret) {
			regulator_disable(samsung_touchkey->regulator_vcc);
			return ret;
		}
		msleep(150);
	} else {
		int err;

		err = regulator_disable(samsung_touchkey->regulator_vcc);
		if (err)
			ret = err;

		err = regulator_disable(samsung_touchkey->regulator_vdd);
		if (err && !ret)
			ret = err;
	}
	samsung_touchkey->power_onoff = onoff;

	return ret;
}

static void tm2_touchkey_irq_work(struct work_struct *irq_work)
{
	struct tm2_touchkey_data *samsung_touchkey =
	    container_of(irq_work, struct tm2_touchkey_data, irq_work);

	if (!samsung_touchkey->pressed) {
		input_report_key(samsung_touchkey->input_dev, KEY_PHONE, 0);
		input_report_key(samsung_touchkey->input_dev, KEY_BACK, 0);
	} else {
		if (samsung_touchkey->keycode_type == TM2_TOUCHKEY_KEY_MENU)
			input_report_key(samsung_touchkey->input_dev,
					 KEY_PHONE, 1);
		else
			input_report_key(samsung_touchkey->input_dev,
					 KEY_BACK, 1);
	}
	input_sync(samsung_touchkey->input_dev);
}

static irqreturn_t tm2_touchkey_irq_handler(int irq, void *devid)
{
	struct tm2_touchkey_data *samsung_touchkey = devid;
	u32 data;

	data = i2c_smbus_read_byte_data(samsung_touchkey->client,
					TM2_TOUCHKEY_KEYCODE_REG);

	if (data < 0) {
		dev_err(&samsung_touchkey->client->dev, "Failed to read i2c data\n");
		return IRQ_HANDLED;
	}

	samsung_touchkey->keycode_type = data & TM2_TOUCHKEY_BIT_KEYCODE;
	samsung_touchkey->pressed = !(data & TM2_TOUCHKEY_BIT_PRESS_EV);

	if (samsung_touchkey->keycode_type != TM2_TOUCHKEY_KEY_MENU &&
	    samsung_touchkey->keycode_type != TM2_TOUCHKEY_KEY_BACK)
		return IRQ_HANDLED;

	schedule_work(&samsung_touchkey->irq_work);

	return IRQ_HANDLED;
}

static int tm2_touchkey_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct tm2_touchkey_data *samsung_touchkey;
	int ret;

	ret = i2c_check_functionality(client->adapter,
				      I2C_FUNC_SMBUS_BYTE |
				      I2C_FUNC_SMBUS_BYTE_DATA);
	if (!ret) {
		dev_err(&client->dev, "No I2C functionality found\n");
		return -ENODEV;
	}

	samsung_touchkey = devm_kzalloc(&client->dev,
			sizeof(struct tm2_touchkey_data), GFP_KERNEL);

	if (!samsung_touchkey) {
		dev_err(&client->dev, "Failed to allocate memory.\n");
		return -ENOMEM;
	}

	samsung_touchkey->client = client;
	i2c_set_clientdata(client, samsung_touchkey);
	INIT_WORK(&samsung_touchkey->irq_work, tm2_touchkey_irq_work);

	/* regulator */
	samsung_touchkey->regulator_vcc =
				devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(samsung_touchkey->regulator_vcc)) {
		dev_err(&client->dev, "Failed to get vcc regulator\n");
		return PTR_ERR(samsung_touchkey->regulator_vcc);
	}

	samsung_touchkey->regulator_vdd =
				devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(samsung_touchkey->regulator_vdd)) {
		dev_err(&client->dev, "Failed to get vdd regulator\n");
		return PTR_ERR(samsung_touchkey->regulator_vcc);
	}

	/* power */
	ret = tm2_touchkey_power_enable(samsung_touchkey);
	if (ret) {
		dev_err(&client->dev, "Failed to enable power\n");
		return ret;
	}

	/* irq */
	ret = devm_request_threaded_irq(&client->dev,
					client->irq, NULL,
					tm2_touchkey_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					TM2_TOUCHKEY_DEV_NAME,
					samsung_touchkey);
	if (ret) {
		dev_err(&client->dev, "Failed to request threaded irq\n");
		return ret;
	}

	/* input device */
	samsung_touchkey->input_dev = devm_input_allocate_device(&client->dev);
	if (!samsung_touchkey->input_dev) {
		dev_err(&client->dev, "Failed to alloc input device.\n");
		return -ENOMEM;
	}
	samsung_touchkey->input_dev->name = TM2_TOUCHKEY_DEV_NAME;
	samsung_touchkey->input_dev->id.bustype = BUS_I2C;
	samsung_touchkey->input_dev->dev.parent = &client->dev;

	set_bit(EV_KEY, samsung_touchkey->input_dev->evbit);
	set_bit(KEY_PHONE, samsung_touchkey->input_dev->keybit);
	set_bit(KEY_BACK, samsung_touchkey->input_dev->keybit);
	input_set_drvdata(samsung_touchkey->input_dev, samsung_touchkey);

	ret = input_register_device(samsung_touchkey->input_dev);
	if (ret) {
		dev_err(&client->dev, "Failed to register input device.\n");
		return ret;
	}

	/* led device */
	samsung_touchkey->led_dev.name = TM2_TOUCHKEY_DEV_NAME;
	samsung_touchkey->led_dev.brightness = LED_FULL;
	samsung_touchkey->led_dev.max_brightness = LED_FULL;
	samsung_touchkey->led_dev.brightness_set =
						tm2_touchkey_led_brightness_set;

	ret = devm_led_classdev_register(&client->dev,
					 &samsung_touchkey->led_dev);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register touchkey led\n");
		return ret;
	}

	return 0;
}

static void tm2_touchkey_shutdown(struct i2c_client *client)
{
	struct tm2_touchkey_data *samsung_touchkey =
						i2c_get_clientdata(client);
	int ret;

	disable_irq(client->irq);
	ret = tm2_touchkey_power_disable(samsung_touchkey);
	if (ret)
		dev_err(&client->dev, "Failed to disable power\n");
}

static int tm2_touchkey_suspend(struct device *dev)
{
	struct tm2_touchkey_data *samsung_touchkey = dev_get_drvdata(dev);
	int ret;

	disable_irq(samsung_touchkey->client->irq);
	ret = tm2_touchkey_power_disable(samsung_touchkey);
	if (ret)
		dev_err(dev, "Failed to disable power\n");

	return ret;
}

static int tm2_touchkey_resume(struct device *dev)
{
	struct tm2_touchkey_data *samsung_touchkey = dev_get_drvdata(dev);
	int ret;

	enable_irq(samsung_touchkey->client->irq);
	ret = tm2_touchkey_power_enable(samsung_touchkey);
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

static const struct of_device_id tm2_touchkey_of_match[] = {
	{.compatible = "samsung,tm2-touchkey",},
	{},
};

static struct i2c_driver tm2_touchkey_driver = {
	.driver = {
		.name = TM2_TOUCHKEY_DEV_NAME,
		.pm = &tm2_touchkey_pm_ops,
		.of_match_table = of_match_ptr(tm2_touchkey_of_match),
	},
	.probe = tm2_touchkey_probe,
	.shutdown = tm2_touchkey_shutdown,
	.id_table = tm2_touchkey_id_table,
};

module_i2c_driver(tm2_touchkey_driver);

MODULE_AUTHOR("Beomho Seo <beomho.seo@samsung.com>");
MODULE_AUTHOR("Jaechul Lee <jcsing.lee@samsung.com>");
MODULE_DESCRIPTION("Samsung touchkey driver");
MODULE_LICENSE("GPL v2");
