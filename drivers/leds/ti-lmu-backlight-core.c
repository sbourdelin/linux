// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015 Texas Instruments
 * Copyright 2018 Sebastian Reichel
 *
 * TI LMU Backlight driver, based on previous work from
 * Milo Kim <milo.kim@ti.com>
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/leds.h>
#include <linux/mfd/ti-lmu.h>
#include <linux/mfd/ti-lmu-register.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "ti-lmu-backlight-data.h"

enum ti_lmu_bl_ramp_mode {
	BL_RAMP_UP,
	BL_RAMP_DOWN,
};

#define NUM_DUAL_CHANNEL			2
#define LMU_BACKLIGHT_DUAL_CHANNEL_USED		(BIT(0) | BIT(1))
#define LMU_BACKLIGHT_11BIT_LSB_MASK		(BIT(0) | BIT(1) | BIT(2))
#define LMU_BACKLIGHT_11BIT_MSB_SHIFT		3

struct ti_lmu_bank {
	struct device *dev;
	int bank_id;
	const struct ti_lmu_bl_cfg *cfg;
	struct ti_lmu *lmu;
	const char *label;
	int leds;
	int current_brightness;
	u32 default_brightness;
	u32 ramp_up_msec;
	u32 ramp_down_msec;

	struct notifier_block nb;

	struct backlight_device *backlight;
	struct led_classdev *led;
};

static int ti_lmu_bl_enable(struct ti_lmu_bank *lmu_bank, bool enable)
{
	struct regmap *regmap = lmu_bank->lmu->regmap;
	unsigned long enable_time = lmu_bank->cfg->reginfo->enable_usec;
	u8 *reg = lmu_bank->cfg->reginfo->enable;
	u8 mask = BIT(lmu_bank->bank_id);
	u8 val = (enable == true) ? mask : 0;
	int ret;

	if (!reg)
		return -EINVAL;

	ret = regmap_update_bits(regmap, *reg, mask, val);
	if (ret)
		return ret;

	if (enable_time > 0)
		usleep_range(enable_time, enable_time + 100);

	return 0;
}

static int ti_lmu_bl_update_brightness_register(struct ti_lmu_bank *lmu_bank,
						       int brightness)
{
	const struct ti_lmu_bl_cfg *cfg = lmu_bank->cfg;
	const struct ti_lmu_bl_reg *reginfo = cfg->reginfo;
	struct regmap *regmap = lmu_bank->lmu->regmap;
	u8 reg, val;
	int ret;

	/*
	 * Brightness register update
	 *
	 * 11 bit dimming: update LSB bits and write MSB byte.
	 *		   MSB brightness should be shifted.
	 *  8 bit dimming: write MSB byte.
	 */
	if (cfg->max_brightness == MAX_BRIGHTNESS_11BIT) {
		reg = reginfo->brightness_lsb[lmu_bank->bank_id];
		ret = regmap_update_bits(regmap, reg,
					 LMU_BACKLIGHT_11BIT_LSB_MASK,
					 brightness);
		if (ret)
			return ret;

		val = brightness >> LMU_BACKLIGHT_11BIT_MSB_SHIFT;
	} else {
		val = brightness;
	}

	reg = reginfo->brightness_msb[lmu_bank->bank_id];
	return regmap_write(regmap, reg, val);
}

static int ti_lmu_bl_set_brightness(struct ti_lmu_bank *lmu_bank,
				    int brightness)
{
	bool enable = brightness > 0;
	int ret;

	ret = ti_lmu_bl_enable(lmu_bank, enable);
	if (ret)
		return ret;

	lmu_bank->current_brightness = brightness;

	return ti_lmu_bl_update_brightness_register(lmu_bank, brightness);
}

static int ti_lmu_bl_set_led_blocking(struct led_classdev *ledc,
					     enum led_brightness value)
{
	struct ti_lmu_bank *lmu_bank = dev_get_drvdata(ledc->dev->parent);
	int brightness = value;

	return ti_lmu_bl_set_brightness(lmu_bank, brightness);
}

