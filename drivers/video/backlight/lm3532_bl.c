/*
 * TI LM3532 Backlight Driver
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

#define LM3532_PWM1			0
#define LM3532_BL_MAX_STRINGS		3
#define LM3532_MAX_ZONE_CFG		3
#define LM3532_MAX_BRIGHTNESS		255

static int lm3532_bl_init(struct ti_lmu_bl_chip *chip)
{
	int i, ret;
	u8 lm3532_regs[] = { LM3532_REG_ZONE_CFG_A, LM3532_REG_ZONE_CFG_B,
			     LM3532_REG_ZONE_CFG_C, };

	/*
	 * Assign zone targets as below.
	 *   Zone target 0 for control A
	 *   Zone target 1 for control B
	 *   Zone target 2 for control C
	 */

	for (i = 0; i < LM3532_MAX_ZONE_CFG; i++) {
		ret = ti_lmu_update_bits(chip->lmu, lm3532_regs[i],
					 LM3532_ZONE_CFG_MASK,
					 i << LM3532_ZONE_CFG_SHIFT);
		if (ret)
			return ret;
	}

	return 0;
}

static int lm3532_bl_enable(struct ti_lmu_bl *lmu_bl, int enable)
{
	return ti_lmu_update_bits(lmu_bl->chip->lmu, LM3532_REG_ENABLE,
				  BIT(lmu_bl->bank_id),
				  enable << lmu_bl->bank_id);
}

static int lm3532_bl_set_brightness(struct ti_lmu_bl *lmu_bl, int brightness)
{
	u8 reg[] = { LM3532_REG_BRT_A, LM3532_REG_BRT_B, LM3532_REG_BRT_C, };

	return ti_lmu_write_byte(lmu_bl->chip->lmu, reg[lmu_bl->bank_id],
				 brightness);
}

static int lm3532_bl_select_pwm_bank(struct ti_lmu_bl *lmu_bl, int bank_id)
{
	struct ti_lmu *lmu = lmu_bl->chip->lmu;
	u8 mask[]  = { LM3532_PWM_SEL_A_MASK, LM3532_PWM_SEL_B_MASK,
		       LM3532_PWM_SEL_C_MASK, };
	u8 shift[] = { LM3532_PWM_SEL_A_SHIFT, LM3532_PWM_SEL_B_SHIFT,
		       LM3532_PWM_SEL_C_SHIFT, };

	/* Limitation: only PWM1 is supported. PWM2 is not supported. */

	return ti_lmu_update_bits(lmu, LM3532_REG_PWM_CFG_BASE + bank_id,
				  mask[bank_id],
				  (1 << shift[bank_id]) | LM3532_PWM1);
}

static int lm3532_bl_string_configure(struct ti_lmu_bl *lmu_bl)
{
	struct ti_lmu *lmu = lmu_bl->chip->lmu;
	int bank_id = lmu_bl->bank_id;
	int is_detected = 0;
	int i, ret;
	u8 mask[]  = { LM3532_ILED1_CFG_MASK, LM3532_ILED2_CFG_MASK,
		       LM3532_ILED3_CFG_MASK, };
	u8 shift[]  = { LM3532_ILED1_CFG_SHIFT, LM3532_ILED2_CFG_SHIFT,
			LM3532_ILED3_CFG_SHIFT, };

	/* Assign control bank from backlight string configuration */
	for (i = 0; i < LM3532_BL_MAX_STRINGS; i++) {
		if (test_bit(i, &lmu_bl->bl_string)) {
			ret = ti_lmu_update_bits(lmu, LM3532_REG_OUTPUT_CFG,
						 mask[i], bank_id << shift[i]);
			if (ret)
				return ret;

			is_detected = 1;
		}
	}

	if (!is_detected) {
		dev_err(lmu_bl->chip->dev, "No backlight string found\n");
		return -EINVAL;
	}

	if (lmu_bl->mode == BL_PWM_BASED)
		return lm3532_bl_select_pwm_bank(lmu_bl, bank_id);

	return 0;
}

static int lm3532_bl_set_current_limit(struct ti_lmu_bl *lmu_bl)
{
	u8 reg[] = { LM3532_REG_IMAX_A, LM3532_REG_IMAX_B, LM3532_REG_IMAX_C };

	return ti_lmu_write_byte(lmu_bl->chip->lmu, reg[lmu_bl->bank_id],
				 lmu_bl->imax);
}

static int lm3532_bl_set_ramp(struct ti_lmu_bl *lmu_bl)
{
	int ret, index;

	index = ti_lmu_backlight_get_ramp_index(lmu_bl, BL_RAMP_UP);
	if (index > 0) {
		ret = ti_lmu_update_bits(lmu_bl->chip->lmu, LM3532_REG_RAMPUP,
					 LM3532_RAMPUP_MASK,
					 index << LM3532_RAMPUP_SHIFT);
		if (ret)
			return ret;
	}

	index = ti_lmu_backlight_get_ramp_index(lmu_bl, BL_RAMP_DOWN);
	if (index > 0) {
		ret = ti_lmu_update_bits(lmu_bl->chip->lmu, LM3532_REG_RAMPDN,
					 LM3532_RAMPDN_MASK,
					 index << LM3532_RAMPDN_SHIFT);
		if (ret)
			return ret;
	}

	return 0;
}

static int lm3532_bl_configure(struct ti_lmu_bl *lmu_bl)
{
	int ret;

	ret = lm3532_bl_string_configure(lmu_bl);
	if (ret)
		return ret;

	ret = lm3532_bl_set_current_limit(lmu_bl);
	if (ret)
		return ret;

	return lm3532_bl_set_ramp(lmu_bl);
}

/* Backlight ramp up/down time. Unit is msec. */
static const int lm3532_ramp_table[] = { 0, 1, 2, 4, 8, 16, 32, 65 };

static const struct ti_lmu_bl_ops lm3532_lmu_ops = {
	.init			= lm3532_bl_init,
	.configure		= lm3532_bl_configure,
	.update_brightness	= lm3532_bl_set_brightness,
	.bl_enable		= lm3532_bl_enable,
	.max_brightness		= LM3532_MAX_BRIGHTNESS,
	.ramp_table		= lm3532_ramp_table,
	.size_ramp		= ARRAY_SIZE(lm3532_ramp_table),
};

/* LM3532 backlight of_device_id */
TI_LMU_BL_OF_DEVICE(lm3532, "ti,lm3532-backlight");

/* LM3532 backlight platform driver */
TI_LMU_BL_PLATFORM_DRIVER(lm3532, "lm3532-backlight");

MODULE_DESCRIPTION("TI LM3532 Backlight Driver");
MODULE_AUTHOR("Milo Kim");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lm3532-backlight");
