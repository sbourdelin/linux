/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 */

#ifndef __QCOM_PDC_H
#define __QCOM_PDC_H

#include <linux/kernel.h>
#include <linux/mod_devicetable.h>

struct pdc_gpio_pin_map {
	unsigned int gpio;
	u32 pdc_pin;
};

struct pdc_gpio_pin_data {
	size_t size;
	const struct pdc_gpio_pin_map *map;
};

extern const struct of_device_id pdc_gpio_match_table[];

#endif /* __QCOM_PDC_H */
