// SPDX-License-Identifier: GPL-2.0
// Flash and torch driver for Texas Instruments LM3601X LED
// Flash driver chip family
// Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <uapi/linux/uleds.h>

#define LM3601X_LED_TORCH	0x0
#define LM3601X_LED_IR		0x1

/* Registers */
#define LM3601X_ENABLE_REG	0x01
#define LM3601X_CFG_REG		0x02
#define LM3601X_LED_FLASH_REG	0x03
#define LM3601X_LED_TORCH_REG	0x04
#define LM3601X_FLAGS_REG	0x05
#define LM3601X_DEV_ID_REG	0x06

#define LM3601X_SW_RESET	BIT(7)

/* Enable Mode bits */
#define LM3601X_MODE_STANDBY	0x00
#define LM3601X_MODE_IR_DRV	BIT(0)
#define LM3601X_MODE_TORCH	BIT(1)
#define LM3601X_MODE_STROBE	(BIT(0) | BIT(1))
#define LM3601X_STRB_EN		BIT(2)
#define LM3601X_STRB_LVL_TRIG	~BIT(3)
#define LM3601X_STRB_EDGE_TRIG	BIT(3)
#define LM3601X_IVFM_EN		BIT(4)

#define LM36010_BOOST_LIMIT_19	~BIT(5)
#define LM36010_BOOST_LIMIT_28	BIT(5)
#define LM36010_BOOST_FREQ_2MHZ	~BIT(6)
#define LM36010_BOOST_FREQ_4MHZ	BIT(6)
#define LM36010_BOOST_MODE_NORM	~BIT(7)
#define LM36010_BOOST_MODE_PASS	BIT(7)

/* Flag Mask */
#define LM3601X_FLASH_TIME_OUT	BIT(0)
#define LM3601X_UVLO_FAULT	BIT(1)
#define LM3601X_THERM_SHUTDOWN	BIT(2)
#define LM3601X_THERM_CURR	BIT(3)
#define LM36010_CURR_LIMIT	BIT(4)
#define LM3601X_SHORT_FAULT	BIT(5)
#define LM3601X_IVFM_TRIP	BIT(6)
#define LM36010_OVP_FAULT	BIT(7)

#define LM3601X_MIN_TORCH_I_UA	2400
#define LM3601X_MIN_STROBE_I_MA	11

#define LM3601X_TIMEOUT_MASK	0x1e
#define LM3601X_ENABLE_MASK	0x03

enum lm3601x_type {
	CHIP_LM36010 = 0,
	CHIP_LM36011,
};

/**
 * struct lm3601x_max_timeouts -
 * @timeout: timeout value in ms
 * @regval: the value of the register to write
 */
struct lm3601x_max_timeouts {
	int timeout;
	int reg_val;
};

/**
 * struct lm3601x_led -
 * @lock: Lock for reading/writing the device
 * @regmap: Devices register map
 * @client: Pointer to the I2C client
 * @led_node: DT device node for the led
 * @cdev_torch: led class device pointer for the torch
 * @cdev_ir: led class device pointer for infrared
 * @fled_cdev: flash led class device pointer
 * @led_name: LED label for the Torch or IR LED
 * @strobe: LED label for the strobe
 * @last_flag: last known flags register value
 * @strobe_timeout: the timeout for the strobe
 * @torch_current_max: maximum current for the torch
 * @strobe_current_max: maximum current for the strobe
 * @max_strobe_timeout: maximum timeout for the strobe
 * @led_mode: The mode to enable either IR or Torch
 */
struct lm3601x_led {
	struct mutex lock;
	struct regmap *regmap;
	struct i2c_client *client;

	struct device_node *led_node;

	struct led_classdev cdev_torch;
	struct led_classdev cdev_ir;

	struct led_classdev_flash fled_cdev;

	char led_name[LED_MAX_NAME_SIZE];
	char strobe[LED_MAX_NAME_SIZE];

	unsigned int last_flag;
	unsigned int strobe_timeout;

	u32 torch_current_max;
	u32 strobe_current_max;
	u32 max_strobe_timeout;

	int led_mode;
};

static const struct lm3601x_max_timeouts strobe_timeouts[] = {
	{ 40000, 0x00 },
	{ 80000, 0x01 },
	{ 120000, 0x02 },
	{ 160000, 0x03 },
	{ 200000, 0x04 },
	{ 240000, 0x05 },
	{ 280000, 0x06 },
	{ 320000, 0x07 },
	{ 360000, 0x08 },
	{ 400000, 0x09 },
	{ 600000, 0x0a },
	{ 800000, 0x0b },
	{ 1000000, 0x0c },
	{ 1200000, 0x0d },
	{ 1400000, 0x0e },
	{ 1600000, 0x0f },
};

