/*
 * Copyright (c) 2016 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or later as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

/* ASPEED PWM & FAN Tach Register Definition */
#define ASPEED_PTCR_CTRL		0x00
#define ASPEED_PTCR_CLK_CTRL		0x04
#define ASPEED_PTCR_DUTY0_CTRL		0x08
#define ASPEED_PTCR_DUTY1_CTRL		0x0c
#define ASPEED_PTCR_TYPEM_CTRL		0x10
#define ASPEED_PTCR_TYPEM_CTRL1		0x14
#define ASPEED_PTCR_TYPEN_CTRL		0x18
#define ASPEED_PTCR_TYPEN_CTRL1		0x1c
#define ASPEED_PTCR_TACH_SOURCE		0x20
#define ASPEED_PTCR_TRIGGER		0x28
#define ASPEED_PTCR_RESULT		0x2c
#define ASPEED_PTCR_INTR_CTRL		0x30
#define ASPEED_PTCR_INTR_STS		0x34
#define ASPEED_PTCR_TYPEM_LIMIT		0x38
#define ASPEED_PTCR_TYPEN_LIMIT		0x3C
#define ASPEED_PTCR_CTRL_EXT		0x40
#define ASPEED_PTCR_CLK_CTRL_EXT	0x44
#define ASPEED_PTCR_DUTY2_CTRL		0x48
#define ASPEED_PTCR_DUTY3_CTRL		0x4c
#define ASPEED_PTCR_TYPEO_CTRL		0x50
#define ASPEED_PTCR_TYPEO_CTRL1		0x54
#define ASPEED_PTCR_TACH_SOURCE_EXT	0x60
#define ASPEED_PTCR_TYPEO_LIMIT		0x78

/* ASPEED_PTCR_CTRL : 0x00 - General Control Register */
#define ASPEED_PTCR_CTRL_SET_PWMD_TYPE_PART1	15
#define ASPEED_PTCR_CTRL_SET_PWMD_TYPE_PART2	6
#define ASPEED_PTCR_CTRL_SET_PWMD_TYPE_MASK	(BIT(7) | BIT(15))

#define ASPEED_PTCR_CTRL_SET_PWMC_TYPE_PART1	14
#define ASPEED_PTCR_CTRL_SET_PWMC_TYPE_PART2	5
#define ASPEED_PTCR_CTRL_SET_PWMC_TYPE_MASK	(BIT(6) | BIT(14))

#define ASPEED_PTCR_CTRL_SET_PWMB_TYPE_PART1	13
#define ASPEED_PTCR_CTRL_SET_PWMB_TYPE_PART2	4
#define ASPEED_PTCR_CTRL_SET_PWMB_TYPE_MASK	(BIT(5) | BIT(13))

#define ASPEED_PTCR_CTRL_SET_PWMA_TYPE_PART1	12
#define ASPEED_PTCR_CTRL_SET_PWMA_TYPE_PART2	3
#define ASPEED_PTCR_CTRL_SET_PWMA_TYPE_MASK	(BIT(4) | BIT(12))

#define	ASPEED_PTCR_CTRL_FAN_NUM_EN(x)	(0x1 << (16 + (x)))

#define	ASPEED_PTCR_CTRL_PWMD_EN	(0x1 << 11)
#define	ASPEED_PTCR_CTRL_PWMC_EN	(0x1 << 10)
#define	ASPEED_PTCR_CTRL_PWMB_EN	(0x1 << 9)
#define	ASPEED_PTCR_CTRL_PWMA_EN	(0x1 << 8)

#define	ASPEED_PTCR_CTRL_CLK_SRC	0x2
#define	ASPEED_PTCR_CTRL_CLK_EN		0x1

/* ASPEED_PTCR_CLK_CTRL : 0x04 - Clock Control Register */
/* TYPE N */
#define ASPEED_PTCR_CLK_CTRL_TYPEN_UNIT		24
#define ASPEED_PTCR_CLK_CTRL_TYPEN_H		20
#define ASPEED_PTCR_CLK_CTRL_TYPEN_L		16
/* TYPE M */
#define ASPEED_PTCR_CLK_CTRL_TYPEM_UNIT		8
#define ASPEED_PTCR_CLK_CTRL_TYPEM_H		4
#define ASPEED_PTCR_CLK_CTRL_TYPEM_L		0

