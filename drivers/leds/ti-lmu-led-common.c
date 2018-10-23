// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2015 Texas Instruments
 * Copyright 2018 Sebastian Reichel
 * Copyright 2018 Pavel Machek <pavel@ucw.cz>
 *
 * TI LMU Led driver, based on previous work from
 * Milo Kim <milo.kim@ti.com>
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/notifier.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <linux/ti-lmu-led-common.h>

const static int ramp_table[16] = { 2, 262, 524, 1049, 2090, 4194, 8389,
				16780, 33550, 41940, 50330, 58720,
				67110, 83880, 100660, 117440};

static int ti_lmu_common_update_brightness_register(struct ti_lmu_bank *lmu_bank,
						       int brightness)
{
	struct regmap *regmap = lmu_bank->regmap;
	u8 reg, val;
	int ret;

	/*
	 * Brightness register update
	 *
	 * 11 bit dimming: update LSB bits and write MSB byte.
	 *		   MSB brightness should be shifted.
	 *  8 bit dimming: write MSB byte.
	 */
	if (lmu_bank->max_brightness == MAX_BRIGHTNESS_11BIT) {
		reg = lmu_bank->lsb_brightness_reg;
		ret = regmap_update_bits(regmap, reg,
					 LMU_11BIT_LSB_MASK,
					 brightness);
		if (ret)
			return ret;

		val = brightness >> LMU_11BIT_MSB_SHIFT;
	} else {
		val = brightness;
	}

	reg = lmu_bank->msb_brightness_reg;

	return regmap_write(regmap, reg, val);
}

int ti_lmu_common_set_brightness(struct ti_lmu_bank *lmu_bank,
				    int brightness)
{
	lmu_bank->current_brightness = brightness;

	return ti_lmu_common_update_brightness_register(lmu_bank, brightness);
}
EXPORT_SYMBOL(ti_lmu_common_set_brightness);

static int ti_lmu_common_convert_ramp_to_index(unsigned int msec)
{
	int size = ARRAY_SIZE(ramp_table);
	int i;

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

int ti_lmu_common_set_ramp(struct ti_lmu_bank *lmu_bank)
{
	struct regmap *regmap = lmu_bank->regmap;
	u8 ramp, ramp_up, ramp_down;

	if (lmu_bank->ramp_up_msec == 0 && lmu_bank->ramp_down_msec == 0) {
		ramp_up = 0;
		ramp_down = 0;
	} else {
		ramp_up = ti_lmu_common_convert_ramp_to_index(lmu_bank->ramp_up_msec);
		ramp_down = ti_lmu_common_convert_ramp_to_index(lmu_bank->ramp_down_msec);
	}

	if (ramp_up < 0 || ramp_down < 0)
		return -EINVAL;

	ramp = (ramp_up << 4) | ramp_down;

	return regmap_write(regmap, lmu_bank->runtime_ramp_reg, ramp);

}
EXPORT_SYMBOL(ti_lmu_common_set_ramp);

int ti_lmu_common_get_ramp_params(struct device *dev,
				  struct fwnode_handle *child,
				  struct ti_lmu_bank *lmu_data)
{
	int ret;

	ret = fwnode_property_read_u32(child, "ramp-up-ms",
				 &lmu_data->ramp_up_msec);
	if (ret)
		dev_warn(dev, "ramp-up-ms property missing\n");


	ret = fwnode_property_read_u32(child, "ramp-down-ms",
				 &lmu_data->ramp_down_msec);
	if (ret)
		dev_warn(dev, "ramp-down-ms property missing\n");

	return 0;
}
EXPORT_SYMBOL(ti_lmu_common_get_ramp_params);

MODULE_DESCRIPTION("TI LMU LED Driver");
MODULE_AUTHOR("Sebastian Reichel");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:ti-lmu-led");
