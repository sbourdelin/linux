/*
 * Purna Chandra Mandal,<purna.mandal@microchip.com>
 * Copyright (C) 2015 Microchip Technology Inc.  All rights reserved.
 *
 * This program is free software; you can distribute it and/or modify it
 * under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#ifndef __MICROCHIP_CLK_PIC32_H_
#define __MICROCHIP_CLK_PIC32_H_

struct clk_hw;

/* System PLL clock */
struct pic32_sys_pll {
	struct clk_hw hw;
	void __iomem *ctrl_reg;
	void __iomem *status_reg;
	u32 lock_mask;
	u32 idiv; /* PLL iclk divider, treated fixed */
};

/* System clock */
struct pic32_sys_clk {
	struct clk_hw hw;
	void __iomem *mux_reg;
	void __iomem *slew_reg;
	const u32 *parent_map;
	const u32 slew_div;
};

/* Reference Oscillator clock */
struct pic32_ref_osc {
	struct clk_hw hw;
	void __iomem *regs;
	const u32 *parent_map;
};

/* Peripheral Bus clock */
struct pic32_periph_clk {
	struct clk_hw hw;
	void __iomem *ctrl_reg;
};

/* External Secondary Oscillator clock  */
struct pic32_sec_osc {
	struct clk_hw hw;
	void __iomem *enable_reg;
	void __iomem *status_reg;
	u32 enable_bitmask;
	u32 status_bitmask;
	unsigned long fixed_rate;
};

extern const struct clk_ops pic32_pbclk_ops;
extern const struct clk_ops pic32_sclk_ops;
extern const struct clk_ops pic32_sclk_no_div_ops;
extern const struct clk_ops pic32_spll_ops;
extern const struct clk_ops pic32_roclk_ops;
extern const struct clk_ops pic32_sosc_ops;

struct clk *pic32_periph_clk_register(struct pic32_periph_clk *pbclk,
				      void __iomem *clk_iobase);
struct clk *pic32_refo_clk_register(struct pic32_ref_osc *refo,
				    void __iomem *clk_iobase);
struct clk *pic32_sys_clk_register(struct pic32_sys_clk *sclk,
				   void __iomem *clk_iobase);
struct clk *pic32_spll_clk_register(struct pic32_sys_pll *spll,
				    void __iomem *clk_iobase);
struct clk *pic32_sosc_clk_register(struct pic32_sec_osc *sosc,
				    void __iomem *clk_iobase);

#endif /* __MICROCHIP_CLK_PIC32_H_*/