/*
 * ASPEED_PTCR_DUTY_CTRL/1/2/3 : 0x08/0x0C/0x48/0x4C - PWM-FAN duty control
 * 0/1/2/3 register
 */
#define DUTY_CTRL_PWM2_FALL_POINT	24
#define DUTY_CTRL_PWM2_RISE_POINT	16
#define DUTY_CTRL_PWM1_FALL_POINT	8
#define DUTY_CTRL_PWM1_RISE_POINT	0

/* ASPEED_PTCR_TYPEM_CTRL : 0x10/0x18/0x50 - Type M/N/O Ctrl 0 Register */
#define TYPE_CTRL_FAN_PERIOD		16
#define TYPE_CTRL_FAN_MODE		4
#define TYPE_CTRL_FAN_DIVISION		1
#define TYPE_CTRL_FAN_TYPE_EN		1

/* ASPEED_PTCR_TACH_SOURCE : 0x20/0x60 - Tach Source Register */
/* bit [0,1] at 0x20, bit [2] at 0x60 */
#define TACH_PWM_SOURCE_BIT01(x)	((x) * 2)
#define TACH_PWM_SOURCE_BIT2(x)		((x) * 2)
#define TACH_PWM_SOURCE_MASK_BIT01(x)	(0x3 << ((x) * 2))
#define TACH_PWM_SOURCE_MASK_BIT2(x)	(0x1 << ((x) * 2))

/* ASPEED_PTCR_TRIGGER : 0x28 - Trigger Register */
#define TRIGGER_READ_FAN_NUM(x)		(0x1 << (x))

/* ASPEED_PTCR_RESULT : 0x2c - Result Register */
#define RESULT_STATUS			31
#define RESULT_VALUE_MASK		0xfffff

/* ASPEED_PTCR_CTRL_EXT : 0x40 - General Control Extension #1 Register */
#define ASPEED_PTCR_CTRL_SET_PWMH_TYPE_PART1	15
#define ASPEED_PTCR_CTRL_SET_PWMH_TYPE_PART2	6
#define ASPEED_PTCR_CTRL_SET_PWMH_TYPE_MASK	(BIT(7) | BIT(15))

#define ASPEED_PTCR_CTRL_SET_PWMG_TYPE_PART1	14
#define ASPEED_PTCR_CTRL_SET_PWMG_TYPE_PART2	5
#define ASPEED_PTCR_CTRL_SET_PWMG_TYPE_MASK	(BIT(6) | BIT(14))

#define ASPEED_PTCR_CTRL_SET_PWMF_TYPE_PART1	13
#define ASPEED_PTCR_CTRL_SET_PWMF_TYPE_PART2	4
#define ASPEED_PTCR_CTRL_SET_PWMF_TYPE_MASK	(BIT(5) | BIT(13))

#define ASPEED_PTCR_CTRL_SET_PWME_TYPE_PART1	12
#define ASPEED_PTCR_CTRL_SET_PWME_TYPE_PART2	3
#define ASPEED_PTCR_CTRL_SET_PWME_TYPE_MASK	(BIT(4) | BIT(12))

#define	ASPEED_PTCR_CTRL_PWMH_EN	(0x1 << 11)
#define	ASPEED_PTCR_CTRL_PWMG_EN	(0x1 << 10)
#define	ASPEED_PTCR_CTRL_PWMF_EN	(0x1 << 9)
#define	ASPEED_PTCR_CTRL_PWME_EN	(0x1 << 8)

/* ASPEED_PTCR_CLK_EXT_CTRL : 0x44 - Clock Control Extension #1 Register */
/* TYPE O */
#define ASPEED_PTCR_CLK_CTRL_TYPEO_UNIT		8
#define ASPEED_PTCR_CLK_CTRL_TYPEO_H		4
#define ASPEED_PTCR_CLK_CTRL_TYPEO_L		0

#define MCLK 1
#define PWM_MAX 255
#define MAX_HIGH_LOW_BIT 15

