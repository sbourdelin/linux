/*
 * platform_emc1403.c: emc1403 platform data initialization file
 *
 * (C) Copyright 2013 Intel Corporation
 * Author: Sathyanarayanan Kuppuswamy <sathyanarayanan.kuppuswamy@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <asm/intel-mid.h>

#define EMC1403_THERMAL_INT		"thermal_int"
#define EMC1403_THERMAL_ALERT_INT	"thermal_alert"

static void __init *emc1403_platform_data(void *info)
{
	static short intr2nd_pdata;
	struct i2c_board_info *i2c_info = info;
	int intr = get_gpio_by_name(EMC1403_THERMAL_INT);
	int intr2nd = get_gpio_by_name(EMC1403_THERMAL_ALERT_INT);

	if (intr < 0) {
		pr_err("%s: Can't find %s GPIO interrupt\n", __func__,
		       EMC1403_THERMAL_INT);
		return NULL;
	}

	if (intr2nd < 0) {
		pr_err("%s: Can't find %s GPIO interrupt\n", __func__,
		       EMC1403_THERMAL_ALERT_INT);
		return NULL;
	}

	i2c_info->irq = intr + INTEL_MID_IRQ_OFFSET;
	intr2nd_pdata = intr2nd + INTEL_MID_IRQ_OFFSET;

	return &intr2nd_pdata;
}

static const struct devs_id emc1403_dev_id __initconst = {
	.name = "emc1403",
	.type = SFI_DEV_TYPE_I2C,
	.delay = 1,
	.get_platform_data = &emc1403_platform_data,
};

sfi_device(emc1403_dev_id);
