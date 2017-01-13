/*
 * Intel Atom platform clocks for BayTrail and CherryTrail SoC.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Irina Tirdea <irina.tirdea@intel.com>
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

#ifndef __CLK_BYT_PLT_H
#define __CLK_BYT_PLT_H

/**
 * struct pmc_clk - Struct to configure PMC platform clocks
 *
 * @name:	identified, typically pmc_plt_clk_%d
 * @freq:	in Hz, 19.2 and 25MHz (Baytrail only) supported
 * @parent_name:xtal or osc
 */
struct pmc_clk {
	const char *name;
	unsigned long freq;
	const char *parent_name;
};

/**
 * struct pmc_clk_data - Struct for PMC clock-related platform data
 *
 * @base:	PMC clock register base offset
 * @clks:	pointer to set of registered clocks, typically 1..6
 */
struct pmc_clk_data {
	void __iomem *base;
	const struct pmc_clk *clks;
};

#endif /* __CLK_BYT_PLT_H */