struct aspeed_pwm_tacho_data {
	void __iomem *base;
	unsigned long clk_freq;
	const struct attribute_group *groups[24];
	u8 type_pwm_clock_unit[3];
	u8 type_pwm_clock_division_h[3];
	u8 type_pwm_clock_division_l[3];
	u8 type_fan_tach_clock_division[3];
	u16 type_fan_tach_unit[3];
	u8 pwm_port_type[8];
	u8 pwm_port_fan_ctrl[8];
	u8 fan_tach_ch_source[16];
};

enum type { TYPEM, TYPEN, TYPEO };

struct type_params {
	u32 l_value;
	u32 h_value;
	u32 unit_value;
	u32 clk_ctrl_reg;
	u32 ctrl_reg;
	u32 ctrl_reg1;
};

static const struct type_params type_params[] = {
	[TYPEM] = {
		.l_value = ASPEED_PTCR_CLK_CTRL_TYPEM_L,
		.h_value = ASPEED_PTCR_CLK_CTRL_TYPEM_H,
		.unit_value = ASPEED_PTCR_CLK_CTRL_TYPEM_UNIT,
		.clk_ctrl_reg = ASPEED_PTCR_CLK_CTRL,
		.ctrl_reg = ASPEED_PTCR_TYPEM_CTRL,
		.ctrl_reg1 = ASPEED_PTCR_TYPEM_CTRL1,
	},
	[TYPEN] = {
		.l_value = ASPEED_PTCR_CLK_CTRL_TYPEN_L,
		.h_value = ASPEED_PTCR_CLK_CTRL_TYPEN_H,
		.unit_value = ASPEED_PTCR_CLK_CTRL_TYPEN_UNIT,
		.clk_ctrl_reg = ASPEED_PTCR_CLK_CTRL,
		.ctrl_reg = ASPEED_PTCR_TYPEN_CTRL,
		.ctrl_reg1 = ASPEED_PTCR_TYPEN_CTRL1,
	},
	[TYPEO] = {
		.l_value = ASPEED_PTCR_CLK_CTRL_TYPEO_L,
		.h_value = ASPEED_PTCR_CLK_CTRL_TYPEO_H,
		.unit_value = ASPEED_PTCR_CLK_CTRL_TYPEO_UNIT,
		.clk_ctrl_reg = ASPEED_PTCR_CLK_CTRL_EXT,
		.ctrl_reg = ASPEED_PTCR_TYPEO_CTRL,
		.ctrl_reg1 = ASPEED_PTCR_TYPEO_CTRL1,
	}
};

enum pwm_port { PWMA, PWMB, PWMC, PWMD, PWME, PWMF, PWMG, PWMH };

struct pwm_port_params {
	u32 pwm_en;
	u32 ctrl_reg;
	u32 type_part1;
	u32 type_part2;
	u32 type_mask;
	u32 duty_ctrl_rise_point;
	u32 duty_ctrl_fall_point;
	u32 duty_ctrl_reg;
	u8 duty_ctrl_calc_type;
};

static const struct pwm_port_params pwm_port_params[] = {
	[PWMA] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMA_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMA_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMA_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMA_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM1_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM1_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY0_CTRL,
		.duty_ctrl_calc_type = 0,
	},
	[PWMB] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMB_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMB_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMB_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMB_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM2_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM2_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY0_CTRL,
		.duty_ctrl_calc_type = 1,
	},
	[PWMC] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMC_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMC_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMC_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMC_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM1_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM1_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY1_CTRL,
		.duty_ctrl_calc_type = 0,
	},
	[PWMD] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMD_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMD_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMD_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMD_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM2_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM2_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY1_CTRL,
		.duty_ctrl_calc_type = 1,
	},
	[PWME] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWME_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL_EXT,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWME_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWME_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWME_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM1_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM1_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY2_CTRL,
		.duty_ctrl_calc_type = 0,
	},
	[PWMF] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMF_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL_EXT,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMF_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMF_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMF_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM2_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM2_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY2_CTRL,
		.duty_ctrl_calc_type = 1,
	},
	[PWMG] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMG_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL_EXT,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMG_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMG_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMG_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM1_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM1_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY3_CTRL,
		.duty_ctrl_calc_type = 0,
	},
	[PWMH] = {
		.pwm_en = ASPEED_PTCR_CTRL_PWMH_EN,
		.ctrl_reg = ASPEED_PTCR_CTRL_EXT,
		.type_part1 = ASPEED_PTCR_CTRL_SET_PWMH_TYPE_PART1,
		.type_part2 = ASPEED_PTCR_CTRL_SET_PWMH_TYPE_PART2,
		.type_mask = ASPEED_PTCR_CTRL_SET_PWMH_TYPE_MASK,
		.duty_ctrl_rise_point = DUTY_CTRL_PWM2_RISE_POINT,
		.duty_ctrl_fall_point = DUTY_CTRL_PWM2_FALL_POINT,
		.duty_ctrl_reg = ASPEED_PTCR_DUTY3_CTRL,
		.duty_ctrl_calc_type = 1,
	}
};

