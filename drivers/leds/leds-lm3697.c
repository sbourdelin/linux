// SPDX-License-Identifier: GPL-2.0
// TI LM3697 LED chip family driver
// Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <uapi/linux/uleds.h>

#define LM3697_REV			0x0
#define LM3697_RESET			0x1
#define LM3697_OUTPUT_CONFIG		0x10
#define LM3697_CTRL_A_RAMP		0x11
#define LM3697_CTRL_B_RAMP		0x12
#define LM3697_CTRL_A_B_RT_RAMP		0x13
#define LM3697_CTRL_A_B_RAMP_CFG	0x14
#define LM3697_CTRL_A_B_BRT_CFG		0x16
#define LM3697_CTRL_A_FS_CURR_CFG	0x17
#define LM3697_CTRL_B_FS_CURR_CFG	0x18
#define LM3697_PWM_CFG			0x1c
#define LM3697_CTRL_A_BRT_LSB		0x20
#define LM3697_CTRL_A_BRT_MSB		0x21
#define LM3697_CTRL_B_BRT_LSB		0x22
#define LM3697_CTRL_B_BRT_MSB		0x23
#define LM3697_CTRL_ENABLE		0x24

#define LM3697_SW_RESET		BIT(0)

#define LM3697_CTRL_A_EN	BIT(0)
#define LM3697_CTRL_B_EN	BIT(1)
#define LM3697_CTRL_A_B_EN	(LM3697_CTRL_A_EN | LM3697_CTRL_B_EN)

#define LM3697_MAX_LED_STRINGS	3

#define LM3697_CONTROL_A	0
#define LM3697_CONTROL_B	1

#define LM3697_END_OF_ARRAY	0
#define LM3697_HVLED1	1
#define LM3697_HVLED2	2
#define LM3697_HVLED3	3

#define LM3697_HVLED1_SHIFT	0
#define LM3697_HVLED2_SHIFT	1
#define LM3697_HVLED3_SHIFT	2

/**
 * struct lm3697_led -
 * @hvled_strings - Array of LED strings associated with a control bank
 * @label - LED label
 * @led_dev - LED class device
 * @priv - Pointer to the device struct
 * @control_bank - Control bank the LED is associated to. 0 is control bank A
 *		   1 is control bank B.
 */
struct lm3697_led {
	u32 hvled_strings[LM3697_MAX_LED_STRINGS];
	char label[LED_MAX_NAME_SIZE];
	struct led_classdev led_dev;
	struct lm3697 *priv;
	int control_bank;
};

/**
 * struct lm3697 -
 * @enable_gpio - Hardware enable gpio
 * @regulator - LED supply regulator pointer
 * @client - Pointer to the I2C client
 * @regmap - Devices register map
 * @dev - Pointer to the devices device struct
 * @lock - Lock for reading/writing the device
 * @leds - Array of LED strings.
 */
struct lm3697 {
	struct gpio_desc *enable_gpio;
	struct regulator *regulator;
	struct i2c_client *client;
	struct regmap *regmap;
	struct device *dev;
	struct mutex lock;
	struct lm3697_led leds[];
};

static const struct reg_default lm3697_reg_defs[] = {
	{LM3697_OUTPUT_CONFIG, 0x6},
	{LM3697_CTRL_A_RAMP, 0x0},
	{LM3697_CTRL_B_RAMP, 0x0},
	{LM3697_CTRL_A_B_RT_RAMP, 0x0},
	{LM3697_CTRL_A_B_RAMP_CFG, 0x0},
	{LM3697_CTRL_A_B_BRT_CFG, 0x0},
	{LM3697_CTRL_A_FS_CURR_CFG, 0x13},
	{LM3697_CTRL_B_FS_CURR_CFG, 0x13},
	{LM3697_PWM_CFG, 0xc},
	{LM3697_CTRL_A_BRT_LSB, 0x0},
	{LM3697_CTRL_A_BRT_MSB, 0x0},
	{LM3697_CTRL_B_BRT_LSB, 0x0},
	{LM3697_CTRL_B_BRT_MSB, 0x0},
	{LM3697_CTRL_ENABLE, 0x0},
};

static const struct regmap_config lm3697_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = LM3697_CTRL_ENABLE,
	.reg_defaults = lm3697_reg_defs,
	.num_reg_defaults = ARRAY_SIZE(lm3697_reg_defs),
	.cache_type = REGCACHE_FLAT,
};

