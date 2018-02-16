/*
 * TI DaVinci AEMIF platform glue.
 *
 * Copyright (C) 2017 BayLibre SAS
 *
 * Author:
 *   Bartosz Golaszewski <bgolaszewski@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __TI_DAVINCI_AEMIF_DATA_H__
#define __TI_DAVINCI_AEMIF_DATA_H__

#include <linux/of_platform.h>

struct aemif_abus_data {
	u32 cs;
};

struct aemif_platform_data {
	struct of_dev_auxdata *dev_lookup;
	u32 cs_offset;
	struct aemif_abus_data *abus_data;
	size_t num_abus_data;
	struct platform_device *sub_devices;
	size_t num_sub_devices;
};

#endif /* __TI_DAVINCI_AEMIF_DATA_H__ */
