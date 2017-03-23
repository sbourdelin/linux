/*
 * Copyright (c) 2017 Linaro Ltd
 * Copyright (c) 2010-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "leds-qcom-lpg.h"

#define LPG_PATTERN_CONFIG_REG	0x40
#define LPG_SIZE_CLK_REG	0x41
#define LPG_PREDIV_CLK_REG	0x42
#define PWM_TYPE_CONFIG_REG	0x43
#define PWM_VALUE_REG		0x44
#define PWM_ENABLE_CONTROL_REG	0x46
#define PWM_SYNC_REG		0x47
#define LPG_RAMP_DURATION_REG	0x50
#define LPG_HI_PAUSE_REG	0x52
#define LPG_LO_PAUSE_REG	0x54
#define LPG_HI_IDX_REG		0x56
#define LPG_LO_IDX_REG		0x57
#define PWM_SEC_ACCESS_REG	0xd0
#define PWM_DTEST_REG(x)	(0xe2 + (x) - 1)

/*
 * lpg - LPG device context
 * @dev:	struct device for LPG device
 * @map:	regmap for register access
 * @reg:	base address of the LPG device
 * @dtest_line:	DTEST line for output, or 0 if disabled
 * @dtest_value: DTEST line configuration
 * @is_lpg:	operating as LPG, in contrast to simple PWM
 * @cdev:	LED class object
 * @tri_led:	reference to TRILED color object, optional
 * @chip:	PWM-chip object, if operating in PWM mode
 * @period_us:	period (in microseconds) of the generated pulses
 * @pwm_value:	duty (in microseconds) of the generated pulses, overriden by LUT
 * @enabled:	output enabled?
 * @pwm_size:	resolution of the @pwm_value, 6 or 9 bits
 * @clk:	base frequency of the clock generator
 * @pre_div:	divider of @clk
 * @pre_div_exp: exponential divider of @clk
 * @ramp_enabled: duty cycle is driven by iterating over lookup table
 * @ramp_ping_pong: reverse through pattern, rather than wrapping to start
 * @ramp_oneshot: perform only a single pass over the pattern
 * @ramp_reverse: iterate over pattern backwards
 * @ramp_duration_ms: length (in milliseconds) of one pattern run
 * @ramp_lo_pause_ms: pause (in milliseconds) before iterating over pattern
 * @ramp_hi_pause_ms: pause (in milliseconds) after iterating over pattern
 * @lut:	LUT context reference
 * @pattern:	reference to allocated pattern with LUT
 */

struct lpg {
	struct device *dev;
	struct regmap *map;

	u32 reg;
	int dtest_line;
	int dtest_value;

	bool is_lpg;

	struct led_classdev cdev;

	struct qcom_tri_led *tri_led;

	struct pwm_chip chip;

	unsigned int period_us;

	u16 pwm_value;
	bool enabled;

	unsigned int pwm_size;
	unsigned int clk;
	unsigned int pre_div;
	unsigned int pre_div_exp;

	bool ramp_enabled;
	bool ramp_ping_pong;
	bool ramp_oneshot;
	bool ramp_reverse;
	unsigned long ramp_duration_ms;
	unsigned long ramp_lo_pause_ms;
	unsigned long ramp_hi_pause_ms;

	struct qcom_lpg_lut *lut;
	struct qcom_lpg_pattern *pattern;
};

#define NUM_PWM_PREDIV	4
#define NUM_PWM_CLK	3
#define NUM_EXP		7

static const unsigned int lpg_clk_table[NUM_PWM_PREDIV][NUM_PWM_CLK] = {
	{
		1 * (NSEC_PER_SEC / 1024),
		1 * (NSEC_PER_SEC / 32768),
		1 * (NSEC_PER_SEC / 19200000),
	},
	{
		3 * (NSEC_PER_SEC / 1024),
		3 * (NSEC_PER_SEC / 32768),
		3 * (NSEC_PER_SEC / 19200000),
	},
	{
		5 * (NSEC_PER_SEC / 1024),
		5 * (NSEC_PER_SEC / 32768),
		5 * (NSEC_PER_SEC / 19200000),
	},
	{
		6 * (NSEC_PER_SEC / 1024),
		6 * (NSEC_PER_SEC / 32768),
		6 * (NSEC_PER_SEC / 19200000),
	},
};

