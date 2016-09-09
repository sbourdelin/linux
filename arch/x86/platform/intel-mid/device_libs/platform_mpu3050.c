/*
 * platform_mpu3050.c: mpu3050 platform data initialization file
 *
 * (C) Copyright 2013 Intel Corporation
 * Author: Sathyanarayanan Kuppuswamy <sathyanarayanan.kuppuswamy@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <asm/intel-mid.h>

#define MPU3050_INT		"mpu3050_int"

static void *mpu3050_platform_data(void *info)
{
	struct i2c_board_info *i2c_info = info;
	int intr = get_gpio_by_name(MPU3050_INT);

	if (intr < 0) {
		pr_err("%s: Can't find %s GPIO interrupt\n", __func__,
		       MPU3050_INT);
		return ERR_PTR(intr);
	}

	i2c_info->irq = intr + INTEL_MID_IRQ_OFFSET;
	return NULL;
}

static const struct devs_id mpu3050_dev_id __initconst = {
	.name = "mpu3050",
	.type = SFI_DEV_TYPE_I2C,
	.delay = 1,
	.get_platform_data = &mpu3050_platform_data,
};

sfi_device(mpu3050_dev_id);
