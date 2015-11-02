/*
 * TI LM3633 Backlight Driver
 *
 * Copyright 2015 Texas Instruments
 *
 * Author: Milo Kim <milo.kim@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/mfd/ti-lmu.h>
#include <linux/mfd/ti-lmu-register.h>
#include <linux/module.h>

#include "ti-lmu-backlight.h"

#define LM3633_DEFAULT_OVP		LM3633_BOOST_OVP_40V
#define LM3633_BL_MAX_STRINGS		3
#define LM3633_BL_MAX_BRIGHTNESS	2047

static int lm3633_bl_init(struct ti_lmu_bl_chip *chip)
{
	/* Configure ramp selection for each bank */
	return ti_lmu_update_bits(chip->lmu, LM3633_REG_BL_RAMP_CONF,
				  LM3633_BL_RAMP_MASK, LM3633_BL_RAMP_EACH);
}

static int lm3633_bl_enable(struct ti_lmu_bl *lmu_bl, int enable)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3633_REG_ENABLE,
				  BIT(lmu_bl->bank_id),
				  enable << lmu_bl->bank_id);
}

static int lm3633_bl_set_brightness(struct ti_lmu_bl *lmu_bl, int brightness)
{
	int ret;
	u8 data;
	u8 reg_lsb[] = { LM3633_REG_BRT_HVLED_A_LSB,
			 LM3633_REG_BRT_HVLED_B_LSB, };
	u8 reg_msb[] = { LM3633_REG_BRT_HVLED_A_MSB,
			 LM3633_REG_BRT_HVLED_B_MSB, };

	if (lmu_bl->mode == BL_PWM_BASED) {
		/*
		 * PWM can start from any non-zero code and dim down to zero.
		 * So, brightness register should be updated even in PWM mode.
		 */
		if (brightness > 0)
			brightness = LM3633_BL_MAX_BRIGHTNESS;
		else
			brightness = 0;
	}

	data = brightness & LM3633_BRT_HVLED_LSB_MASK;
	ret = ti_lmu_update_bits(lmu_bl->chip->lmu, reg_lsb[lmu_bl->bank_id],
				 LM3633_BRT_HVLED_LSB_MASK, data);
	if (ret)
		return ret;

	data = (brightness >> LM3633_BRT_HVLED_MSB_SHIFT) & 0xFF;
	return ti_lmu_write_byte(lmu_bl->chip->lmu, reg_msb[lmu_bl->bank_id],
				 data);
}

static int lm3633_bl_boost_configure(struct ti_lmu_bl *lmu_bl)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3633_REG_BOOST_CFG,
				  LM3633_BOOST_OVP_MASK, LM3633_DEFAULT_OVP);
}

static int lm3633_bl_set_ctrl_mode(struct ti_lmu_bl *lmu_bl)
{
	int bank_id = lmu_bl->bank_id;

	if (lmu_bl->mode == BL_PWM_BASED)
		return ti_lmu_update_bits(lmu_bl->chip->lmu,
					  LM3633_REG_PWM_CFG,
					  BIT(bank_id), 1 << bank_id);

	return 0;
}

static int lm3633_bl_string_configure(struct ti_lmu_bl *lmu_bl)
{
	struct ti_lmu *lmu = lmu_bl->chip->lmu;
	int is_detected = 0;
	int i, ret;

	/* Assign control bank from backlight string configuration */
	for (i = 0; i < LM3633_BL_MAX_STRINGS; i++) {
		if (test_bit(i, &lmu_bl->bl_string)) {
			ret = ti_lmu_update_bits(lmu,
						 LM3633_REG_HVLED_OUTPUT_CFG,
						 BIT(i), lmu_bl->bank_id << i);
			if (ret)
				return ret;

			is_detected = 1;
		}
	}

	if (!is_detected) {
		dev_err(lmu_bl->chip->dev, "No backlight string found\n");
		return -EINVAL;
	}

	return 0;
}

static int lm3633_bl_set_current_limit(struct ti_lmu_bl *lmu_bl)
{
	u8 reg[] = { LM3633_REG_IMAX_HVLED_A, LM3633_REG_IMAX_HVLED_B, };

	return ti_lmu_write_byte(lmu_bl->chip->lmu, reg[lmu_bl->bank_id],
				 lmu_bl->imax);
}

static int lm3633_bl_set_ramp(struct ti_lmu_bl *lmu_bl)
{
	int ret, index;
	u8 reg;

	index = ti_lmu_backlight_get_ramp_index(lmu_bl, BL_RAMP_UP);
	if (index > 0) {
		if (lmu_bl->bank_id == 0)
			reg = LM3633_REG_BL0_RAMPUP;
		else
			reg = LM3633_REG_BL1_RAMPUP;

		ret = ti_lmu_update_bits(lmu_bl->chip->lmu, reg,
					 LM3633_BL_RAMPUP_MASK,
					 index << LM3633_BL_RAMPUP_SHIFT);
		if (ret)
			return ret;
	}

	index = ti_lmu_backlight_get_ramp_index(lmu_bl, BL_RAMP_DOWN);
	if (index > 0) {
		if (lmu_bl->bank_id == 0)
			reg = LM3633_REG_BL0_RAMPDN;
		else
			reg = LM3633_REG_BL1_RAMPDN;

		ret = ti_lmu_update_bits(lmu_bl->chip->lmu, reg,
					 LM3633_BL_RAMPDN_MASK,
					 index << LM3633_BL_RAMPDN_SHIFT);
		if (ret)
			return ret;
	}

	return 0;
}

static int lm3633_bl_configure(struct ti_lmu_bl *lmu_bl)
{
	int ret;

	ret = lm3633_bl_boost_configure(lmu_bl);
	if (ret)
		return ret;

	ret = lm3633_bl_set_ctrl_mode(lmu_bl);
	if (ret)
		return ret;

	ret = lm3633_bl_string_configure(lmu_bl);
	if (ret)
		return ret;

	ret = lm3633_bl_set_current_limit(lmu_bl);
	if (ret)
		return ret;

	ret = lm3633_bl_set_ramp(lmu_bl);
	if (ret)
		return ret;

	return 0;
}

/* Backlight ramp up/down time. Unit is msec. */
static const int lm3633_ramp_table[] = {
	   2, 250, 500, 1000, 2000, 4000, 8000, 16000,
};

static const struct ti_lmu_bl_ops lm3633_lmu_ops = {
	.init			= lm3633_bl_init,
	.configure		= lm3633_bl_configure,
	.update_brightness	= lm3633_bl_set_brightness,
	.bl_enable		= lm3633_bl_enable,
	.hwmon_notifier_used	= true,
	.max_brightness		= LM3633_BL_MAX_BRIGHTNESS,
	.ramp_table		= lm3633_ramp_table,
	.size_ramp		= ARRAY_SIZE(lm3633_ramp_table),
};

/* LM3633 backlight of_device_id */
TI_LMU_BL_OF_DEVICE(lm3633, "ti,lm3633-backlight");

/* LM3633 backlight platform driver */
TI_LMU_BL_PLATFORM_DRIVER(lm3633, "lm3633-backlight");

MODULE_DESCRIPTION("TI LM3633 Backlight Driver");
MODULE_AUTHOR("Milo Kim");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm3633-backlight");
