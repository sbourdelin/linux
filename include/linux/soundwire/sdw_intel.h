/*
 * Soundwire Intel Shim Driver
 * Copyright (c) 2016-17, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __SDW_INTEL_H
#define __SDW_INTEL_H

#include <linux/acpi.h>

/**
 * struct sdw_config_ops: callback ops for shim to callback the audio driver
 * for any configuration required
 *
 * @config_stream: configure the stream with the given hw_params
 */
struct sdw_config_ops {
	int (*config_stream)(void *substream, void *dai, void *hw_params);
};

/**
 * struct intel_sdw_res: Soundwire Intel resource structure
 *
 * @mmio_base: mmio base of soundwire registers
 * @irq: interrupt number
 * @parent: parent device
 * @config_ops: callback ops
 */
struct intel_sdw_res {
	void __iomem *mmio_base;
	int irq;
	struct device *parent;
	const struct sdw_config_ops *config_ops;
};

void *intel_sdw_init(acpi_handle *parent_handle, struct intel_sdw_res *res);
void intel_sdw_exit(void *arg);

#endif