/*
 * PWM Frequency = Clock Frequency / (N * T)
 *      or
 * PWM Period = Clock Period * (N * T)
 *      where
 * N = 2^9 or 2^6 for 9-bit or 6-bit PWM size
 * T = Pre-divide * 2^m, where m = 0..7 (exponent)
 *
 * This is the formula to figure out m for the best pre-divide and clock:
 * (PWM Period / N) = (Pre-divide * Clock Period) * 2^m
 */
static void lpg_calc_freq(struct lpg *lpg, unsigned int period_us)
{
	int             n, m, clk, div;
	int             best_m, best_div, best_clk;
	unsigned int    last_err, cur_err, min_err;
	unsigned int    tmp_p, period_n;

	if (period_us == lpg->period_us)
		return;

	/* PWM Period / N */
	if (period_us < ((unsigned int)(-1) / NSEC_PER_USEC)) {
		period_n = (period_us * NSEC_PER_USEC) >> 6;
		n = 6;
	} else {
		period_n = (period_us >> 9) * NSEC_PER_USEC;
		n = 9;
	}

	min_err = last_err = (unsigned int)(-1);
	best_m = 0;
	best_clk = 0;
	best_div = 0;
	for (clk = 0; clk < NUM_PWM_CLK; clk++) {
		for (div = 0; div < NUM_PWM_PREDIV; div++) {
			/* period_n = (PWM Period / N) */
			/* tmp_p = (Pre-divide * Clock Period) * 2^m */
			tmp_p = lpg_clk_table[div][clk];
			for (m = 0; m <= NUM_EXP; m++) {
				if (period_n > tmp_p)
					cur_err = period_n - tmp_p;
				else
					cur_err = tmp_p - period_n;

				if (cur_err < min_err) {
					min_err = cur_err;
					best_m = m;
					best_clk = clk;
					best_div = div;
				}

				if (m && cur_err > last_err)
					/* Break for bigger cur_err */
					break;

				last_err = cur_err;
				tmp_p <<= 1;
			}
		}
	}

	/* Use higher resolution */
	if (best_m >= 3 && n == 6) {
		n += 3;
		best_m -= 3;
	}

	lpg->clk = best_clk;
	lpg->pre_div = best_div;
	lpg->pre_div_exp = best_m;
	lpg->pwm_size = n;

	lpg->period_us = period_us;
}

static void lpg_calc_duty(struct lpg *lpg, unsigned long duty_us)
{
	unsigned long max = (1 << lpg->pwm_size) - 1;
	unsigned long val;

	/* Figure out pwm_value with overflow handling */
	if (duty_us < 1 << (sizeof(val) * 8 - lpg->pwm_size))
		val = (duty_us << lpg->pwm_size) / lpg->period_us;
	else
		val = duty_us / (lpg->period_us >> lpg->pwm_size);

	if (val > max)
		val = max;

	lpg->pwm_value = val;
}

#define LPG_RESOLUTION_9BIT	BIT(4)

static void lpg_apply_freq(struct lpg *lpg)
{
	unsigned long val;

	if (!lpg->enabled)
		return;

	/* Clock register values are off-by-one from lpg_clk_table */
	val = lpg->clk + 1;

	if (lpg->pwm_size == 9)
		val |= LPG_RESOLUTION_9BIT;
	regmap_write(lpg->map, lpg->reg + LPG_SIZE_CLK_REG, val);

	val = lpg->pre_div << 5 | lpg->pre_div_exp;
	regmap_write(lpg->map, lpg->reg + LPG_PREDIV_CLK_REG, val);
}

#define LPG_ENABLE_GLITCH_REMOVAL	BIT(5)

static void lpg_enable_glitch(struct lpg *lpg)
{
	regmap_update_bits(lpg->map, lpg->reg + PWM_TYPE_CONFIG_REG,
			   LPG_ENABLE_GLITCH_REMOVAL, 0);
}

static void lpg_disable_glitch(struct lpg *lpg)
{
	regmap_update_bits(lpg->map, lpg->reg + PWM_TYPE_CONFIG_REG,
			   LPG_ENABLE_GLITCH_REMOVAL,
			   LPG_ENABLE_GLITCH_REMOVAL);
}

static void lpg_apply_pwm_value(struct lpg *lpg)
{
	u8 val[] = { lpg->pwm_value & 0xff, lpg->pwm_value >> 8 };

	if (!lpg->enabled)
		return;

	regmap_bulk_write(lpg->map, lpg->reg + PWM_VALUE_REG, val, 2);
}