static int ti_lmu_bl_check_channel(struct ti_lmu_bank *lmu_bank)
{
	const struct ti_lmu_bl_cfg *cfg = lmu_bank->cfg;
	const struct ti_lmu_bl_reg *reginfo = cfg->reginfo;

	if (!reginfo->brightness_msb)
		return -EINVAL;

	if (cfg->max_brightness > MAX_BRIGHTNESS_8BIT) {
		if (!reginfo->brightness_lsb)
			return -EINVAL;
	}

	return 0;
}

static int ti_lmu_bl_create_channel(struct ti_lmu_bank *lmu_bank)
{
	struct regmap *regmap = lmu_bank->lmu->regmap;
	const struct lmu_bl_reg_data *regdata = lmu_bank->cfg->reginfo->channel;
	int num_channels = lmu_bank->cfg->num_channels;
	unsigned long led_sources = lmu_bank->leds;
	int i, ret;
	u8 shift;

	/*
	 * How to create backlight output channels:
	 *   Check 'led_sources' bit and update registers.
	 *
	 *   1) Dual channel configuration
	 *     The 1st register data is used for single channel.
	 *     The 2nd register data is used for dual channel.
	 *
	 *   2) Multiple channel configuration
	 *     Each register data is mapped to bank ID.
	 *     Bit shift operation is defined in channel registers.
	 *
	 * Channel register data consists of address, mask, value.
	 */

	if (num_channels == NUM_DUAL_CHANNEL) {
		if (led_sources == LMU_BACKLIGHT_DUAL_CHANNEL_USED)
			regdata++;

		return regmap_update_bits(regmap, regdata->reg, regdata->mask,
					  regdata->val);
	}

	for (i = 0; regdata && i < num_channels; i++) {
		/*
		 * Note that the result of regdata->val is shift bit.
		 * The bank_id should be shifted for the channel configuration.
		 */
		if (test_bit(i, &led_sources)) {
			shift = regdata->val;
			ret = regmap_update_bits(regmap, regdata->reg,
						 regdata->mask,
						 lmu_bank->bank_id << shift);
			if (ret)
				return ret;
		}

		regdata++;
	}

	return 0;
}

static int ti_lmu_bl_update_ctrl_mode(struct ti_lmu_bank *lmu_bank)
{
	struct regmap *regmap = lmu_bank->lmu->regmap;
	const struct lmu_bl_reg_data *regdata =
		lmu_bank->cfg->reginfo->mode + lmu_bank->bank_id;
	u8 val = regdata->val;

	if (!regdata)
		return 0;

	/*
	 * Update PWM configuration register.
	 * If the mode is register based, then clear the bit.
	 */
	val = 0;

	return regmap_update_bits(regmap, regdata->reg, regdata->mask, val);
}

static int ti_lmu_bl_convert_ramp_to_index(struct ti_lmu_bank *lmu_bank,
						  enum ti_lmu_bl_ramp_mode mode)
{
	const int *ramp_table = lmu_bank->cfg->ramp_table;
	const int size = lmu_bank->cfg->size_ramp;
	unsigned int msec;
	int i;

	if (!ramp_table)
		return -EINVAL;

	switch (mode) {
	case BL_RAMP_UP:
		msec = lmu_bank->ramp_up_msec;
		break;
	case BL_RAMP_DOWN:
		msec = lmu_bank->ramp_down_msec;
		break;
	default:
		return -EINVAL;
	}

	if (msec <= ramp_table[0])
		return 0;

	if (msec > ramp_table[size - 1])
		return size - 1;

	for (i = 1; i < size; i++) {
		if (msec == ramp_table[i])
			return i;

		/* Find an approximate index by looking up the table */
		if (msec > ramp_table[i - 1] && msec < ramp_table[i]) {
			if (msec - ramp_table[i - 1] < ramp_table[i] - msec)
				return i - 1;
			else
				return i;
		}
	}

	return -EINVAL;
}