static const struct reg_default lm3601x_regmap_defs[] = {
	{ LM3601X_ENABLE_REG, 0x20 },
	{ LM3601X_CFG_REG, 0x15 },
	{ LM3601X_LED_FLASH_REG, 0x00 },
	{ LM3601X_LED_TORCH_REG, 0x00 },
};

static bool lm3601x_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LM3601X_FLAGS_REG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config lm3601x_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = LM3601X_DEV_ID_REG,
	.reg_defaults = lm3601x_regmap_defs,
	.num_reg_defaults = ARRAY_SIZE(lm3601x_regmap_defs),
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = lm3601x_volatile_reg,
};

static struct lm3601x_led *fled_cdev_to_led(
				struct led_classdev_flash *fled_cdev)
{
	return container_of(fled_cdev, struct lm3601x_led, fled_cdev);
}

static int lm3601x_read_faults(struct lm3601x_led *led)
{
	int flags_val;
	int ret;

	ret = regmap_read(led->regmap, LM3601X_FLAGS_REG, &flags_val);
	if (ret < 0)
		return -EIO;

	led->last_flag = 0;

	if (flags_val & LM36010_OVP_FAULT)
		led->last_flag |= LED_FAULT_OVER_VOLTAGE;

	if (flags_val & (LM3601X_THERM_SHUTDOWN | LM3601X_THERM_CURR))
		led->last_flag |= LED_FAULT_OVER_TEMPERATURE;

	if (flags_val & LM3601X_SHORT_FAULT)
		led->last_flag |= LED_FAULT_SHORT_CIRCUIT;

	if (flags_val & LM36010_CURR_LIMIT)
		led->last_flag |= LED_FAULT_OVER_CURRENT;

	if (flags_val & LM3601X_UVLO_FAULT)
		led->last_flag |= LED_FAULT_UNDER_VOLTAGE;

	if (flags_val & LM3601X_IVFM_TRIP)
		led->last_flag |= LED_FAULT_INPUT_VOLTAGE;

	if (flags_val & LM3601X_THERM_SHUTDOWN)
		led->last_flag |= LED_FAULT_LED_OVER_TEMPERATURE;

	return led->last_flag;
}

static int lm3601x_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct lm3601x_led *led =
	    container_of(cdev, struct lm3601x_led, cdev_torch);
	u8 brightness_val;
	int ret, led_mode_val;

	mutex_lock(&led->lock);

	ret = lm3601x_read_faults(led);
	if (ret < 0)
		goto out;

	if (led->led_mode == LM3601X_LED_TORCH)
		led_mode_val = LM3601X_MODE_TORCH;
	else
		led_mode_val = LM3601X_MODE_IR_DRV;

	if (brightness == LED_OFF) {
		ret = regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
					led_mode_val, LED_OFF);
		goto out;
	}

	if (brightness == LED_ON)
		brightness_val = LED_ON;
	else
		brightness_val = (brightness/2);

	ret = regmap_write(led->regmap, LM3601X_LED_TORCH_REG, brightness_val);
	if (ret < 0)
		goto out;

	ret = regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
					led_mode_val,
					led_mode_val);

out:
	mutex_unlock(&led->lock);
	return ret;
}

static int lm3601x_strobe_set(struct led_classdev_flash *fled_cdev,
				bool state)
{
	struct lm3601x_led *led = fled_cdev_to_led(fled_cdev);
	int ret;
	int current_timeout;
	int reg_count;
	int i;
	int timeout_reg_val = 0;

	mutex_lock(&led->lock);

	ret = regmap_read(led->regmap, LM3601X_CFG_REG, &current_timeout);
	if (ret < 0)
		goto out;

	reg_count = ARRAY_SIZE(strobe_timeouts);
	for (i = 0; i < reg_count; i++) {
		if (led->strobe_timeout > strobe_timeouts[i].timeout)
			continue;

		if (led->strobe_timeout <= strobe_timeouts[i].timeout) {
			timeout_reg_val = (strobe_timeouts[i].reg_val << 1);
			break;
		}

		ret = -EINVAL;
		goto out;
	}

	if (led->strobe_timeout != current_timeout)
		ret = regmap_update_bits(led->regmap, LM3601X_CFG_REG,
					LM3601X_TIMEOUT_MASK, timeout_reg_val);

	if (state)
		ret = regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
					LM3601X_MODE_STROBE,
					LM3601X_MODE_STROBE);
	else
		ret = regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
					LM3601X_MODE_STROBE, LED_OFF);

	ret = lm3601x_read_faults(led);