static int lm3697_brightness_set(struct led_classdev *led_cdev,
				enum led_brightness brt_val)
{
	struct lm3697_led *led = container_of(led_cdev, struct lm3697_led,
					      led_dev);
	int brt_msb_reg, brt_lsb_reg, ctrl_en_val;
	int led_brightness_lsb = (brt_val >> 5);
	int ret;

	mutex_lock(&led->priv->lock);

	if (led->control_bank == LM3697_CONTROL_A) {
		brt_msb_reg = LM3697_CTRL_A_BRT_MSB;
		brt_lsb_reg = LM3697_CTRL_A_BRT_LSB;
		ctrl_en_val = LM3697_CTRL_A_EN;
	} else {
		brt_msb_reg = LM3697_CTRL_B_BRT_MSB;
		brt_lsb_reg = LM3697_CTRL_B_BRT_LSB;
		ctrl_en_val = LM3697_CTRL_B_EN;
	}

	if (brt_val == LED_OFF)
		ret = regmap_update_bits(led->priv->regmap, LM3697_CTRL_ENABLE,
					 ctrl_en_val, ~ctrl_en_val);
	else
		ret = regmap_update_bits(led->priv->regmap, LM3697_CTRL_ENABLE,
					 ctrl_en_val, ctrl_en_val);

	if (ret) {
		dev_err(&led->priv->client->dev, "Cannot write CTRL enable\n");
		goto out;
	}

	ret = regmap_write(led->priv->regmap, brt_lsb_reg, led_brightness_lsb);
	if (ret) {
		dev_err(&led->priv->client->dev, "Cannot write LSB\n");
		goto out;
	}

	ret = regmap_write(led->priv->regmap, brt_msb_reg, brt_val);
	if (ret) {
		dev_err(&led->priv->client->dev, "Cannot write MSB\n");
		goto out;
	}
out:
	mutex_unlock(&led->priv->lock);
	return ret;
}

static int lm3697_set_control_bank(struct lm3697 *priv)
{
	u8 control_bank_config = 0;
	struct lm3697_led *led;
	int ret, i;

	led = &priv->leds[0];
	if (led->control_bank == LM3697_CONTROL_A)
		led = &priv->leds[1];

	if (led->control_bank == LM3697_CONTROL_B &&
	    led->hvled_strings[0] == LM3697_END_OF_ARRAY)
		goto write_config;

	for (i = 0; i < LM3697_MAX_LED_STRINGS; i++) {
		if (led->hvled_strings[i] == LM3697_END_OF_ARRAY)
			break;

		if (led->hvled_strings[i] == LM3697_HVLED1)
			control_bank_config |= 1 << LM3697_HVLED1_SHIFT;

		if (led->hvled_strings[i] == LM3697_HVLED2)
			control_bank_config |= 1 << LM3697_HVLED2_SHIFT;

		if (led->hvled_strings[i] == LM3697_HVLED3)
			control_bank_config |= 1 << LM3697_HVLED3_SHIFT;
	}

write_config:
	ret = regmap_write(priv->regmap, LM3697_OUTPUT_CONFIG,
			   control_bank_config);
	if (ret)
		dev_err(&priv->client->dev, "Cannot write OUTPUT config\n");

	return ret;
}

static int lm3697_init(struct lm3697 *priv)
{
	int ret;

	if (priv->enable_gpio) {
		gpiod_direction_output(priv->enable_gpio, 1);
	} else {
		ret = regmap_write(priv->regmap, LM3697_RESET, LM3697_SW_RESET);
		if (ret) {
			dev_err(&priv->client->dev, "Cannot reset the device\n");
			goto out;
		}
	}

	ret = regmap_write(priv->regmap, LM3697_CTRL_ENABLE, 0x0);
	if (ret) {
		dev_err(&priv->client->dev, "Cannot write ctrl enable\n");
		goto out;
	}

	ret = lm3697_set_control_bank(priv);
	if (ret)
		dev_err(&priv->client->dev, "Setting the CRTL bank failed\n");

out:
	return ret;
}