static void aspeed_set_clock_enable(void __iomem *base, bool val)
{
	u32 reg_value = ioread32(base + ASPEED_PTCR_CTRL);

	if (val)
		reg_value |= ASPEED_PTCR_CTRL_CLK_EN;
	else
		reg_value &= ~ASPEED_PTCR_CTRL_CLK_EN;

	iowrite32(reg_value, base + ASPEED_PTCR_CTRL);
}

static void aspeed_set_clock_source(void __iomem *base, int val)
{
	u32 reg_value = ioread32(base + ASPEED_PTCR_CTRL);

	if (val == MCLK)
		reg_value |= ASPEED_PTCR_CTRL_CLK_SRC;
	else
		reg_value &= ~ASPEED_PTCR_CTRL_CLK_SRC;

	iowrite32(reg_value, base + ASPEED_PTCR_CTRL);
}

static void aspeed_set_pwm_clock_values(void __iomem *base, u8 type,
					u8 div_high, u8 div_low, u8 unit)
{
	u32 reg_offset = type_params[type].clk_ctrl_reg;
	u32 reg_value = ioread32(base + reg_offset);

	reg_value &= ~((0xF << type_params[type].h_value) |
	(0xF << type_params[type].l_value) |
	(0xFF << type_params[type].unit_value));
	reg_value |= ((div_high << type_params[type].h_value) |
	(div_low << type_params[type].l_value) |
	(unit << type_params[type].unit_value));

	iowrite32(reg_value, base + reg_offset);
}

static void aspeed_set_pwm_port_enable(void __iomem *base, u8 pwm_port,
				       bool enable)
{
	u32 reg_offset = pwm_port_params[pwm_port].ctrl_reg;
	u32 reg_value = ioread32(base + reg_offset);

	if (enable)
		reg_value |= pwm_port_params[pwm_port].pwm_en;
	else
		reg_value &= ~pwm_port_params[pwm_port].pwm_en;
	iowrite32(reg_value, base + reg_offset);
}

static void aspeed_set_pwm_port_type(void __iomem *base, u8 pwm_port, u8 type)
{
	u32 reg_offset = pwm_port_params[pwm_port].ctrl_reg;
	u32 reg_value = ioread32(base + reg_offset);

	reg_value &= ~pwm_port_params[pwm_port].type_mask;
	reg_value |= (type & 0x1) <<
		pwm_port_params[pwm_port].type_part1;
	reg_value |= (type & 0x2) <<
		pwm_port_params[pwm_port].type_part2;

	iowrite32(reg_value, base + reg_offset);
}

static void aspeed_set_pwm_port_duty_rising_falling(void __iomem *base,
						    u8 pwm_port, u8 rising,
						    u8 falling)
{
	u32 reg_offset = pwm_port_params[pwm_port].duty_ctrl_reg;
	u32 reg_value = ioread32(base + reg_offset);

	reg_value &= ~(0xFF << pwm_port_params[pwm_port].duty_ctrl_rise_point);
	reg_value &= ~(0xFF << pwm_port_params[pwm_port].duty_ctrl_fall_point);

	if (pwm_port_params[pwm_port].duty_ctrl_calc_type == 0) {
		reg_value |= rising;
	} else if (pwm_port_params[pwm_port].duty_ctrl_calc_type == 1) {
		reg_value |= (rising <<
			pwm_port_params[pwm_port].duty_ctrl_rise_point);
	}
	reg_value |= (falling <<
			pwm_port_params[pwm_port].duty_ctrl_fall_point);

	iowrite32(reg_value, base + reg_offset);
}

