/*
 * This header provides constants for Tegra210 IO pads pinctrl bindings.
 *
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _DT_BINDINGS_PINCTRL_TEGRA210_IO_PAD_H
#define _DT_BINDINGS_PINCTRL_TEGRA210_IO_PAD_H

/* Voltage levels of Tegra210 IO rails. */
#define TEGRA210_IO_RAIL_1800000UV		0
#define TEGRA210_IO_RAIL_3300000UV		1

/* Deep power down state enable/disable for Tegra210 IO pads */
#define TEGRA210_IO_PAD_DEEP_POWER_DOWN_DISABLE	0
#define TEGRA210_IO_PAD_DEEP_POWER_DOWN_ENABLE	1

#endif
