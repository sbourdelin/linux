/*
 * TI LM3632 Backlight Driver
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

#define LM3632_DEFAULT_OVP		LM3632_OVP_25V
#define LM3632_FULL_STRINGS		(LMU_HVLED1 | LMU_HVLED2)
#define LM3632_MAX_BRIGHTNESS		2047

static int lm3632_bl_enable(struct ti_lmu_bl *lmu_bl, int enable)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_ENABLE,
				  LM3632_BL_EN_MASK, enable);
}

static int lm3632_bl_set_brightness(struct ti_lmu_bl *lmu_bl, int brightness)
{
	u8 data;
	int ret;

	if (lmu_bl->mode == BL_PWM_BASED)
		return 0;

	data = brightness & LM3632_BRT_LSB_MASK;
	ret = ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_BRT_LSB,
				 LM3632_BRT_LSB_MASK, data);
	if (ret)
		return ret;

	data = (brightness >> LM3632_BRT_MSB_SHIFT) & 0xFF;
	return ti_lmu_write_byte(lmu_bl->chip->lmu, LM3632_REG_BRT_MSB,
				 data);
}

static int lm3632_bl_string_configure(struct ti_lmu_bl *lmu_bl)
{
	u8 val;

	if (lmu_bl->bl_string == LM3632_FULL_STRINGS)
		val = LM3632_BL_TWO_STRINGS;
	else
		val = LM3632_BL_ONE_STRING;

	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_ENABLE,
				  LM3632_BL_STRING_MASK, val);
}

static int lm3632_bl_set_ovp(struct ti_lmu_bl *lmu_bl)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_CONFIG1,
				  LM3632_OVP_MASK, LM3632_DEFAULT_OVP);
}

static int lm3632_bl_set_swfreq(struct ti_lmu_bl *lmu_bl)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_CONFIG2,
				  LM3632_SWFREQ_MASK, LM3632_SWFREQ_1MHZ);
}

static int lm3632_bl_set_ctrl_mode(struct ti_lmu_bl *lmu_bl)
{
	u8 val;

	if (lmu_bl->mode == BL_PWM_BASED)
		val = LM3632_PWM_MODE;
	else
		val = LM3632_I2C_MODE;

	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3632_REG_IO_CTRL,
				  LM3632_PWM_MASK, val);
}

static int lm3632_bl_configure(struct ti_lmu_bl *lmu_bl)
{
	int ret;

	ret = lm3632_bl_string_configure(lmu_bl);
	if (ret)
		return ret;

	/* Select OVP level */
	ret = lm3632_bl_set_ovp(lmu_bl);
	if (ret)
		return ret;

	/* Select switching frequency */
	ret = lm3632_bl_set_swfreq(lmu_bl);
	if (ret)
		return ret;

	/* Backlight control mode - PWM or I2C */
	return lm3632_bl_set_ctrl_mode(lmu_bl);
}

static const struct ti_lmu_bl_ops lm3632_lmu_ops = {
	.configure		= lm3632_bl_configure,
	.update_brightness	= lm3632_bl_set_brightness,
	.bl_enable		= lm3632_bl_enable,
	.max_brightness		= LM3632_MAX_BRIGHTNESS,
};

/* LM3632 backlight of_device_id */
TI_LMU_BL_OF_DEVICE(lm3632, "ti,lm3632-backlight");

/* LM3632 backlight platform driver */
TI_LMU_BL_PLATFORM_DRIVER(lm3632, "lm3632-backlight");

MODULE_DESCRIPTION("TI LM3632 Backlight Driver");
MODULE_AUTHOR("Milo Kim");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm3632-backlight");