static void aspeed_set_tacho_type_enable(void __iomem *base, u8 type,
					 bool enable)
{
	u32 reg_offset = type_params[type].ctrl_reg;
	u32 reg_value = ioread32(base + reg_offset);

	if (enable)
		reg_value |= TYPE_CTRL_FAN_TYPE_EN;
	else
		reg_value &= ~TYPE_CTRL_FAN_TYPE_EN;

	iowrite32(reg_value, base + reg_offset);
}

static void aspeed_set_tacho_type_values(void __iomem *base, u8 type, u8 mode,
					 u16 unit, u8 division)
{
	u32 reg_offset = type_params[type].ctrl_reg;
	u32 reg_offset1 = type_params[type].ctrl_reg1;
	u32 reg_value = ioread32(base + reg_offset);

	reg_value &= ~((0x3 << TYPE_CTRL_FAN_MODE) |
			(0xFFFF << TYPE_CTRL_FAN_PERIOD) |
			(0x7 << TYPE_CTRL_FAN_DIVISION));
	reg_value |= ((mode << TYPE_CTRL_FAN_MODE) |
			(unit << TYPE_CTRL_FAN_PERIOD) |
			(division << TYPE_CTRL_FAN_DIVISION));

	iowrite32(reg_value, base + reg_offset);

	iowrite32(unit << 16, base + reg_offset1);
}

static void aspeed_set_fan_tach_ch_enable(void __iomem *base, u8 fan_tach_ch,
					  bool enable)
{
	u32 reg_value = ioread32(base + ASPEED_PTCR_CTRL);

	if (enable)
		reg_value |= ASPEED_PTCR_CTRL_FAN_NUM_EN(fan_tach_ch);
	else
		reg_value &= ~ASPEED_PTCR_CTRL_FAN_NUM_EN(fan_tach_ch);

	iowrite32(reg_value, base + ASPEED_PTCR_CTRL);
}

static void aspeed_set_fan_tach_ch_source(void __iomem *base, u8 fan_tach_ch,
					  u8 fan_tach_ch_source)
{
	u32 reg_value1 = ioread32(base + ASPEED_PTCR_TACH_SOURCE);
	u32 reg_value2 = ioread32(base + ASPEED_PTCR_TACH_SOURCE_EXT);

	reg_value1 &= ~(TACH_PWM_SOURCE_MASK_BIT01(fan_tach_ch));
	reg_value1 |= ((fan_tach_ch_source & 0x3) <<
			(TACH_PWM_SOURCE_BIT01(fan_tach_ch)));

	reg_value2 &= ~(TACH_PWM_SOURCE_MASK_BIT2(fan_tach_ch));
	reg_value2 |= (((fan_tach_ch_source & 0x4) >> 2) <<
			(TACH_PWM_SOURCE_BIT2(fan_tach_ch)));

	iowrite32(reg_value1, base + ASPEED_PTCR_TACH_SOURCE);
	iowrite32(reg_value2, base + ASPEED_PTCR_TACH_SOURCE_EXT);
}

static void aspeed_set_pwm_port_fan_ctrl(struct aspeed_pwm_tacho_data *priv,
					 u8 index, u8 fan_ctrl)
{
	u16 period;
	u16 dc_time_on;

	period = priv->type_pwm_clock_unit[priv->pwm_port_type[index]];
	period += 1;
	dc_time_on = (fan_ctrl * period) / PWM_MAX;

	if (dc_time_on == 0) {
		aspeed_set_pwm_port_enable(priv->base, index, false);
	} else {
		if (dc_time_on == period)
			dc_time_on = 0;

		aspeed_set_pwm_port_duty_rising_falling(priv->base, index, 0,
							dc_time_on);
		aspeed_set_pwm_port_enable(priv->base, index, true);
	}
}

