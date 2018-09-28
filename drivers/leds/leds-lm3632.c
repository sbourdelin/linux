// SPDX-License-Identifier: GPL-2.0
// Flash and torch driver for Texas Instruments LM3632 LED
// Flash driver chip family
// Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/

#include <linux/led-class-flash.h>

#include "ti-lmu-led-common.h"

#define LM3632_MODE_BL		0x0
#define LM3632_MODE_TORCH	0x1

/* Registers */
#define LM3632_REV_REG		0x01
#define LM3632_CFG1_REG		0x02
#define LM3632_CFG2_REG		0x03
#define LM3633_BL_BRT_LSB	0x04
#define LM3633_BL_BRT_MSB	0x05
#define LM3632_FLASH_TORCH_BRT	0x06
#define LM3632_FLASH_CFG	0x07
#define LM3632_IO_CTRL		0x09
#define LM3632_ENABLE_REG	0x0a
#define LM3632_FLAGS1_REG	0x0b
#define LM3632_FLAGS2_REG	0x10

/* Enable Mode bits */
#define LM3632_BL_EN		BIT(0)
#define LM3632_FLASH_OUT_EN	BIT(1)
#define LM3632_FLASH_MODE	BIT(2)
#define LM3632_BLED1_2_EN	BIT(3)
#define LM3632_BLED1_EN		BIT(4)
#define LM3632_BLED1_2_MASK	(LM3632_BL_EN | LM3632_BLED1_2_EN | LM3632_BLED1_EN)
#define LM3632_BL_OVP_EN	BIT(6)
#define LM3632_SW_RESET		BIT(7)

/* Flags 1 Mask */
#define LM3632_THERM_SHUTDOWN	BIT(0)
#define LM3632_FLASH_TIME_OUT	BIT(1)
#define LM3632_FLED_SHORT_FAULT	BIT(2)
#define LM3632_VINM_SHORT_FAULT	BIT(4)
#define LM3632_FOUT_SHORT_FAULT	BIT(5)
#define LM3632_FLASH_OVP_FAULT	BIT(6)
#define LM3632_BL_OVP_FAULT	BIT(7)

/* Flags 2 Mask */
#define LM3632_BL_OCP_FAULT	BIT(0)
#define LM3632_FLASH_OCP_FAULT	BIT(1)
#define LM3632_VNEG_SHORT_FAULT	BIT(2)
#define LM3632_VPOS_SHORT_FAULT	BIT(3)
#define LM3632_VNEG_OVP_FAULT	BIT(4)
#define LM3632_LCM_OVP_FAULT	BIT(5)

/* IO CTRL bits */
#define LM3632_VINM_EN		BIT(0)
#define LM3632_VINM_MODE_EN	BIT(1)
#define LM3632_TX_EN		BIT(2)
#define LM3632_HW_STROBE_EN	BIT(4)
#define LM3632_PWM_EN		BIT(6)

#define LM3632_TORCH_BRT_SHIFT	4

#define LM3632_MAX_TORCH_I_UA	375000
#define LM3632_MIN_TORCH_I_UA	25000
#define LM3632_TORCH_STEP_UA	25000

#define LM3632_MAX_STROBE_I_UA	1500000
#define LM3632_MIN_STROBE_I_UA	100000
#define LM3632_STROBE_STEP_UA	100000

#define LM3632_TIMEOUT_MASK	0x1f
#define LM3632_ENABLE_MASK	(LM3632_BL_EN | LM3632_FLASH_OUT_EN)

#define LM3632_TIMEOUT_STEP_US	32000
#define LM3632_MIN_TIMEOUT_US	32000
#define LM3632_MAX_TIMEOUT_US	1024000

#define LM3632_TORCH_BRT_MASK	0xf0
#define LM3632_FLASH_BRT_MASK	0xf

#define LM3632_NUM_OF_BL_STRINGS 2
#define LM3632_BL_ENABLED	1
#define LM3632_BL1_ENABLE_SRC	0
#define LM3632_BL12_ENABLE_SRC	1