static int ti_lmu_bl_set_ramp(struct ti_lmu_bank *lmu_bank)
{
	struct regmap *regmap = lmu_bank->lmu->regmap;
	const struct ti_lmu_bl_reg *reginfo = lmu_bank->cfg->reginfo;
	int offset = reginfo->ramp_reg_offset;
	int i, ret, index;
	struct lmu_bl_reg_data regdata;

	for (i = BL_RAMP_UP; i <= BL_RAMP_DOWN; i++) {
		index = ti_lmu_bl_convert_ramp_to_index(lmu_bank, i);
		if (index > 0) {
			if (!reginfo->ramp)
				break;

			regdata = reginfo->ramp[i];
			if (lmu_bank->bank_id != 0)
				regdata.val += offset;

			/* regdata.val is shift bit */
			ret = regmap_update_bits(regmap, regdata.reg,
						 regdata.mask,
						 index << regdata.val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int ti_lmu_bl_configure(struct ti_lmu_bank *lmu_bank)
{
	int ret;

	ret = ti_lmu_bl_check_channel(lmu_bank);
	if (ret)
		return ret;

	ret = ti_lmu_bl_create_channel(lmu_bank);
	if (ret)
		return ret;

	ret = ti_lmu_bl_update_ctrl_mode(lmu_bank);
	if (ret)
		return ret;

	return ti_lmu_bl_set_ramp(lmu_bank);
}

static int ti_lmu_bl_register_led(struct ti_lmu_bank *lmu_bank)
{
	int err;

	printk("lmu: register_led\n");

	lmu_bank->led = devm_kzalloc(lmu_bank->dev, sizeof(*lmu_bank->led),
				     GFP_KERNEL);
	if (!lmu_bank->led)
		return -ENOMEM;

	lmu_bank->led->name = lmu_bank->label;
	lmu_bank->led->max_brightness = lmu_bank->cfg->max_brightness;
	lmu_bank->led->brightness_set_blocking =
		ti_lmu_bl_set_led_blocking;

	printk("lmu: register_led\n");
	
	err = devm_led_classdev_register(lmu_bank->dev, lmu_bank->led);
	if (err)
		return err;

	return 0;
}

static int ti_lmu_bl_add_device(struct ti_lmu_bank *lmu_bank)
{
	return ti_lmu_bl_register_led(lmu_bank);
}

static int setup_of_node(struct platform_device *pdev)
{
	struct device_node *parent_node = pdev->dev.parent->of_node;
	char *name;

	if (!parent_node)
		return 0;

	name = kasprintf(GFP_KERNEL, "bank%d", pdev->id);
//		name = kasprintf(GFP_KERNEL, "lcd_backlight", pdev->id);
	if (!name) {
		printk("No memory?!\n");
		return -ENOMEM;
	}

	printk("Searching for device in parent: %pOFn", parent_node);

	pdev->dev.of_node = of_get_child_by_name(parent_node, name);
	kfree(name);

	if (!pdev->dev.of_node) {
		printk("No such child: %s\n", name);		
		return -ENODEV;
	}

	return 0;
}

static int ti_lmu_parse_led_sources(struct device *dev)
{
	unsigned long mask = 0;
	int ret;
	int size, i;
	u32 *leds;

	size = device_property_read_u32_array(dev, "ti,led-sources", NULL, 0);
	if (size <= 0) {
		dev_err(dev, "Missing or malformed property led-sources: %d\n",
			size);
		return size < 0 ? size : -EINVAL;
	}

	leds = kmalloc_array(size, sizeof(u32), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	ret = device_property_read_u32_array(dev, "ti,led-sources", leds, size);
	if (ret) {
		dev_err(dev, "Failed to read led-sources property: %d\n", ret);
		goto out;
	}

	for (i = 0; i < size; i++)
		set_bit(leds[i], &mask);

	ret = mask;

out:
	kfree(leds);
	return ret;
}

static int ti_lmu_bl_init(struct ti_lmu_bank *lmu_bank)
{
	struct regmap *regmap = lmu_bank->lmu->regmap;
	const struct lmu_bl_reg_data *regdata =
		lmu_bank->cfg->reginfo->init;
	int num_init = lmu_bank->cfg->reginfo->num_init;
	int i, ret;

	if (lmu_bank->lmu->backlight_initialized)
		return 0;
	lmu_bank->lmu->backlight_initialized = true;

	for (i = 0; regdata && i < num_init; i++) {
		ret = regmap_update_bits(regmap, regdata->reg, regdata->mask,
					 regdata->val);
		if (ret)
			return ret;

		regdata++;
	}

	return 0;
}

static int ti_lmu_bl_reload(struct ti_lmu_bank *lmu_bank)
{
	int err;

	ti_lmu_bl_init(lmu_bank);

	err = ti_lmu_bl_configure(lmu_bank);
	if (err)
		return err;

	printk("lmu: set_brightness %d\n", lmu_bank->default_brightness);		
	return ti_lmu_bl_set_brightness(lmu_bank, lmu_bank->current_brightness);
}

static int ti_lmu_bl_monitor_notifier(struct notifier_block *nb,
					     unsigned long action, void *unused)
{
	struct ti_lmu_bank *lmu_bank = container_of(nb, struct ti_lmu_bank, nb);

	if (action == LMU_EVENT_MONITOR_DONE) {
		if (ti_lmu_bl_reload(lmu_bank))
			return NOTIFY_STOP;
	}

	return NOTIFY_OK;
}

static int ti_lmu_bl_probe(struct platform_device *pdev)
{
	struct ti_lmu_bank *lmu_bank;
	int err;

	printk("lmu: bl probe\n");
	err = setup_of_node(pdev);
	if (err)
		return err;

	printk("lmu: bank\n");	
	lmu_bank = devm_kzalloc(&pdev->dev, sizeof(*lmu_bank), GFP_KERNEL);
	if (!lmu_bank)
		return -ENOMEM;
	lmu_bank->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, lmu_bank);

	err = device_property_read_string(&pdev->dev, "label",
					  &lmu_bank->label);
	if (err)
		return err;

	if (!strcmp(lmu_bank->label, "keyboard")) {
		lmu_bank->label = "kbd_backlight";
	} else
		lmu_bank->default_brightness = 255;

	lmu_bank->leds = ti_lmu_parse_led_sources(&pdev->dev);
	if (lmu_bank->leds < 0)
		return lmu_bank->leds;
	else if (lmu_bank->leds == 0)
		return -EINVAL;

	device_property_read_u32(&pdev->dev, "default-brightness-level",
				 &lmu_bank->default_brightness);
	device_property_read_u32(&pdev->dev, "ti,ramp-up-ms",
				 &lmu_bank->ramp_up_msec);
	device_property_read_u32(&pdev->dev, "ti,ramp-down-ms",
				 &lmu_bank->ramp_down_msec);
	
	lmu_bank->lmu = dev_get_drvdata(pdev->dev.parent);
	lmu_bank->cfg = &lmu_bl_cfg[lmu_bank->lmu->id];
	lmu_bank->bank_id = pdev->id;

	ti_lmu_bl_init(lmu_bank);

	err = ti_lmu_bl_configure(lmu_bank);
	if (err)
		return err;

	err = ti_lmu_bl_add_device(lmu_bank);
	if (err)
		return err;

	printk("lmu: brightness\n");		
	err = ti_lmu_bl_set_brightness(lmu_bank,
					      lmu_bank->default_brightness);
	if (err)
		return err;

	/*
	 * Notifier callback is required because backlight device needs
	 * reconfiguration after fault detection procedure is done by
	 * ti-lmu-fault-monitor driver.
	 */
	if (lmu_bank->cfg->fault_monitor_used) {
		lmu_bank->nb.notifier_call = ti_lmu_bl_monitor_notifier;
		err = blocking_notifier_chain_register(&lmu_bank->lmu->notifier,
						       &lmu_bank->nb);
		if (err)
			return err;
	}

	return 0;
}

static int ti_lmu_bl_remove(struct platform_device *pdev)
{
	struct ti_lmu_bank *lmu_bank = platform_get_drvdata(pdev);

	if (lmu_bank->cfg->fault_monitor_used)
		blocking_notifier_chain_unregister(&lmu_bank->lmu->notifier,
						   &lmu_bank->nb);

	ti_lmu_bl_set_brightness(lmu_bank, 0);

	return 0;
}

static struct platform_driver ti_lmu_bl_driver = {
	.probe  = ti_lmu_bl_probe,
	.remove  = ti_lmu_bl_remove,
	.driver = {
		.name = "ti-lmu-led-backlight",
	},
};
module_platform_driver(ti_lmu_bl_driver)

MODULE_DESCRIPTION("TI LMU Backlight LED Driver");
MODULE_AUTHOR("Sebastian Reichel");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:ti-lmu-led-backlight");