static u32 aspeed_get_fan_tach_ch_measure_period(struct aspeed_pwm_tacho_data
						 *priv, u8 type)
{
	u32 clk;
	u16 tacho_unit;
	u8 clk_unit, div_h, div_l, tacho_div;

	clk = priv->clk_freq;

	clk_unit = priv->type_pwm_clock_unit[type];
	div_h = priv->type_pwm_clock_division_h[type];
	div_h = 0x1 << div_h;
	div_l = priv->type_pwm_clock_division_l[type];
	if (div_l == 0)
		div_l = 1;
	else
		div_l = div_l * 2;

	tacho_unit = priv->type_fan_tach_unit[type];
	tacho_div = priv->type_fan_tach_clock_division[type];

	tacho_div = 0x4 << (tacho_div * 2);
	return clk / (clk_unit * div_h * div_l * tacho_div * tacho_unit);
}

static u32 aspeed_get_fan_tach_ch_rpm(struct aspeed_pwm_tacho_data *priv,
				      u8 fan_tach_ch)
{
	u32 raw_data, rpm, tach_div, clk_source, timeout = 0, sec;
	u8 fan_tach_ch_source, type;
	void __iomem *base = priv->base;

	iowrite32(0, priv->base + ASPEED_PTCR_TRIGGER);
	iowrite32(0x1 << fan_tach_ch, priv->base + ASPEED_PTCR_TRIGGER);

	fan_tach_ch_source = priv->fan_tach_ch_source[fan_tach_ch];
	type = priv->pwm_port_type[fan_tach_ch_source];

	sec = (1000 / aspeed_get_fan_tach_ch_measure_period(priv, type));

	msleep(sec);

	while (!(ioread32(priv->base + ASPEED_PTCR_RESULT) &
				(0x1 << RESULT_STATUS))) {
		timeout++;
		if (timeout > 1)
			return 0;
		msleep(sec);
	};

	raw_data = (ioread32(base + ASPEED_PTCR_RESULT)) &
				RESULT_VALUE_MASK;
	tach_div = priv->type_fan_tach_clock_division[type];

	tach_div = 0x4 << (tach_div * 2);
	clk_source = priv->clk_freq;
	rpm = (clk_source * 60) / (2 * raw_data * tach_div);
	return rpm;
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct sensor_device_attribute *sensor_attr = to_sensor_dev_attr(attr);
	int index = sensor_attr->index;
	int ret;

	struct aspeed_pwm_tacho_data *priv = dev_get_drvdata(dev);
	long fan_ctrl;

	ret = kstrtol(buf, 10, &fan_ctrl);
	if (ret != 0)
		return ret;

	if (fan_ctrl < 0 || fan_ctrl > PWM_MAX)
		return -EINVAL;

	if (priv->pwm_port_fan_ctrl[index] == fan_ctrl)
		return count;

	priv->pwm_port_fan_ctrl[index] = fan_ctrl;
	aspeed_set_pwm_port_fan_ctrl(priv, index, fan_ctrl);

	return count;
}

static ssize_t show_pwm(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sensor_device_attribute *sensor_attr = to_sensor_dev_attr(attr);
	int index = sensor_attr->index;

	struct aspeed_pwm_tacho_data *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", priv->pwm_port_fan_ctrl[index]);
}

static ssize_t show_rpm(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sensor_device_attribute *sensor_attr = to_sensor_dev_attr(attr);
	int index = sensor_attr->index;
	u32 rpm;

	struct aspeed_pwm_tacho_data *priv = dev_get_drvdata(dev);

	rpm = aspeed_get_fan_tach_ch_rpm(priv, index);

	return sprintf(buf, "%u\n", rpm);
}

#define pwm_index(index)					\
static SENSOR_DEVICE_ATTR(pwm##index, 0644,	\
			show_pwm, set_pwm, index - 1);		\
								\
static struct attribute *pwm##index##_dev_attrs[] = {		\
	&sensor_dev_attr_pwm##index.dev_attr.attr,		\
	NULL,							\
};								\
static const struct attribute_group pwm##index##_dev_groups = {	\
	.attrs = pwm##index##_dev_attrs,			\
}

#define fan_index(index)					\
static SENSOR_DEVICE_ATTR(fan##index##_input, 0444,		\
		show_rpm, NULL, index - 1);			\
								\
static struct attribute *fan##index##_dev_attrs[] = {		\
	&sensor_dev_attr_fan##index##_input.dev_attr.attr,	\
	NULL,							\
};								\
static const struct attribute_group fan##index##_dev_groups = {	\
	.attrs = fan##index##_dev_attrs,			\
}

