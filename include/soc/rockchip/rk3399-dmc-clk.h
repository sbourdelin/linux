/*
* Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*/

#ifndef _RK3399_DMC_CLK_H
#define _RK3399_DMC_CLK_H

enum dram_type_tag {
	DDR3 = 6,
	LPDDR3 = 7,
	LPDDR4 = 0x0b,
};

/* DENALI_CTL_00 */
#define DENALI_CTL_00		0x00
#define DRAM_CLASS_MASK		0x0f
#define DRAM_CLASS_SHIFT	0x8

struct rk3399_dmcclk {
	struct device *dev;
	struct clk_hw *hw;
	u32 cur_freq;
	u32 target_freq;
	u32 ddr_type;
	void __iomem *ctrl_regs;
	void __iomem *dfi_regs;
	void __iomem *cru;
};

#endif