#define LPG_PATTERN_CONFIG_LO_TO_HI	BIT(4)
#define LPG_PATTERN_CONFIG_REPEAT	BIT(3)
#define LPG_PATTERN_CONFIG_TOGGLE	BIT(2)
#define LPG_PATTERN_CONFIG_PAUSE_HI	BIT(1)
#define LPG_PATTERN_CONFIG_PAUSE_LO	BIT(0)

static void lpg_apply_lut_control(struct lpg *lpg)
{
	struct qcom_lpg_pattern *pattern = lpg->pattern;
	unsigned int hi_pause;
	unsigned int lo_pause;
	unsigned int step;
	unsigned int conf = 0;
	int pattern_len;

	if (!lpg->ramp_enabled || !pattern)
		return;

	pattern_len = pattern->hi_idx - pattern->lo_idx + 1;

	step = DIV_ROUND_UP(lpg->ramp_duration_ms, pattern_len);
	hi_pause = DIV_ROUND_UP(lpg->ramp_hi_pause_ms, step);
	lo_pause = DIV_ROUND_UP(lpg->ramp_lo_pause_ms, step);

	if (!lpg->ramp_reverse)
		conf |= LPG_PATTERN_CONFIG_LO_TO_HI;
	if (!lpg->ramp_oneshot)
		conf |= LPG_PATTERN_CONFIG_REPEAT;
	if (lpg->ramp_ping_pong)
		conf |= LPG_PATTERN_CONFIG_TOGGLE;
	if (lpg->ramp_hi_pause_ms)
		conf |= LPG_PATTERN_CONFIG_PAUSE_HI;
	if (lpg->ramp_lo_pause_ms)
		conf |= LPG_PATTERN_CONFIG_PAUSE_LO;

	regmap_write(lpg->map, lpg->reg + LPG_PATTERN_CONFIG_REG, conf);
	regmap_write(lpg->map, lpg->reg + LPG_HI_IDX_REG, pattern->hi_idx);
	regmap_write(lpg->map, lpg->reg + LPG_LO_IDX_REG, pattern->lo_idx);

	regmap_write(lpg->map, lpg->reg + LPG_RAMP_DURATION_REG, step);
	regmap_write(lpg->map, lpg->reg + LPG_HI_PAUSE_REG, hi_pause);
	regmap_write(lpg->map, lpg->reg + LPG_LO_PAUSE_REG, lo_pause);

	/* Trigger start of ramp generator(s) */
	qcom_lpg_lut_sync(lpg->lut);
}

#define LPG_ENABLE_CONTROL_OUTPUT		BIT(7)
#define LPG_ENABLE_CONTROL_BUFFER_TRISTATE	BIT(5)
#define LPG_ENABLE_CONTROL_SRC_PWM		BIT(2)
#define LPG_ENABLE_CONTROL_RAMP_GEN		BIT(1)

static void lpg_apply_control(struct lpg *lpg)
{
	unsigned int ctrl;

	ctrl = LPG_ENABLE_CONTROL_BUFFER_TRISTATE;

	if (lpg->enabled)
		ctrl |= LPG_ENABLE_CONTROL_OUTPUT;

	if (lpg->pattern)
		ctrl |= LPG_ENABLE_CONTROL_RAMP_GEN;
	else
		ctrl |= LPG_ENABLE_CONTROL_SRC_PWM;

	regmap_write(lpg->map, lpg->reg + PWM_ENABLE_CONTROL_REG, ctrl);

	/*
	 * Due to LPG hardware bug, in the PWM mode, having enabled PWM,
	 * We have to write PWM values one more time.
	 */
	if (lpg->enabled)
		lpg_apply_pwm_value(lpg);
}

#define LPG_SYNC_PWM	BIT(0)

static void lpg_apply_sync(struct lpg *lpg)
{
	regmap_write(lpg->map, lpg->reg + PWM_SYNC_REG, LPG_SYNC_PWM);
}

static void lpg_apply_dtest(struct lpg *lpg)
{
	if (!lpg->dtest_line)
		return;

	regmap_write(lpg->map, lpg->reg + PWM_SEC_ACCESS_REG, 0xa5);
	regmap_write(lpg->map, lpg->reg + PWM_DTEST_REG(lpg->dtest_line),
		     lpg->dtest_value);
}