static int lm3697_probe_dt(struct lm3697 *priv)
{
	struct fwnode_handle *child = NULL;
	struct lm3697_led *led;
	const char *name;
	int control_bank;
	size_t i = 0;
	int ret;

	priv->enable_gpio = devm_gpiod_get_optional(&priv->client->dev,
						   "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable_gpio)) {
		ret = PTR_ERR(priv->enable_gpio);
		dev_err(&priv->client->dev, "Failed to get enable gpio: %d\n",
			ret);
		return ret;
	}

	priv->regulator = devm_regulator_get(&priv->client->dev, "vled");
	if (IS_ERR(priv->regulator))
		priv->regulator = NULL;

	device_for_each_child_node(priv->dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &control_bank);
		if (ret) {
			dev_err(&priv->client->dev, "reg property missing\n");
			fwnode_handle_put(child);
			goto child_out;
		}

		if (control_bank > LM3697_CONTROL_B) {
			dev_err(&priv->client->dev, "reg property is invalid\n");
			ret = -EINVAL;
			fwnode_handle_put(child);
			goto child_out;
		}

		led = &priv->leds[i];
		led->control_bank = control_bank;
		ret = fwnode_property_read_u32_array(child, "led-sources",
						    led->hvled_strings,
						    LM3697_MAX_LED_STRINGS);
		if (ret) {
			dev_err(&priv->client->dev, "led-sources property missing\n");
			fwnode_handle_put(child);
			goto child_out;
		}

		fwnode_property_read_string(child, "linux,default-trigger",
					    &led->led_dev.default_trigger);

		ret = fwnode_property_read_string(child, "label", &name);
		if (ret)
			snprintf(led->label, sizeof(led->label),
				"%s::", priv->client->name);
		else
			snprintf(led->label, sizeof(led->label),
				 "%s:%s", priv->client->name, name);

		led->priv = priv;
		led->led_dev.name = led->label;
		led->led_dev.brightness_set_blocking = lm3697_brightness_set;

		ret = devm_led_classdev_register(priv->dev, &led->led_dev);
		if (ret) {
			dev_err(&priv->client->dev, "led register err: %d\n",
				ret);
			fwnode_handle_put(child);
			goto child_out;
		}

		i++;
	}

child_out:
	return ret;
}

static int lm3697_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct lm3697 *led;
	int count;
	int ret;

	count = device_get_child_node_count(&client->dev);
	if (!count) {
		dev_err(&client->dev, "LEDs are not defined in device tree!");
		return -ENODEV;
	}

	led = devm_kzalloc(&client->dev, struct_size(led, leds, count),
			   GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	mutex_init(&led->lock);
	i2c_set_clientdata(client, led);

	led->client = client;
	led->dev = &client->dev;
	led->regmap = devm_regmap_init_i2c(client, &lm3697_regmap_config);
	if (IS_ERR(led->regmap)) {
		ret = PTR_ERR(led->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = lm3697_probe_dt(led);
	if (ret)
		return ret;

	ret = lm3697_init(led);

	return ret;
}

static int lm3697_remove(struct i2c_client *client)
{
	struct lm3697 *led = i2c_get_clientdata(client);
	int ret;

	ret = regmap_update_bits(led->regmap, LM3697_CTRL_ENABLE,
				 LM3697_CTRL_A_B_EN, 0);
	if (ret) {
		dev_err(&led->client->dev, "Failed to disable the device\n");
		return ret;
	}

	if (led->enable_gpio)
		gpiod_direction_output(led->enable_gpio, 0);

	if (led->regulator) {
		ret = regulator_disable(led->regulator);
		if (ret)
			dev_err(&led->client->dev,
				"Failed to disable regulator\n");
	}

	mutex_destroy(&led->lock);

	return 0;
}

static const struct i2c_device_id lm3697_id[] = {
	{ "lm3697", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm3697_id);

static const struct of_device_id of_lm3697_leds_match[] = {
	{ .compatible = "ti,lm3697", },
	{},
};
MODULE_DEVICE_TABLE(of, of_lm3697_leds_match);

static struct i2c_driver lm3697_driver = {
	.driver = {
		.name	= "lm3697",
		.of_match_table = of_lm3697_leds_match,
	},
	.probe		= lm3697_probe,
	.remove		= lm3697_remove,
	.id_table	= lm3697_id,
};
module_i2c_driver(lm3697_driver);

MODULE_DESCRIPTION("Texas Instruments LM3697 LED driver");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com>");
MODULE_LICENSE("GPL v2");
