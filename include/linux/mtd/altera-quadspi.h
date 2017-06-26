/*
 *
 * Copyright 2017 Intel Corporation, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __ALTERA_QUADSPI_H
#define __ALTERA_QUADSPI_H

#include <linux/device.h>

#define ALTERA_QUADSPI_FL_BITREV_READ BIT(0)
#define ALTERA_QUADSPI_FL_BITREV_WRITE BIT(1)

#define ALTERA_QUADSPI_MAX_NUM_FLASH_CHIP 3

int altera_quadspi_create(struct device *dev, void __iomem *csr_base,
			  void __iomem *data_base, void __iomem *window_reg,
			  size_t window_size, u32 flags);

int altera_qspi_add_bank(struct device *dev,
			 u32 bank, struct device_node *np);

int altera_quadspi_remove_banks(struct device *dev);
#endif
