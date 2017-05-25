/*
 * ACPI support for indirect-PIO bus.
 *
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Gabriele Paoloni <gabriele.paoloni@huawei.com>
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _ACPI_INDIRECT_PIO_H
#define _ACPI_INDIRECT_PIO_H

struct indirect_pio_device_desc {
	void *pdata; /* device relevant info data */
	int (*pre_setup)(struct acpi_device *adev, void *pdata);
};

#ifdef CONFIG_HISILICON_LPC
extern const struct indirect_pio_device_desc lpc_host_desc;
#endif

int acpi_set_logic_pio_resource(struct device *child,
		struct device *hostdev);

#endif