static void lpg_apply(struct lpg *lpg)
{
	lpg_disable_glitch(lpg);
	lpg_apply_freq(lpg);
	lpg_apply_pwm_value(lpg);
	lpg_apply_control(lpg);
	lpg_apply_sync(lpg);
	lpg_apply_lut_control(lpg);
	lpg_enable_glitch(lpg);

	if (lpg->tri_led)
		qcom_tri_led_set(lpg->tri_led, lpg->enabled);
}

static void lpg_brightnes_set(struct led_classdev *cdev,
			      enum led_brightness value)
{
	struct lpg *lpg = container_of(cdev, struct lpg, cdev);
	unsigned int duty_us;

	if (value == LED_OFF) {
		lpg->enabled = false;
		lpg->ramp_enabled = false;
	} else if (lpg->pattern) {
		lpg_calc_freq(lpg, NSEC_PER_USEC);

		lpg->enabled = true;
		lpg->ramp_enabled = true;
	} else {
		lpg_calc_freq(lpg, NSEC_PER_USEC);

		duty_us = value * lpg->period_us / cdev->max_brightness;
		lpg_calc_duty(lpg, duty_us);
		lpg->enabled = true;
		lpg->ramp_enabled = false;
	}

	lpg_apply(lpg);
}

static int lpg_blink_set(struct led_classdev *cdev,
			 unsigned long *delay_on, unsigned long *delay_off)
{
	struct lpg *lpg = container_of(cdev, struct lpg, cdev);
	unsigned int period_us;
	unsigned int duty_us;

	if (!*delay_on && !*delay_off) {
		*delay_on = 500;
		*delay_off = 500;
	}

	duty_us = *delay_on * USEC_PER_MSEC;
	period_us = (*delay_on + *delay_off) * USEC_PER_MSEC;

	lpg_calc_freq(lpg, period_us);
	lpg_calc_duty(lpg, duty_us);

	lpg->enabled = true;
	lpg->ramp_enabled = false;

	lpg_apply(lpg);

	return 0;
}

static enum led_brightness lpg_brightnes_get(struct led_classdev *cdev)
{
	struct lpg *lpg = container_of(cdev, struct lpg, cdev);
	unsigned long max = (1 << lpg->pwm_size) - 1;

	if (!lpg->enabled)
		return LED_OFF;

	return lpg->pwm_value * cdev->max_brightness / max;
}

static int lpg_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 struct pwm_state *state)
{
	struct lpg *lpg = container_of(chip, struct lpg, chip);

	lpg_calc_freq(lpg, state->period / NSEC_PER_USEC);
	lpg_calc_duty(lpg, state->duty_cycle / NSEC_PER_USEC);
	lpg->enabled = state->enabled;

	lpg_apply(lpg);

	state->polarity = PWM_POLARITY_NORMAL;
	state->period = lpg->period_us * NSEC_PER_USEC;

	return 0;
}

static const struct pwm_ops lpg_pwm_ops = {
	.apply = lpg_pwm_apply,
	.owner = THIS_MODULE,
};

static ssize_t lpg_attr_get(struct device *dev,
			    struct device_attribute *attr,
			    char *buf);
static ssize_t lpg_attr_set(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count);

