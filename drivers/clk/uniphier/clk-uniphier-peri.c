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

#define UNIPHIER_PERI_CLK_UART(index, ch)			\
	{							\
		.name = "uart" #ch,				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = "uart",			\
			.reg = 0x24,				\
			.mask = BIT(19 + (ch)),			\
		},						\
	}

#define UNIPHIER_PERI_CLK_I2C_COMMON				\
	{							\
		.name = "i2c-common",				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = -1,				\
		.data.gate = {					\
			.parent_name = "i2c",			\
			.reg = 0x20,				\
			.mask = BIT(1),				\
		},						\
	}

#define UNIPHIER_PERI_CLK_I2C(index, ch)			\
	{							\
		.name = "i2c" #ch,				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = "i2c-common",		\
			.reg = 0x24,				\
			.mask = BIT(5 + (ch)),			\
		},						\
	}

#define UNIPHIER_PERI_CLK_FI2C(index, ch)			\
	{							\
		.name = "i2c" #ch,				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = "i2c",			\
			.reg = 0x24,				\
			.mask = BIT(24 + (ch)),			\
		},						\
	}

const struct uniphier_clk_data uniphier_ld4_peri_clk_data[] = {
	UNIPHIER_PERI_CLK_UART(0, 0),
	UNIPHIER_PERI_CLK_UART(1, 1),
	UNIPHIER_PERI_CLK_UART(2, 2),
	UNIPHIER_PERI_CLK_UART(3, 3),
	UNIPHIER_PERI_CLK_I2C_COMMON,
	UNIPHIER_PERI_CLK_I2C(4, 0),
	UNIPHIER_PERI_CLK_I2C(5, 1),
	UNIPHIER_PERI_CLK_I2C(6, 2),
	UNIPHIER_PERI_CLK_I2C(7, 3),
	UNIPHIER_PERI_CLK_I2C(8, 4),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_pro4_peri_clk_data[] = {
	UNIPHIER_PERI_CLK_UART(0, 0),
	UNIPHIER_PERI_CLK_UART(1, 1),
	UNIPHIER_PERI_CLK_UART(2, 2),
	UNIPHIER_PERI_CLK_UART(3, 3),
	UNIPHIER_PERI_CLK_FI2C(4, 0),
	UNIPHIER_PERI_CLK_FI2C(5, 1),
	UNIPHIER_PERI_CLK_FI2C(6, 2),
	UNIPHIER_PERI_CLK_FI2C(7, 3),
	UNIPHIER_PERI_CLK_FI2C(8, 4),
	UNIPHIER_PERI_CLK_FI2C(9, 5),
	UNIPHIER_PERI_CLK_FI2C(10, 6),
	{ /* sentinel */ }
};
