/*
 * Copyright (C) 2016 Socionext Inc.
 *   Author: Masahiro Yamada <yamada.masahiro@socionext.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>

#include "clk-uniphier.h"

#define UNIPHIER_MIO_CLK_SD_FIXED					\
	{								\
		.name = "sd-44m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-133m",			\
			.mult = 1,					\
			.div = 3,					\
		},							\
	},								\
	{								\
		.name = "sd-33m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 6,					\
		},							\
	},								\
	{								\
		.name = "sd-50m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 4,					\
		},							\
	},								\
	{								\
		.name = "sd-67m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 3,					\
		},							\
	},								\
	{								\
		.name = "sd-100m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 2,					\
		},							\
	},								\
	{								\
		.name = "sd-40m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 5,					\
		},							\
	},								\
	{								\
		.name = "sd-25m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-200m",			\
			.mult = 1,					\
			.div = 8,					\
		},							\
	},								\
	{								\
		.name = "sd-22m",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,			\
		.output_index = -1,					\
		.data.factor = {					\
			.parent_name = "sd-133m",			\
			.mult = 1,					\
			.div = 6,					\
		},							\
	}

#define UNIPHIER_MIO_CLK_SD(index, ch)					\
	{								\
		.name = "sd" #ch "-sel",				\
		.type = UNIPHIER_CLK_TYPE_MUX,				\
		.output_index = -1,					\
		.data.mux = {						\
			.parent_names = {				\
				"sd-44m",				\
				"sd-33m",				\
				"sd-50m",				\
				"sd-67m",				\
				"sd-100m",				\
				"sd-40m",				\
				"sd-25m",				\
				"sd-22m",				\
			},						\
			.num_parents = 8,				\
			.reg = 0x30 + 0x200 * ch,			\
			.masks = {					\
				0x00031000,				\
				0x00031000,				\
				0x00031000,				\
				0x00031000,				\
				0x00001300,				\
				0x00001300,				\
				0x00001300,				\
				0x00001300,				\
			},						\
			.vals = {					\
				0x00000000,				\
				0x00010000,				\
				0x00020000,				\
				0x00030000,				\
				0x00001000,				\
				0x00001100,				\
				0x00001200,				\
				0x00001300,				\
			},						\
		},							\
	},								\
	{								\
		.name = "sd" #ch,					\
		.type = UNIPHIER_CLK_TYPE_GATE,				\
		.output_index = (index),				\
		.data.gate = {						\
			.parent_name = "sd" #ch "-sel",			\
			.reg = 0x20 + 0x200 * ch,			\
			.mask = BIT(8),					\
		},							\
	}

#define UNIPHIER_MIO_CLK_USB2(index, ch)				\
	{								\
		.name = "usb2" #ch,					\
		.type = UNIPHIER_CLK_TYPE_GATE,				\
		.output_index = (index),				\
		.data.gate = {						\
			.parent_name = "usb2",				\
			.reg = 0x20 + 0x200 * ch,			\
			.mask = BIT(29) | BIT(28),			\
		},							\
	}

#define UNIPHIER_MIO_CLK_DMAC(index)					\
	{								\
		.name = "miodmac",					\
		.type = UNIPHIER_CLK_TYPE_GATE,				\
		.output_index = (index),				\
		.data.gate = {						\
			.parent_name = "stdmac",			\
			.reg = 0x20,					\
			.mask = BIT(25),				\
		},							\
	}

const struct uniphier_clk_data uniphier_sld3_mio_clk_data[] = {
	UNIPHIER_MIO_CLK_SD_FIXED,
	UNIPHIER_MIO_CLK_SD(0, 0),
	UNIPHIER_MIO_CLK_SD(1, 1),
	UNIPHIER_MIO_CLK_SD(2, 2),
	UNIPHIER_MIO_CLK_DMAC(3),
	UNIPHIER_MIO_CLK_USB2(4, 0),
	UNIPHIER_MIO_CLK_USB2(5, 1),
	UNIPHIER_MIO_CLK_USB2(6, 2),
	UNIPHIER_MIO_CLK_USB2(7, 3),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_pro5_mio_clk_data[] = {
	UNIPHIER_MIO_CLK_SD_FIXED,
	UNIPHIER_MIO_CLK_SD(0, 0),
	UNIPHIER_MIO_CLK_SD(1, 1),
	{ /* sentinel */ }
};
