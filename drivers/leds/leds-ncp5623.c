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

#define NCP5623_MAX_LEDS	3
#define NCP5623_MAX_STEPS	31
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

#define LED_TO_PWM_CMD(led)	((0x02 + led) << NCP5623_CMD_SHIFT)

#define NCP5623_DATA_MASK	GENMASK(NCP5623_CMD_SHIFT - 1, 0)
#define NCP5623_CMD(cmd, data)	(cmd | (data & NCP5623_DATA_MASK))

struct ncp5623_led {
	int led_no;
	u32 led_max_current;
	struct led_classdev ldev;
	struct ncp5623_priv *priv;
};

struct ncp5623_priv {
	struct ncp5623_led leds[NCP5623_MAX_LEDS];
	u32 led_iref;
	u32 leds_max_current;
	struct i2c_client *client;
};

static struct ncp5623_led *ldev_to_led(struct led_classdev *ldev)
{
	return container_of(ldev, struct ncp5623_led, ldev);
}

static int ncp5623_send_cmd(struct ncp5623_priv *priv, u8 cmd, u8 data)
{
	char cmd_data[1] = { NCP5623_CMD(cmd, data) };
	int err;

	err = i2c_master_send(priv->client, cmd_data, ARRAY_SIZE(cmd_data));

	return (err < 0 ? err : 0);
}

static int ncp5623_brightness_set(struct led_classdev *led_cdev,
				  enum led_brightness brightness)
{
	struct ncp5623_led *led = ldev_to_led(led_cdev);

	return ncp5623_send_cmd(led->priv, LED_TO_PWM_CMD(led->led_no),
				brightness);
}

static int ncp5623_configure(struct device *dev,
			     struct ncp5623_priv *priv)
{
	unsigned int i;
	unsigned int n;
	struct ncp5623_led *led;
	int effective_current;
	int err;

	/* Setup the internal current source, round down */
	n = 2400 * priv->led_iref / priv->leds_max_current + 1;
	if (n > NCP5623_MAX_CURRENT)
		n = NCP5623_MAX_CURRENT;

	effective_current = 2400 * priv->led_iref / n;
	dev_dbg(dev, "setting maximum current to %u uA\n", effective_current);

	err = ncp5623_send_cmd(priv, CMD_ILED, NCP5623_MAX_CURRENT - n);
	if (err < 0) {
		dev_err(dev, "cannot set the current\n");
		return err;
	}

	/* Setup each individual LED */
	for (i = 0; i < NCP5623_MAX_LEDS; i++) {
		led = &priv->leds[i];

		if (led->led_no < 0)
			continue;

		led->priv = priv;
		led->ldev.brightness_set_blocking = ncp5623_brightness_set;

		led->ldev.max_brightness = led->led_max_current *
			NCP5623_MAX_STEPS / effective_current;
		if (led->ldev.max_brightness > NCP5623_MAX_STEPS)
			led->ldev.max_brightness = NCP5623_MAX_STEPS;

		err = devm_led_classdev_register(dev, &led->ldev);
		if (err < 0) {
			dev_err(dev, "couldn't register LED %s\n",
				led->ldev.name);
			return err;
		}
	}

	return 0;
}

static int ncp5623_parse_dt(struct ncp5623_priv *priv, struct device_node *np)
{
	struct device_node *child;
	struct ncp5623_led *led;
	u32 reg;
	int count;
	int err;

	err = of_property_read_u32(np, "onnn,led-iref-microamp",
				   &priv->led_iref);
	if (err)
		return -EINVAL;

	count = of_get_child_count(np);
	if (!count || count > NCP5623_MAX_LEDS)
		return -EINVAL;

	for_each_child_of_node(np, child) {
		err = of_property_read_u32(child, "reg", &reg);
		if (err)
			return err;

		if (reg >= NCP5623_MAX_LEDS) {
			err = -EINVAL;
			goto dt_child_parse_error;
		}

		led = &priv->leds[reg];
		if (led->led_no >= 0) {
			/* Already registered */
			err = -EINVAL;
			goto dt_child_parse_error;
		}
		led->led_no = reg;

		err = of_property_read_u32(child, "led-max-microamp",
					   &led->led_max_current);
		if (err || led->led_max_current > NCP5623_MAX_CURRENT_UA)
			return -EINVAL;
		if (led->led_max_current > priv->leds_max_current)
			priv->leds_max_current = led->led_max_current;

		led->ldev.name =
			of_get_property(child, "label", NULL) ? : child->name;
		led->ldev.default_trigger =
			of_get_property(child, "linux,default-trigger", NULL);
	}

	return 0;

dt_child_parse_error:
	of_node_put(child);

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
	struct device_node *np = dev->of_node;
	struct ncp5623_priv *priv;
	int i, err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Mark all LEDs as inactive by default */
	for (i = 0; i < NCP5623_MAX_LEDS; i++)
		priv->leds[i].led_no = -ENODEV;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	err = ncp5623_parse_dt(priv, np);
	if (err)
		return err;

	return ncp5623_configure(dev, priv);
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
	.id_table = ncp5623_id,
};

module_i2c_driver(ncp5623_driver);

MODULE_AUTHOR("Florian Vaussard <florian.vaussard@heig-vd.ch>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("NCP5623 LED driver");
