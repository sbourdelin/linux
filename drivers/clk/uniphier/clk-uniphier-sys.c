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

#define UNIPHIER_SLD3_SYS_CLK_SD				\
	{							\
		.name = "sd-200m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 8,				\
		},						\
	},							\
	{							\
		.name = "sd-133m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "vpll27a",		\
			.mult = 1,				\
			.div = 2,				\
		},						\
	}

#define UNIPHIER_PRO5_SYS_CLK_SD				\
	{							\
		.name = "sd-200m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 12,				\
		},						\
	},							\
	{							\
		.name = "sd-133m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 18,				\
		},						\
	}

#define UNIPHIER_LD20_SYS_CLK_SD				\
	{							\
		.name = "sd-200m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 10,				\
		},						\
	},							\
	{							\
		.name = "sd-133m",				\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 15,				\
		},						\
	}

#define UNIPHIER_PRO5_SYS_CLK_I2C				\
	{							\
		.name = "i2c",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 48,				\
		},						\
	}

#define UNIPHIER_LD11_SYS_CLK_I2C				\
	{							\
		.name = "i2c",					\
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,		\
		.output_index = -1,				\
		.data.factor = {				\
			.parent_name = "spll",			\
			.mult = 1,				\
			.div = 40,				\
		},						\
	}

#define UNIPHIER_SLD3_SYS_CLK_STDMAC(index)			\
	{							\
		.name = "stdmac",				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = NULL,			\
			.reg = 0x2104,				\
			.mask = BIT(10),			\
		},						\
	}

#define UNIPHIER_LD11_SYS_CLK_STDMAC(index)			\
	{							\
		.name = "stdmac",				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = NULL,			\
			.reg = 0x210c,				\
			.mask = BIT(8),				\
		},						\
	}

#define UNIPHIER_PRO4_SYS_CLK_GIO(index)			\
	{							\
		.name = "gio",					\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = NULL,			\
			.reg = 0x2104,				\
			.mask = BIT(6),				\
		},						\
	}

#define UNIPHIER_PRO4_SYS_CLK_USB3(index, ch)			\
	{							\
		.name = "usb3" #ch,				\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = NULL,			\
			.reg = 0x2104,				\
			.mask = BIT(16 + (ch)),			\
		},						\
	}

#define UNIPHIER_PXS2_SYS_CLK_USB3PHY(index, ch)		\
	{							\
		.name = "usb3" #ch "phy",			\
		.type = UNIPHIER_CLK_TYPE_GATE,			\
		.output_index = (index),			\
		.data.gate = {					\
			.parent_name = NULL,			\
			.reg = 0x2104,				\
			.mask = BIT(19 + (ch)),			\
		},						\
	}

