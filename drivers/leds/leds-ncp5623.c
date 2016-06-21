/*
 * Copyright 2016 Florian Vaussard <florian.vaussard@heig-vd.ch>
 *
 * Based on leds-tlc591xx.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define NCP5623_MAX_LEDS	3
#define NCP5623_MAX_STEPS	32
#define NCP5623_MAX_CURRENT	31
#define NCP5623_MAX_CURRENT_UA	30000

#define NCP5623_CMD_SHIFT	5
#define CMD_SHUTDOWN		(0x00 << NCP5623_CMD_SHIFT)
#define CMD_ILED		(0x01 << NCP5623_CMD_SHIFT)
#define CMD_PWM1		(0x02 << NCP5623_CMD_SHIFT)
#define CMD_PWM2		(0x03 << NCP5623_CMD_SHIFT)
#define CMD_PWM3		(0x04 << NCP5623_CMD_SHIFT)
#define CMD_UPWARD_DIM		(0x05 << NCP5623_CMD_SHIFT)
#define CMD_DOWNWARD_DIM	(0x06 << NCP5623_CMD_SHIFT)
#define CMD_DIM_STEP		(0x07 << NCP5623_CMD_SHIFT)

#define NCP5623_DATA_MASK	GENMASK(NCP5623_CMD_SHIFT - 1, 0)

#define NCP5623_CMD(cmd, data)	(cmd | (data & NCP5623_DATA_MASK))

struct ncp5623_led {
	bool active;
	unsigned int led_no;
	struct led_classdev ldev;
	struct work_struct work;
	struct ncp5623_priv *priv;
};

struct ncp5623_priv {
	struct ncp5623_led leds[NCP5623_MAX_LEDS];
	u32 led_iref;
	u32 led_max_current;
	struct i2c_client *client;
};

static struct ncp5623_led *ldev_to_led(struct led_classdev *ldev)
{
	return container_of(ldev, struct ncp5623_led, ldev);
}

static struct ncp5623_led *work_to_led(struct work_struct *work)
{
	return container_of(work, struct ncp5623_led, work);
}

static int ncp5623_send_cmd(struct ncp5623_priv *priv, u8 cmd, u8 data)
{
	char cmd_data[1] = { NCP5623_CMD(cmd, data) };
	int err;

	err = i2c_master_send(priv->client, cmd_data, ARRAY_SIZE(cmd_data));
	return (err < 0 ? err : 0);
}

static int ncp5623_set_pwm(struct ncp5623_led *led, u8 brightness)
{
	struct ncp5623_priv *priv = led->priv;
	u8 cmd;

	switch (led->led_no) {
	case 0:
		cmd = CMD_PWM1;
		break;
	case 1:
		cmd = CMD_PWM2;
		break;
	case 2:
		cmd = CMD_PWM3;
		break;
	default:
		return -EINVAL;
	}

	return ncp5623_send_cmd(priv, cmd, brightness);
}

static void ncp5623_led_work(struct work_struct *work)
{
	struct ncp5623_led *led = work_to_led(work);
	enum led_brightness brightness = led->ldev.brightness;
	int err;

	err = ncp5623_set_pwm(led, brightness);

	if (err < 0)
		dev_err(led->ldev.dev, "failed setting brightness\n");
}

static void ncp5623_brightness_set(struct led_classdev *led_cdev,
				   enum led_brightness brightness)
{
	struct ncp5623_led *led = ldev_to_led(led_cdev);

	led->ldev.brightness = brightness;
	schedule_work(&led->work);
}

static void ncp5623_destroy_devices(struct ncp5623_priv *priv)
{
	struct ncp5623_led *led;
	int i;

	for (i = 0; i < NCP5623_MAX_LEDS; i++) {
		led = &priv->leds[i];
		if (led->active) {
			led_classdev_unregister(&led->ldev);
			cancel_work_sync(&led->work);
		}
	}
}

static int ncp5623_configure(struct device *dev,
			     struct ncp5623_priv *priv)
{
	unsigned int i;
	unsigned int n;
	struct ncp5623_led *led;
	int err;

	/* Compute the value of ILED register to honor led_max_current */
	n = 2400 * priv->led_iref / priv->led_max_current + 1;
	if (n > NCP5623_MAX_CURRENT)
		n = NCP5623_MAX_CURRENT;
	n = NCP5623_MAX_CURRENT - n;

	dev_dbg(dev, "setting maximum current to %u uA\n",
		2400 * priv->led_iref / (NCP5623_MAX_CURRENT - n));

	err = ncp5623_send_cmd(priv, CMD_ILED, n);
	if (err < 0) {
		dev_err(dev, "cannot set the current\n");
		return err;
	}

	/* Setup each individual LED */
	for (i = 0; i < NCP5623_MAX_LEDS; i++) {
		led = &priv->leds[i];

		if (!led->active)
			continue;

		led->priv = priv;
		led->led_no = i;
		led->ldev.brightness_set = ncp5623_brightness_set;
		led->ldev.max_brightness = NCP5623_MAX_STEPS - 1;
		INIT_WORK(&led->work, ncp5623_led_work);
		err = led_classdev_register(dev, &led->ldev);
		if (err < 0) {
			dev_err(dev, "couldn't register LED %s\n",
				led->ldev.name);
			goto exit;
		}
	}

	return 0;

exit:
	ncp5623_destroy_devices(priv);
	return err;
}

static const struct of_device_id ncp5623_of_match[] = {
	{ .compatible = "onnn,ncp5623" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ncp5623_of_match);

static int ncp5623_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node, *child;
	struct ncp5623_priv *priv;
	struct ncp5623_led *led;
	u32 reg;
	int err, count;

	count = of_get_child_count(np);
	if (!count || count > NCP5623_MAX_LEDS)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	i2c_set_clientdata(client, priv);

	err = of_property_read_u32(np, "onnn,led-iref-microamp",
				   &priv->led_iref);
	if (err)
		return -EINVAL;
	err = of_property_read_u32(np, "led-max-microamp",
				   &priv->led_max_current);
	if (err || priv->led_max_current > NCP5623_MAX_CURRENT_UA)
		return -EINVAL;

	for_each_child_of_node(np, child) {
		err = of_property_read_u32(child, "reg", &reg);
		if (err)
			return err;
		if (reg < 0 || reg >= NCP5623_MAX_LEDS)
			return -EINVAL;
		led = &priv->leds[reg];
		if (led->active)
			return -EINVAL;
		led->active = true;
		led->ldev.name =
			of_get_property(child, "label", NULL) ? : child->name;
		led->ldev.default_trigger =
			of_get_property(child, "linux,default-trigger", NULL);
	}
	return ncp5623_configure(dev, priv);
}

static int ncp5623_remove(struct i2c_client *client)
{
	struct ncp5623_priv *priv = i2c_get_clientdata(client);

	ncp5623_destroy_devices(priv);

	return 0;
}

static const struct i2c_device_id ncp5623_id[] = {
	{ "ncp5623" },
	{},
};
MODULE_DEVICE_TABLE(i2c, ncp5623_id);

static struct i2c_driver ncp5623_driver = {
	.driver = {
		.name = "ncp5623",
		.of_match_table = of_match_ptr(ncp5623_of_match),
	},
	.probe = ncp5623_probe,
	.remove = ncp5623_remove,
	.id_table = ncp5623_id,
};

module_i2c_driver(ncp5623_driver);

MODULE_AUTHOR("Florian Vaussard <florian.vaussard@heig-vd.ch>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NCP5623 LED driver");