/**
 * struct lm3632_led -
 * @fled_cdev: flash LED class device pointer
 * @led_name: LED label
 * @led_dev: LED class device
 * @priv: Pointer to the lm3632 struct
 * @lmu_data: Register and setting values for common code
 * @led_name: LED label for the Torch or IR LED
 * @flash_timeout: the timeout for the flash
 * @last_flag: last known flags register value
 * @torch_current_max: maximum current for the torch
 * @flash_current_max: maximum current for the flash
 * @max_flash_timeout: maximum timeout for the flash
 */
struct lm3632_led {
	struct led_classdev_flash fled_cdev;
	u32 led_strings[LM3632_NUM_OF_BL_STRINGS];
	char led_name[LED_MAX_NAME_SIZE];
	struct led_classdev led_dev;
	struct ti_lmu_bank lmu_data;
	struct lm3632 *priv;

	unsigned int flash_timeout;
	unsigned int last_flag;

	u32 torch_current_max;
	u32 flash_current_max;
	u32 max_flash_timeout;
};

/**
 * struct lm3632 -
 * @client: Pointer to the I2C client
 * @regmap: Devices register map
 * @lock: Lock for reading/writing the device
 * @strobe_enable_gpio: Indicates whether strobe is HW controlled
 * @leds: Array of LED strings
 */
struct lm3632 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct device *dev;
	struct mutex lock;
	int strobe_enable_gpio;

	struct lm3632_led leds[];
};

static const struct reg_default lm3632_regmap_defs[] = {
	{ LM3632_CFG1_REG, 0x30 },
	{ LM3632_CFG2_REG, 0x0d },
	{ LM3632_FLASH_CFG, 0x2f },
	{ LM3632_ENABLE_REG, 0x00 },
};

static bool lm3632_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LM3632_FLAGS1_REG:
	case LM3632_FLAGS2_REG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config lm3632_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = LM3632_FLAGS2_REG,
	.reg_defaults = lm3632_regmap_defs,
	.num_reg_defaults = ARRAY_SIZE(lm3632_regmap_defs),
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = lm3632_volatile_reg,
};

static struct lm3632_led *fled_cdev_to_led(struct led_classdev_flash *fled_cdev)
{
	return container_of(fled_cdev, struct lm3632_led, fled_cdev);
}

static int lm3632_read_faults(struct lm3632_led *led)
{
	struct lm3632 *priv = led->priv;
	int flags_val;
	int ret;

	ret = regmap_read(priv->regmap, LM3632_FLAGS1_REG, &flags_val);
	if (ret < 0)
		return -EIO;

	led->last_flag = 0;

	if (flags_val & LM3632_FLASH_OVP_FAULT)
		led->last_flag |= LED_FAULT_OVER_VOLTAGE;

	if (flags_val & LM3632_THERM_SHUTDOWN)
		led->last_flag |= LED_FAULT_OVER_TEMPERATURE;

	if (flags_val & (LM3632_FLED_SHORT_FAULT | LM3632_VINM_SHORT_FAULT |
	    LM3632_FOUT_SHORT_FAULT))
		led->last_flag |= LED_FAULT_SHORT_CIRCUIT;

	if (flags_val & LM3632_THERM_SHUTDOWN)
		led->last_flag |= LED_FAULT_LED_OVER_TEMPERATURE;

	if (flags_val & LM3632_FLASH_TIME_OUT)
		led->last_flag |= LED_FAULT_TIMEOUT;

	ret = regmap_read(priv->regmap, LM3632_FLAGS2_REG, &flags_val);
	if (ret < 0)
		return -EIO;

	if (flags_val & (LM3632_BL_OCP_FAULT | LM3632_FLASH_OCP_FAULT))
		led->last_flag |= LED_FAULT_OVER_CURRENT;

	if (flags_val & LM3632_FLASH_OCP_FAULT)
		led->last_flag |= LED_FAULT_OVER_CURRENT;

	return led->last_flag;
}

