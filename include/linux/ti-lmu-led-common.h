// SPDX-License-Identifier: GPL-2.0
// TI LMU Common Core
// Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/

#ifndef _TI_LMU_COMMON_H_
#define _TI_LMU_COMMON_H_

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <uapi/linux/uleds.h>

#define LMU_DUAL_CHANNEL_USED	(BIT(0) | BIT(1))
#define LMU_11BIT_LSB_MASK	(BIT(0) | BIT(1) | BIT(2))
#define LMU_11BIT_MSB_SHIFT	3

#define MAX_BRIGHTNESS_8BIT	255
#define MAX_BRIGHTNESS_11BIT	2047

#define NUM_DUAL_CHANNEL	2

struct ti_lmu_bank {
	struct regmap *regmap;

	int bank_id;
	int fault_monitor_used;

	u8 enable_reg;
	unsigned long enable_usec;

	int current_brightness;
	u32 default_brightness;
	int max_brightness;

	u8 lsb_brightness_reg;
	u8 msb_brightness_reg;

	u8 runtime_ramp_reg;
	u32 ramp_up_msec;
	u32 ramp_down_msec;
};


int ti_lmu_common_set_brightness(struct ti_lmu_bank *lmu_bank,
				    int brightness);

int ti_lmu_common_set_ramp(struct ti_lmu_bank *lmu_bank);

int ti_lmu_common_get_ramp_params(struct device *dev,
				  struct fwnode_handle *child,
				  struct ti_lmu_bank *lmu_data);

#endif /* _TI_LMU_COMMON_H_ */