pwm_index(1);
pwm_index(2);
pwm_index(3);
pwm_index(4);
pwm_index(5);
pwm_index(6);
pwm_index(7);
pwm_index(8);

fan_index(1);
fan_index(2);
fan_index(3);
fan_index(4);
fan_index(5);
fan_index(6);
fan_index(7);
fan_index(8);
fan_index(9);
fan_index(10);
fan_index(11);
fan_index(12);
fan_index(13);
fan_index(14);
fan_index(15);
fan_index(16);

/*
 * If the clock type is type M then :
 * The PWM frequency = 24MHz / (type M clock division L bit *
 * type M clock division H bit * (type M PWM period bit + 1))
 * Calculate type M clock division L bit and H bits given the other values
 */
static int aspeed_create_type(struct device_node *child,
			      struct aspeed_pwm_tacho_data *priv,
			      u8 index)
{
	u8 period, div_l, div_h;
	bool enable;
	u8 mode, div;
	u16 unit;

	of_property_read_u8(child, "pwm_period", &period);
	of_property_read_u8(child, "pwm_clock_division_h", &div_h);
	of_property_read_u8(child, "pwm_clock_division_l", &div_l);
	priv->type_pwm_clock_division_h[index] = div_h;
	priv->type_pwm_clock_division_l[index] = div_l;
	priv->type_pwm_clock_unit[index] = period;
	aspeed_set_pwm_clock_values(priv->base, index, div_h, div_l, period);

	enable = of_property_read_bool(child, "fan_tach_enable");
	aspeed_set_tacho_type_enable(priv->base, index, enable);

	of_property_read_u8(child, "fan_tach_clock_division", &div);
	priv->type_fan_tach_clock_division[index] = div;

	of_property_read_u8(child, "fan_tach_mode_selection", &mode);

	of_property_read_u16(child, "fan_tach_period", &unit);
	priv->type_fan_tach_unit[index] = unit;
	aspeed_set_tacho_type_values(priv->base, index, mode, unit, div);

	return 0;
}

static int aspeed_create_pwm_port(struct device_node *child,
				  struct aspeed_pwm_tacho_data *priv, u8 index,
				  u8 group_index)
{
	u8 val;
	bool enable;

	switch (index) {
	case 1:
		priv->groups[group_index] = &pwm1_dev_groups;
		break;
	case 2:
		priv->groups[group_index] = &pwm2_dev_groups;
		break;
	case 3:
		priv->groups[group_index] = &pwm3_dev_groups;
		break;
	case 4:
		priv->groups[group_index] = &pwm4_dev_groups;
		break;
	case 5:
		priv->groups[group_index] = &pwm5_dev_groups;
		break;
	case 6:
		priv->groups[group_index] = &pwm6_dev_groups;
		break;
	case 7:
		priv->groups[group_index] = &pwm7_dev_groups;
		break;
	case 8:
		priv->groups[group_index] = &pwm8_dev_groups;
		break;
	}

	enable = of_property_read_bool(child, "enable");
	aspeed_set_pwm_port_enable(priv->base, index - 1, enable);

	of_property_read_u8(child, "type", &val);
	priv->pwm_port_type[index - 1] = val;
	aspeed_set_pwm_port_type(priv->base, index - 1, val);

	of_property_read_u8(child, "fan_ctrl", &val);
	priv->pwm_port_fan_ctrl[index - 1] = val;
	aspeed_set_pwm_port_fan_ctrl(priv, index - 1, val);

	return 0;
}