static int lm3632_backlight_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct lm3632_led *led = container_of(cdev, struct lm3632_led, led_dev);
	struct lm3632 *priv = led->priv;
	int ret;

	mutex_lock(&priv->lock);

	ret = ti_lmu_common_set_brightness(&led->lmu_data, brightness);
	if (ret)
		dev_err(&priv->client->dev, "Cannot write brightness\n");

	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3632_torch_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_flash *fled_cdev = lcdev_to_flcdev(cdev);
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);
	struct lm3632 *priv = led->priv;
	int ret, brightness_val;
	unsigned int reg_val;

	mutex_lock(&priv->lock);

	ret = lm3632_read_faults(led);
	if (ret < 0)
		goto out;

	if (brightness == LED_OFF)
		ret = regmap_update_bits(priv->regmap, LM3632_ENABLE_REG,
					 LM3632_FLASH_OUT_EN,
					 ~LM3632_FLASH_OUT_EN);
	else {
		regmap_read(priv->regmap, LM3632_FLASH_TORCH_BRT, &reg_val);
		brightness_val = (brightness - LM3632_TORCH_STEP_UA) / LM3632_TORCH_STEP_UA;
		brightness_val |= (reg_val & LM3632_FLASH_BRT_MASK);

		ret = regmap_write(priv->regmap, LM3632_FLASH_TORCH_BRT,
				   brightness_val << LM3632_TORCH_BRT_SHIFT);
		if (ret) {
			dev_err(&priv->client->dev, "Cannot write brightness\n");
			goto out;
		}

		ret = regmap_update_bits(priv->regmap, LM3632_ENABLE_REG,
					 LM3632_FLASH_MODE | LM3632_FLASH_OUT_EN,
					 LM3632_FLASH_OUT_EN | ~LM3632_FLASH_MODE);
		if (ret)
			dev_err(&priv->client->dev, "Cannot write enable\n");
	}
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3632_strobe_set(struct led_classdev_flash *fled_cdev, bool state)
{
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);
	struct lm3632 *priv = led->priv;
	int timeout_reg_val;
	int current_timeout;
	int ret;

	mutex_lock(&priv->lock);

	ret = regmap_read(priv->regmap, LM3632_FLASH_CFG, &current_timeout);
	if (ret < 0)
		goto out;

	if (led->flash_timeout != current_timeout) {
		timeout_reg_val = (led->flash_timeout - LM3632_TIMEOUT_STEP_US) / LM3632_TIMEOUT_STEP_US;
		ret = regmap_update_bits(priv->regmap, LM3632_FLASH_CFG,
					LM3632_TIMEOUT_MASK, timeout_reg_val);
		if (ret) {
			dev_err(&priv->client->dev, "Cannot write timeout\n");
			goto out;
		}
	}

	if (state && !priv->strobe_enable_gpio) {
		ret = regmap_update_bits(priv->regmap, LM3632_ENABLE_REG,
					 LM3632_FLASH_MODE | LM3632_FLASH_OUT_EN,
					 LM3632_FLASH_OUT_EN | LM3632_FLASH_MODE);
		if (ret) {
			dev_err(&priv->client->dev, "Cannot write flash en\n");
			goto out;
		}
	}

	ret = lm3632_read_faults(led);
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3632_flash_brightness_set(struct led_classdev_flash *fled_cdev,
					u32 brightness)
{
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);
	struct lm3632 *priv = led->priv;
	u32 brightness_val;
	int ret;
	unsigned int reg_val;

	mutex_lock(&priv->lock);
	ret = lm3632_read_faults(led);
	if (ret < 0)
		goto out;

	brightness_val = (brightness - LM3632_STROBE_STEP_UA) / LM3632_STROBE_STEP_UA;
	regmap_read(priv->regmap, LM3632_FLASH_TORCH_BRT, &reg_val);

	brightness_val |= (reg_val & LM3632_TORCH_BRT_MASK);
	ret = regmap_write(priv->regmap, LM3632_FLASH_TORCH_BRT, brightness_val);
	if (ret)
		dev_err(&priv->client->dev, "Cannot write brightness\n");

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3632_flash_timeout_set(struct led_classdev_flash *fled_cdev,
				u32 timeout)
{
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);
	struct lm3632 *priv = led->priv;

	mutex_lock(&priv->lock);

	led->flash_timeout = timeout;

	mutex_unlock(&priv->lock);

	return 0;
}

static int lm3632_strobe_get(struct led_classdev_flash *fled_cdev, bool *state)
{
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);
	struct lm3632 *priv = led->priv;
	int strobe_state;
	int ret;

	mutex_lock(&priv->lock);

	ret = regmap_read(priv->regmap, LM3632_ENABLE_REG, &strobe_state);
	if (ret < 0)
		goto out;

	*state = strobe_state & LM3632_FLASH_OUT_EN;

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int lm3632_flash_fault_get(struct led_classdev_flash *fled_cdev,
				u32 *fault)
{
	struct lm3632_led *led = fled_cdev_to_led(fled_cdev);

	lm3632_read_faults(led);

	*fault = led->last_flag;

	return 0;
}