const struct uniphier_clk_data uniphier_sld3_sys_clk_data[] = {
	{
		.name = "spll",		/* 1597.44 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 65,
			.div = 1,
		},
	},
	{
		.name = "upll",		/* 288 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 288000,
			.div = 24576,
		},
	},
	{
		.name = "a2pll",	/* 589.824 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 24,
			.div = 1,
		},
	},
	{
		.name = "vpll27a",	/* 270 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 270000,
			.div = 24576,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = 0,
		.data.factor = {
			.parent_name = "a2pll",
			.mult = 1,
			.div = 16,
		},
	},
	{
		.name = "i2c",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = 1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 16,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_SD,
	{
		.name = "usb2",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "upll",
			.mult = 1,
			.div = 12,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_ld4_sys_clk_data[] = {
	{
		.name = "spll",		/* 1597.44 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 65,
			.div = 1,
		},
	},
	{
		.name = "upll",		/* 288 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 288000,
			.div = 24576,
		},
	},
	{
		.name = "a2pll",	/* 589.824 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 24,
			.div = 1,
		},
	},
	{
		.name = "vpll27a",	/* 270 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 270000,
			.div = 24576,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "a2pll",
			.mult = 1,
			.div = 16,
		},
	},
	{
		.name = "i2c",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 16,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_SD,
	{
		.name = "usb2",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "upll",
			.mult = 1,
			.div = 12,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),	/* Ether, HSC, MIO */
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_pro4_sys_clk_data[] = {
	{
		.name = "spll",		/* 1600 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 64,
			.div = 1,
		},
	},
	{
		.name = "upll",		/* 288 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 288,
			.div = 25,
		},
	},
	{
		.name = "a2pll",	/* 589.824 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "upll",
			.mult = 256,
			.div = 125,
		},
	},
	{
		.name = "vpll27a",	/* 270 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 270,
			.div = 25,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "a2pll",
			.mult = 1,
			.div = 8,
		},
	},
	{
		.name = "i2c",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 32,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_SD,
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),	/* HSC, MIO, RLE */
	UNIPHIER_PRO4_SYS_CLK_GIO(12),		/* Ether, SATA, USB3 */
	UNIPHIER_PRO4_SYS_CLK_USB3(16, 0),
	UNIPHIER_PRO4_SYS_CLK_USB3(17, 1),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_sld8_sys_clk_data[] = {
	{
		.name = "spll",		/* 1600 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 64,
			.div = 1,
		},
	},
	{
		.name = "upll",		/* 288 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 288,
			.div = 25,
		},
	},
	{
		.name = "vpll27a",	/* 270 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 270,
			.div = 25,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 20,
		},
	},
	{
		.name = "i2c",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 16,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_SD,
	{
		.name = "usb2",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "upll",
			.mult = 1,
			.div = 12,
		},
	},
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),	/* Ether, HSC, MIO */
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_pro5_sys_clk_data[] = {
	{
		.name = "spll",		/* 2400 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 120,
			.div = 1,
		},
	},
	{
		.name = "dapll1",	/* 2560 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 128,
			.div = 1,
		},
	},
	{
		.name = "dapll2",	/* 2949.12 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "dapll1",
			.mult = 144,
			.div = 125,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "dapll2",
			.mult = 1,
			.div = 40,
		},
	},
	UNIPHIER_PRO5_SYS_CLK_I2C,
	UNIPHIER_PRO5_SYS_CLK_SD,
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),	/* HSC */
	UNIPHIER_PRO4_SYS_CLK_GIO(12),		/* PCIe, USB3 */
	UNIPHIER_PRO4_SYS_CLK_USB3(16, 0),
	UNIPHIER_PRO4_SYS_CLK_USB3(17, 1),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_pxs2_sys_clk_data[] = {
	{
		.name = "spll",		/* 2400 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 96,
			.div = 1,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 27,
		},
	},
	UNIPHIER_PRO5_SYS_CLK_I2C,
	UNIPHIER_PRO5_SYS_CLK_SD,
	UNIPHIER_SLD3_SYS_CLK_STDMAC(8),	/* HSC, RLE */
	/* GIO is always clock-enabled: no function for 0x2104 bit6 */
	UNIPHIER_PRO4_SYS_CLK_USB3(16, 0),
	UNIPHIER_PRO4_SYS_CLK_USB3(17, 1),
	/* The document mentions 0x2104 bit 18, but not functional */
	UNIPHIER_PXS2_SYS_CLK_USB3PHY(18, 0),
	UNIPHIER_PXS2_SYS_CLK_USB3PHY(19, 1),
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_ld11_sys_clk_data[] = {
	{
		.name = "spll",		/* 2000 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 80,
			.div = 1,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 34,
		},
	},
	UNIPHIER_LD11_SYS_CLK_I2C,
	UNIPHIER_LD11_SYS_CLK_STDMAC(8),	/* HSC, MIO */
	{
		.name = "usb2",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 24,
			.div = 25,
		},
	},
	{ /* sentinel */ }
};

const struct uniphier_clk_data uniphier_ld20_sys_clk_data[] = {
	{
		.name = "spll",		/* 2000 MHz */
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "ref",
			.mult = 80,
			.div = 1,
		},
	},
	{
		.name = "uart",
		.type = UNIPHIER_CLK_TYPE_FIXED_FACTOR,
		.output_index = -1,
		.data.factor = {
			.parent_name = "spll",
			.mult = 1,
			.div = 34,
		},
	},
	UNIPHIER_LD11_SYS_CLK_I2C,
	UNIPHIER_LD20_SYS_CLK_SD,
	UNIPHIER_LD11_SYS_CLK_STDMAC(8),	/* HSC */
	/* GIO is always clock-enabled: no function for 0x210c bit5 */
	{
		.name = "usb30",
		.type = UNIPHIER_CLK_TYPE_GATE,
		.output_index = 16,
		.data.gate = {
			.parent_name = NULL,
			.reg = 0x210c,
			/*
			 * clock for USB Link is enabled by the logic "OR"
			 * of bit 14 and bit 15.  We do not use bit 15 here.
			 */
			.mask = BIT(14),
		},
	},
	{
		.name = "usb30phy",
		.type = UNIPHIER_CLK_TYPE_GATE,
		.output_index = 18,
		.data.gate = {
			.parent_name = NULL,
			.reg = 0x210c,
			.mask = BIT(12) | BIT(13),
		},
	},
	{ /* sentinel */ }
};