static int aspeed_create_fan_tach_channel(struct device *dev,
					  struct device_node *child,
					  struct aspeed_pwm_tacho_data *priv,
					  u8 index, u8 group_index)
{
	u8 val;
	bool enable;
	struct gpio_desc *fan_ctrl = devm_gpiod_get(dev, "fan-ctrl", GPIOD_IN);

	if (IS_ERR(fan_ctrl))
		return PTR_ERR(fan_ctrl);

	switch (index) {
	case 1:
		priv->groups[group_index] = &fan1_dev_groups;
		break;
	case 2:
		priv->groups[group_index] = &fan2_dev_groups;
		break;
	case 3:
		priv->groups[group_index] = &fan3_dev_groups;
		break;
	case 4:
		priv->groups[group_index] = &fan4_dev_groups;
		break;
	case 5:
		priv->groups[group_index] = &fan5_dev_groups;
		break;
	case 6:
		priv->groups[group_index] = &fan6_dev_groups;
		break;
	case 7:
		priv->groups[group_index] = &fan7_dev_groups;
		break;
	case 8:
		priv->groups[group_index] = &fan8_dev_groups;
		break;
	case 9:
		priv->groups[group_index] = &fan9_dev_groups;
		break;
	case 10:
		priv->groups[group_index] = &fan10_dev_groups;
		break;
	case 11:
		priv->groups[group_index] = &fan11_dev_groups;
		break;
	case 12:
		priv->groups[group_index] = &fan12_dev_groups;
		break;
	case 13:
		priv->groups[group_index] = &fan13_dev_groups;
		break;
	case 14:
		priv->groups[group_index] = &fan14_dev_groups;
		break;
	case 15:
		priv->groups[group_index] = &fan15_dev_groups;
		break;
	case 16:
		priv->groups[group_index] = &fan16_dev_groups;
		break;
	}

	enable = of_property_read_bool(child, "enable");
	aspeed_set_fan_tach_ch_enable(priv->base, index - 1, enable);

	of_property_read_u8(child, "pwm_source", &val);
	priv->fan_tach_ch_source[index - 1] = val;
	aspeed_set_fan_tach_ch_source(priv->base, index - 1, val);

	return 0;
}

static int aspeed_pwm_tacho_probe(struct platform_device *pdev)
{
	struct device_node *np, *type_np, *pwm_np, *fan_tach_np, *child;
	u8 pwm_index = 1, fan_index = 1, type_index = 0, group_index = 0;
	struct aspeed_pwm_tacho_data *priv;
	struct resource *res;
	struct device *hwmon;
	void __iomem *base;
	struct clk *clk;

	np = pdev->dev.of_node;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOENT;

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->base = base;

	iowrite32(0, base + ASPEED_PTCR_TACH_SOURCE);
	iowrite32(0, base + ASPEED_PTCR_TACH_SOURCE_EXT);

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return -ENODEV;

	priv->clk_freq = clk_get_rate(clk);
	aspeed_set_clock_enable(base, true);
	aspeed_set_clock_source(base, 0);

	type_np = of_get_child_by_name(np, "type_values");
	for_each_child_of_node(type_np, child) {
		aspeed_create_type(child, priv, type_index++);
		of_node_put(child);
	}
	of_node_put(type_np);
	pwm_np = of_get_child_by_name(np, "pwm_port");
	for_each_child_of_node(pwm_np, child) {
		aspeed_create_pwm_port(child, priv, pwm_index++,
				       group_index++);
		of_node_put(child);
	}
	of_node_put(pwm_np);
	fan_tach_np = of_get_child_by_name(np, "fan_tach_channel");
	for_each_child_of_node(fan_tach_np, child) {
		aspeed_create_fan_tach_channel(&pdev->dev, child, priv,
					       fan_index++, group_index++);
		of_node_put(child);
	}
	of_node_put(fan_tach_np);
	of_node_put(np);

	hwmon = devm_hwmon_device_register_with_groups(&pdev->dev,
						       "aspeed_pwm_tacho",
						       priv, priv->groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	return 0;
}

static const struct of_device_id of_pwm_tacho_match_table[] = {
	{ .compatible = "aspeed,aspeed2400-pwm-tacho", },
	{ .compatible = "aspeed,aspeed2500-pwm-tacho", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_tacho_match_table);

static struct platform_driver aspeed_pwm_tacho_driver = {
	.probe		= aspeed_pwm_tacho_probe,
	.driver		= {
		.name	= "aspeed_pwm_tacho",
		.owner	= THIS_MODULE,
		.of_match_table = of_pwm_tacho_match_table,
	},
};

module_platform_driver(aspeed_pwm_tacho_driver);

MODULE_AUTHOR("Jaghathiswari Rankappagounder Natarajan <jaghu@google.com>");
MODULE_DESCRIPTION("ASPEED PWM and Fan Tacho device driver");
MODULE_LICENSE("GPL");