static const struct led_flash_ops flash_ops = {
	.flash_brightness_set	= lm3632_flash_brightness_set,
	.strobe_set		= lm3632_strobe_set,
	.strobe_get		= lm3632_strobe_get,
	.timeout_set		= lm3632_flash_timeout_set,
	.fault_get		= lm3632_flash_fault_get,
};

static int lm3632_register_strobe_leds(struct lm3632_led *led)
{
	struct led_classdev *led_cdev;
	struct led_flash_setting *setting;

	led->fled_cdev.ops = &flash_ops;

	setting = &led->fled_cdev.timeout;
	setting->min = LM3632_MIN_TIMEOUT_US;
	setting->max = led->max_flash_timeout;
	setting->step = LM3632_TIMEOUT_STEP_US;
	setting->val = led->max_flash_timeout;

	setting = &led->fled_cdev.brightness;
	setting->min = LM3632_MIN_STROBE_I_UA;
	setting->max = led->flash_current_max;
	setting->step = LM3632_STROBE_STEP_UA;
	setting->val = led->flash_current_max;

	led_cdev = &led->fled_cdev.led_cdev;
	led_cdev->name = led->led_name;
	led_cdev->brightness_set_blocking = lm3632_torch_brightness_set;
	led_cdev->max_brightness = led->torch_current_max;

	led_cdev->flags |= LED_DEV_CAP_FLASH;

	return led_classdev_flash_register(&led->priv->client->dev, &led->fled_cdev);
}

static int lm3632_strobe_init(struct lm3632_led *led)
{
	struct lm3632 *priv = led->priv;

	if (priv->strobe_enable_gpio)
		regmap_update_bits(priv->regmap, LM3632_IO_CTRL,
				   LM3632_HW_STROBE_EN, LM3632_HW_STROBE_EN);

	return regmap_update_bits(priv->regmap, LM3632_ENABLE_REG,
				  LM3632_FLASH_OUT_EN, ~LM3632_FLASH_OUT_EN);

}

static int lm3632_backlight_init(struct lm3632_led *led)
{
	struct lm3632 *priv = led->priv;
	u8 bl_enable = 0;
	int ret;

	if (led->led_strings[LM3632_BL12_ENABLE_SRC] == LM3632_BL_ENABLED)
		bl_enable |= LM3632_BLED1_2_EN;
	else if (led->led_strings[LM3632_BL1_ENABLE_SRC] == LM3632_BL_ENABLED)
		bl_enable |= LM3632_BLED1_EN;
	else
		return -EINVAL;

	bl_enable |= LM3632_BL_EN;

	/* Power up default is on so lets set it to off */
	ret = ti_lmu_common_set_brightness(&led->lmu_data, LED_OFF);
	if (ret) {
		dev_err(&priv->client->dev, "Cannot write brightness\n");
		return ret;
	}

	return regmap_update_bits(priv->regmap, LM3632_ENABLE_REG,
				  LM3632_BLED1_2_MASK, bl_enable);
}