out:
	mutex_unlock(&led->lock);
	return ret;
}

static int lm3601x_strobe_brightness_set(struct led_classdev *cdev,
					 enum led_brightness brightness)
{
	struct led_classdev_flash *fled_cdev = lcdev_to_flcdev(cdev);
	struct lm3601x_led *led = fled_cdev_to_led(fled_cdev);
	int ret;
	u8 brightness_val;

	mutex_lock(&led->lock);
	ret = lm3601x_read_faults(led);
	if (ret < 0)
		goto out;

	if (brightness == LED_OFF) {
		ret = regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
					LM3601X_MODE_STROBE, LED_OFF);
		goto out;
	}

	if (brightness == LED_ON)
		brightness_val = LED_ON;
	else
		brightness_val = (brightness/2);

	ret = regmap_write(led->regmap, LM3601X_LED_FLASH_REG, brightness_val);

out:
	mutex_unlock(&led->lock);
	return ret;
}

static int lm3601x_strobe_timeout_set(struct led_classdev_flash *fled_cdev,
				u32 timeout)
{
	struct lm3601x_led *led = fled_cdev_to_led(fled_cdev);
	int ret = 0;

	mutex_lock(&led->lock);

	led->strobe_timeout = timeout;

	mutex_unlock(&led->lock);

	return ret;
}

static int lm3601x_strobe_get(struct led_classdev_flash *fled_cdev,
				bool *state)
{
	struct lm3601x_led *led = fled_cdev_to_led(fled_cdev);
	int ret;
	int strobe_state;

	mutex_lock(&led->lock);

	ret = regmap_read(led->regmap, LM3601X_ENABLE_REG, &strobe_state);
	if (ret < 0)
		goto out;

	*state = strobe_state & LM3601X_MODE_STROBE;

out:
	mutex_unlock(&led->lock);
	return ret;
}

static int lm3601x_strobe_fault_get(struct led_classdev_flash *fled_cdev,
				u32 *fault)
{
	struct lm3601x_led *led = fled_cdev_to_led(fled_cdev);

	lm3601x_read_faults(led);

	*fault = led->last_flag;

	return 0;
}

static const struct led_flash_ops strobe_ops = {
	.strobe_set		= lm3601x_strobe_set,
	.strobe_get		= lm3601x_strobe_get,
	.timeout_set		= lm3601x_strobe_timeout_set,
	.fault_get		= lm3601x_strobe_fault_get,
};

static int lm3601x_register_leds(struct lm3601x_led *led)
{
	struct led_classdev_flash *fled_cdev;
	struct led_classdev *led_cdev;
	int err = -ENODEV;

	led->cdev_torch.name = led->led_name;
	led->cdev_torch.max_brightness = LED_FULL;
	led->cdev_torch.brightness_set_blocking = lm3601x_brightness_set;
	err = devm_led_classdev_register(&led->client->dev,
			&led->cdev_torch);
	if (err < 0)
		return err;

	fled_cdev = &led->fled_cdev;
	fled_cdev->ops = &strobe_ops;

	led_cdev = &fled_cdev->led_cdev;
	led_cdev->name = led->strobe;
	led_cdev->max_brightness = LED_FULL;
	led_cdev->brightness_set_blocking = lm3601x_strobe_brightness_set;
	led_cdev->flags |= LED_DEV_CAP_FLASH;

	err = led_classdev_flash_register(&led->client->dev,
			fled_cdev);

	return err;
}

static void lm3601x_init_flash_timeout(struct lm3601x_led *led)
{
	struct led_flash_setting *setting;

	setting = &led->fled_cdev.timeout;
	setting->min = strobe_timeouts[0].timeout;
	setting->max = led->max_strobe_timeout;
	setting->step = 40;
	setting->val = led->max_strobe_timeout;
}

static int lm3601x_parse_node(struct lm3601x_led *led,
			      struct device_node *node)
{
	struct device_node *child_node;
	const char *name;
	char *mode_name;
	int ret = -ENODEV;