static DEVICE_ATTR(ping_pong,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(oneshot,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(reverse,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(pattern,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(duration,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(pause_lo,	0600, lpg_attr_get, lpg_attr_set);
static DEVICE_ATTR(pause_hi,	0600, lpg_attr_get, lpg_attr_set);

static ssize_t lpg_pattern_store(struct lpg *lpg, const char *buf, size_t count)
{
	struct qcom_lpg_pattern *new_pattern;
	unsigned long val;
	char *sbegin;
	u16 *pattern;
	char *elem;
	char *s;
	int len = 0;
	int ret = 0;

	s = sbegin = kstrndup(buf, count, GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	pattern = kcalloc(count, sizeof(u16), GFP_KERNEL);
	if (!pattern) {
		ret = -ENOMEM;
		goto out;
	}

	if (s[0] == '\0' || (s[0] == '\n' && s[1] == '\0')) {
		qcom_lpg_lut_free(lpg->pattern);
		lpg->pattern = NULL;
	} else {
		while ((elem = strsep(&s, " ,")) != NULL) {
			ret = kstrtoul(elem, 10, &val);
			if (ret)
				goto out;

			pattern[len++] = val;
		}

		new_pattern = qcom_lpg_lut_store(lpg->lut, pattern, len);
		if (IS_ERR(new_pattern)) {
			ret = PTR_ERR(new_pattern);
			goto out;
		}

		qcom_lpg_lut_free(lpg->pattern);
		lpg->pattern = new_pattern;
	}

out:
	kfree(pattern);
	kfree(sbegin);
	return ret < 0 ? ret : count;
}

static ssize_t lpg_attr_get(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct lpg *lpg = dev_get_drvdata(dev);

	if (attr == &dev_attr_ping_pong)
		return sprintf(buf, "%d\n", lpg->ramp_ping_pong);
	else if (attr == &dev_attr_oneshot)
		return sprintf(buf, "%d\n", lpg->ramp_oneshot);
	else if (attr == &dev_attr_reverse)
		return sprintf(buf, "%d\n", lpg->ramp_reverse);
	else if (attr == &dev_attr_duration)
		return sprintf(buf, "%ld\n", lpg->ramp_duration_ms);
	else if (attr == &dev_attr_pause_lo)
		return sprintf(buf, "%ld\n", lpg->ramp_lo_pause_ms);
	else if (attr == &dev_attr_pause_hi)
		return sprintf(buf, "%ld\n", lpg->ramp_hi_pause_ms);
	else if (attr == &dev_attr_pattern)
		return qcom_lpg_lut_show(lpg->pattern, buf);

	return -EINVAL;
}

static ssize_t lpg_attr_set(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct lpg *lpg = dev_get_drvdata(dev);
	int ret = -EINVAL;

	if (attr == &dev_attr_ping_pong)
		ret = strtobool(buf, &lpg->ramp_ping_pong);
	else if (attr == &dev_attr_oneshot)
		ret = strtobool(buf, &lpg->ramp_oneshot);
	else if (attr == &dev_attr_reverse)
		ret = strtobool(buf, &lpg->ramp_reverse);
	else if (attr == &dev_attr_duration)
		ret = kstrtoul(buf, 10, &lpg->ramp_duration_ms);
	else if (attr == &dev_attr_pause_lo)
		ret = kstrtoul(buf, 10, &lpg->ramp_lo_pause_ms);
	else if (attr == &dev_attr_pause_hi)
		ret = kstrtoul(buf, 10, &lpg->ramp_hi_pause_ms);
	else if (attr == &dev_attr_pattern)
		ret = lpg_pattern_store(lpg, buf, count);

	if (ret < 0)
		return -EINVAL;

	lpg_apply(lpg);
	return count;
}

static struct attribute *lpg_attributes[] = {
	&dev_attr_ping_pong.attr,
	&dev_attr_oneshot.attr,
	&dev_attr_reverse.attr,
	&dev_attr_pattern.attr,
	&dev_attr_duration.attr,
	&dev_attr_pause_lo.attr,
	&dev_attr_pause_hi.attr,
	NULL
};

static const struct attribute_group lpg_attr_group = {
	.attrs = lpg_attributes,
};

static const struct attribute_group *lpg_attr_groups[] = {
	&lpg_attr_group,
	NULL
};

static int lpg_register_pwm(struct lpg *lpg)
{
	int ret;

	lpg->chip.base = -1;
	lpg->chip.dev = lpg->dev;
	lpg->chip.npwm = 1;
	lpg->chip.ops = &lpg_pwm_ops;

	ret = pwmchip_add(&lpg->chip);
	if (ret)
		dev_err(lpg->dev, "failed to add PWM chip: ret %d\n", ret);

	return ret;
}

static int lpg_parse_lut(struct lpg *lpg)
{
	struct device_node *np = lpg->dev->of_node;
	u16 *pattern;
	u32 val;
	int len;

	lpg->lut = qcom_lpg_lut_get(lpg->dev);
	if (IS_ERR_OR_NULL(lpg->lut))
		return PTR_ERR(lpg->lut);

	if (!of_find_property(np, "qcom,pattern", NULL))
		return 0;

	len = of_property_count_elems_of_size(np, "qcom,pattern", sizeof(u16));
	if (len < 0)
		return -EINVAL;

	pattern = kcalloc(len, sizeof(u16), GFP_KERNEL);
	if (!pattern)
		return -ENOMEM;

	of_property_read_u16_array(np, "qcom,pattern", pattern, len);

	lpg->pattern = qcom_lpg_lut_store(lpg->lut, pattern, len);
	kfree(pattern);
	if (IS_ERR(lpg->pattern))
		return PTR_ERR(lpg->pattern);

	if (!of_property_read_u32(np, "qcom,pattern-length-ms", &val))
		lpg->ramp_duration_ms = val;
	if (!of_property_read_u32(np, "qcom,pattern-pause-lo-ms", &val))
		lpg->ramp_lo_pause_ms = val;
	if (!of_property_read_u32(np, "qcom,pattern-pause-hi-ms", &val))
		lpg->ramp_hi_pause_ms = val;

	lpg->ramp_ping_pong = of_property_read_bool(np, "qcom,pattern-ping-pong");
	lpg->ramp_oneshot = of_property_read_bool(np, "qcom,pattern-oneshot");
	lpg->ramp_reverse = of_property_read_bool(np, "qcom,pattern-reverse");

	return 0;
}

static int lpg_register_led(struct lpg *lpg)
{
	struct device_node *np = lpg->dev->of_node;
	const char *state;
	int ret;

	ret = lpg_parse_lut(lpg);
	if (ret)
		return ret;

	/* Use label else node name */
	lpg->cdev.name = of_get_property(np, "label", NULL) ? : np->name;
	lpg->cdev.default_trigger = of_get_property(np, "linux,default-trigger", NULL);
	lpg->cdev.brightness_set = lpg_brightnes_set;
	lpg->cdev.brightness_get = lpg_brightnes_get;
	lpg->cdev.blink_set = lpg_blink_set;
	lpg->cdev.max_brightness = 255;
	lpg->cdev.groups = lpg_attr_groups;

	if (!of_property_read_string(np, "default-state", &state) &&
	    !strcmp(state, "on"))
		lpg->cdev.brightness = LED_FULL;
	else
		lpg->cdev.brightness = LED_OFF;

	lpg_brightnes_set(&lpg->cdev, lpg->cdev.brightness);

	ret = devm_led_classdev_register(lpg->dev, &lpg->cdev);
	if (ret)
		dev_err(lpg->dev, "unable to register \"%s\"\n", lpg->cdev.name);

	return ret;
}

static int lpg_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct lpg *lpg;
	u32 dtest[2];
	int ret;

	lpg = devm_kzalloc(&pdev->dev, sizeof(*lpg), GFP_KERNEL);
	if (!lpg)
		return -ENOMEM;

	lpg->dev = &pdev->dev;

	lpg->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!lpg->map) {
		dev_err(&pdev->dev, "parent regmap unavailable\n");
		return -ENXIO;
	}

	ret = of_property_read_u32(np, "reg", &lpg->reg);
	if (ret) {
		dev_err(&pdev->dev, "no register offset specified\n");
		return -EINVAL;
	}

	if (!of_find_property(np, "#pwm-cells", NULL))
		lpg->is_lpg = true;

	lpg->tri_led = qcom_tri_led_get(&pdev->dev);
	if (IS_ERR(lpg->tri_led))
		return PTR_ERR(lpg->tri_led);

	ret = of_property_read_u32_array(np, "qcom,dtest", dtest, 2);
	if (!ret) {
		lpg->dtest_line = dtest[0];
		lpg->dtest_value = dtest[1];
	}

	if (lpg->is_lpg) {
		ret = lpg_register_led(lpg);
		if (ret)
			return ret;
	} else {
		ret = lpg_register_pwm(lpg);
		if (ret)
			return ret;
	}

	lpg_apply_dtest(lpg);

	platform_set_drvdata(pdev, lpg);

	return 0;
}

static int lpg_remove(struct platform_device *pdev)
{
	struct lpg *lpg = platform_get_drvdata(pdev);

	if (!lpg->is_lpg)
		pwmchip_remove(&lpg->chip);

	qcom_lpg_lut_free(lpg->pattern);

	return 0;
}

static const struct of_device_id lpg_of_table[] = {
	{ .compatible = "qcom,spmi-lpg" },
	{},
};
MODULE_DEVICE_TABLE(of, lpg_of_table);

static struct platform_driver lpg_driver = {
	.probe = lpg_probe,
	.remove = lpg_remove,
	.driver = {
		.name = "qcom-spmi-lpg",
		.of_match_table = lpg_of_table,
	},
};
module_platform_driver(lpg_driver);

MODULE_DESCRIPTION("Qualcomm TRI LED driver");
MODULE_LICENSE("GPL v2");