static int lm3632_parse_node(struct lm3632 *priv)
{
	struct fwnode_handle *child = NULL;
	struct lm3632_led *led;
	int ret = -ENODEV;
	const char *name;
	u32 led_mode;
	size_t i = 0;

	priv->strobe_enable_gpio = device_property_present(&priv->client->dev,
						   "hw-strobe");

	device_for_each_child_node(priv->dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &led_mode);
		if (ret) {
			dev_err(&priv->client->dev, "reg DT property missing\n");
			goto out_err;
		}

		if (led_mode > LM3632_MODE_TORCH ||
		    led_mode < LM3632_MODE_BL) {
			dev_warn(&priv->client->dev, "Invalid led mode requested\n");
			ret = -EINVAL;
			goto out_err;
		}

		led = &priv->leds[i];
		led->priv = priv;

		ret = fwnode_property_read_string(child, "label", &name);
		if (ret) {
			if (led_mode == LM3632_MODE_TORCH)
				name = "torch";
			else
				name = "backlight";
		}

		snprintf(led->led_name, sizeof(led->led_name),
			"%s:%s", priv->client->name, name);

		if (led_mode == LM3632_MODE_TORCH) {
			ret = fwnode_property_read_u32(child, "led-max-microamp",
							&led->torch_current_max);
			if (ret) {
				dev_warn(&priv->client->dev,
					"led-max-microamp DT property missing\n");
				goto out_err;
			}

			ret = fwnode_property_read_u32(child, "flash-max-microamp",
						&led->flash_current_max);
			if (ret) {
				dev_warn(&priv->client->dev,
					 "flash-max-microamp DT property missing\n");
				goto out_err;
			}

			ret = fwnode_property_read_u32(child, "flash-max-timeout-us",
						&led->max_flash_timeout);
			if (ret) {
				dev_warn(&priv->client->dev,
					 "flash-max-timeout-us DT property missing\n");
				goto out_err;
			}
			ret = lm3632_strobe_init(led);
			if (ret) {
				dev_err(&priv->client->dev, "failed to init strobe\n");
				continue;
			}

			ret = lm3632_register_strobe_leds(led);
			if (ret) {
				dev_warn(&priv->client->dev,
					 "Failed to register flash LEDs\n");
				goto out_err;
			}

		} else	if (led_mode == LM3632_MODE_BL) {

			ret = fwnode_property_read_u32_array(child, "led-sources",
							     led->led_strings,
							     LM3632_NUM_OF_BL_STRINGS);
			if (ret) {
				dev_err(&priv->client->dev, "led-sources property missing\n");
				continue;
			}

			led->led_dev.name = led->led_name;
			led->led_dev.brightness_set_blocking = lm3632_backlight_brightness_set;
			led->lmu_data.regmap = priv->regmap;
			led->lmu_data.max_brightness = MAX_BRIGHTNESS_11BIT;
			led->lmu_data.lsb_brightness_reg = LM3633_BL_BRT_LSB;
			led->lmu_data.msb_brightness_reg = LM3633_BL_BRT_MSB;
			led->lmu_data.enable_reg = LM3632_ENABLE_REG;

			ret = lm3632_backlight_init(led);
			if (ret) {
				dev_err(&priv->client->dev, "failed to init backlight\n");
				continue;
			}

			ret = devm_led_classdev_register(priv->dev, &led->led_dev);
			if (ret) {
				dev_err(&priv->client->dev, "failed to register backlight\n");
				continue;
			}
		} else {
			dev_warn(&priv->client->dev,
					 "Failed to register flash LEDs\n");
			goto out_err;
		}

		i++;
	}
out_err:
	fwnode_handle_put(child);
	return ret;
}

static int lm3632_probe(struct i2c_client *client)
{
	struct lm3632 *led;
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

	led->client = client;
	led->dev = &client->dev;
	i2c_set_clientdata(client, led);

	led->regmap = devm_regmap_init_i2c(client, &lm3632_regmap);
	if (IS_ERR(led->regmap)) {
		ret = PTR_ERR(led->regmap);
		dev_err(&client->dev,
			"Failed to allocate register map: %d\n", ret);
		return ret;
	}

	mutex_init(&led->lock);

	return lm3632_parse_node(led);
}

static int lm3632_remove(struct i2c_client *client)
{
	struct lm3632 *led = i2c_get_clientdata(client);

/*	led_classdev_flash_unregister(&led->fled_cdev);*/
	mutex_destroy(&led->lock);

	return regmap_update_bits(led->regmap, LM3632_ENABLE_REG,
			   LM3632_ENABLE_MASK, 0x00);
}

static const struct i2c_device_id lm3632_id[] = {
	{ "LM3632", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm3632_id);

static const struct of_device_id of_lm3632_leds_match[] = {
	{ .compatible = "ti,lm3632", },
	{ }
};
MODULE_DEVICE_TABLE(of, of_lm3632_leds_match);

static struct i2c_driver lm3632_i2c_driver = {
	.driver = {
		.name = "lm3632",
		.of_match_table = of_lm3632_leds_match,
	},
	.probe_new = lm3632_probe,
	.remove = lm3632_remove,
	.id_table = lm3632_id,
};
module_i2c_driver(lm3632_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments Flash Lighting driver for LM3632");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com>");
MODULE_LICENSE("GPL v2");