	for_each_available_child_of_node(node, child_node) {
		led->led_node = of_node_get(child_node);
		if (!led->led_node) {
			dev_err(&led->client->dev,
				"No LED Child node\n");

			goto out_err;
		}

		ret = of_property_read_u32(led->led_node, "led-sources",
					   &led->led_mode);
		if (ret) {
			dev_err(&led->client->dev,
				"led-sources DT property missing\n");
			goto out_err;
		}

		if (led->led_mode < LM3601X_LED_TORCH ||
		    led->led_mode > LM3601X_LED_IR) {
			dev_warn(&led->client->dev,
				"Invalid led mode requested\n");

			goto out_err;

		}
	}

	if (led->led_mode == LM3601X_LED_TORCH) {
		ret = of_property_read_string(led->led_node, "label", &name);
		if (!ret)
			snprintf(led->led_name, sizeof(led->led_name),
				"%s:%s", led->led_node->name, name);
		else
			snprintf(led->led_name, sizeof(led->led_name),
				"%s:torch", led->led_node->name);

		ret = of_property_read_u32(led->led_node, "led-max-microamp",
					&led->torch_current_max);
		if (ret < 0) {
			dev_warn(&led->client->dev,
				"led-max-microamp DT property missing\n");

			goto out_err;
		}

		mode_name = "torch";

	} else if (led->led_mode == LM3601X_LED_IR) {
		ret = of_property_read_string(led->led_node, "label", &name);
		if (!ret)
			snprintf(led->led_name, sizeof(led->led_name),
				"%s:%s", led->led_node->name, name);
		else
			snprintf(led->led_name, sizeof(led->led_name),
				"%s::infrared", led->led_node->name);

		mode_name = "ir";

	} else {
		dev_warn(&led->client->dev,
			"No LED mode is selected exiting probe\n");

		goto out_err;
	}

	/* Flash mode is available in IR or Torch mode so read the DT */
	snprintf(led->strobe, sizeof(led->strobe),
			"%s:%s:strobe", led->led_node->name, mode_name);

	ret = of_property_read_u32(led->led_node,
				"flash-max-microamp",
				&led->strobe_current_max);
	if (ret < 0) {
		dev_warn(&led->client->dev,
			 "flash-max-microamp DT property missing\n");
		goto out_err;
	}

	ret = of_property_read_u32(led->led_node,
				"flash-max-timeout-us",
				&led->max_strobe_timeout);
	if (ret < 0) {
		dev_warn(&led->client->dev,
			 "flash-max-timeout-us DT property missing\n");

		goto out_err;
	}

	lm3601x_init_flash_timeout(led);

out_err:
	of_node_put(led->led_node);
	return ret;
}

static int lm3601x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct lm3601x_led *led;
	int err;

	led = devm_kzalloc(&client->dev,
			    sizeof(struct lm3601x_led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	err = lm3601x_parse_node(led, client->dev.of_node);
	if (err < 0)
		return -ENODEV;

	led->client = client;
	led->regmap = devm_regmap_init_i2c(client, &lm3601x_regmap);
	if (IS_ERR(led->regmap)) {
		err = PTR_ERR(led->regmap);
		dev_err(&client->dev,
			"Failed to allocate register map: %d\n", err);
		return err;
	}

	mutex_init(&led->lock);
	i2c_set_clientdata(client, led);
	err = lm3601x_register_leds(led);

	return err;
}

static int lm3601x_remove(struct i2c_client *client)
{
	struct lm3601x_led *led = i2c_get_clientdata(client);

	regmap_update_bits(led->regmap, LM3601X_ENABLE_REG,
			   LM3601X_ENABLE_MASK,
			   LM3601X_MODE_STANDBY);

	return 0;
}

static const struct i2c_device_id lm3601x_id[] = {
	{ "LM36010", CHIP_LM36010 },
	{ "LM36011", CHIP_LM36011 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, lm3601x_id);

static const struct of_device_id of_lm3601x_leds_match[] = {
	{ .compatible = "ti,lm36010", },
	{ .compatible = "ti,lm36011", },
	{},
};
MODULE_DEVICE_TABLE(of, of_lm3601x_leds_match);

static struct i2c_driver lm3601x_i2c_driver = {
	.driver = {
		.name = "lm3601x",
		.of_match_table = of_lm3601x_leds_match,
	},
	.probe = lm3601x_probe,
	.remove = lm3601x_remove,
	.id_table = lm3601x_id,
};
module_i2c_driver(lm3601x_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments Flash Lighting driver for LM3601X");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com>");
MODULE_LICENSE("GPL v2");
