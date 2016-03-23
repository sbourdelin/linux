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
#include <dt-bindings/clock/microchip,pic32-clock.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <asm/traps.h>

#include "clk-core.h"

static void __iomem *pic32_clk_iobase;
static DEFINE_SPINLOCK(lock);

/* FRC Postscaler */
#define OSC_FRCDIV_MASK		0x07
#define OSC_FRCDIV_SHIFT	24

/* SPLL fields */
#define PLL_ICLK_MASK		0x01
#define PLL_ICLK_SHIFT		7

#define DECLARE_PERIPHERAL_CLOCK(__clk_name, __reg, __flags)		\
	{								\
		.ctrl_reg = (void __iomem *)(__reg),			\
		.hw.init = &(struct clk_init_data){			\
			.name = (__clk_name),				\
			.parent_names = (const char *[]){ "sys_clk" },	\
			.num_parents = 1,				\
			.ops = &pic32_pbclk_ops,			\
			.flags = (__flags),				\
		},							\
	}

#define DECLARE_REFO_CLOCK(__clkid, __regs)				\
	{								\
		.regs = (void __iomem *)(__regs),			\
		.hw.init = &(struct clk_init_data) {			\
			.name = "refo" #__clkid "_clk",			\
			.parent_names = (const char *[]) {		\
				"sys_clk", "pb1_clk", "posc_clk",	\
				"frc_clk", "lprc_clk", "sosc_clk",	\
				"sys_pll", "refi" #__clkid "_clk",	\
				"bfrc_clk",				\
			},						\
			.num_parents = 9,				\
			.flags = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE,\
			.ops = &pic32_roclk_ops,			\
		},							\
		.parent_map = (const u32[]) {				\
			0, 1, 2, 3, 4, 5, 7, 8, 9			\
		},							\
	}

static struct pic32_ref_osc ref_clks[] = {
	DECLARE_REFO_CLOCK(1, 0x80),
	DECLARE_REFO_CLOCK(2, 0xa0),
	DECLARE_REFO_CLOCK(3, 0xc0),
	DECLARE_REFO_CLOCK(4, 0xe0),
	DECLARE_REFO_CLOCK(5, 0x100),
};

static struct pic32_periph_clk periph_clocks[] = {
	DECLARE_PERIPHERAL_CLOCK("pb1_clk", 0x140, 0),
	DECLARE_PERIPHERAL_CLOCK("pb2_clk", 0x150, CLK_IGNORE_UNUSED),
	DECLARE_PERIPHERAL_CLOCK("pb3_clk", 0x160, 0),
	DECLARE_PERIPHERAL_CLOCK("pb4_clk", 0x170, 0),
	DECLARE_PERIPHERAL_CLOCK("pb5_clk", 0x180, 0),
	DECLARE_PERIPHERAL_CLOCK("pb6_clk", 0x190, 0),
	DECLARE_PERIPHERAL_CLOCK("cpu_clk", 0x1a0, CLK_IGNORE_UNUSED),
};

static struct pic32_sys_clk sys_mux_clk = {
	.mux_reg = (void __iomem *)0x0,
	.slew_reg = (void __iomem *)0x1c0,
	.slew_div = 2, /* step of div_4 -> div_2 -> no_div */
	.hw.init = &(struct clk_init_data) {
		.name = "sys_clk",
		.parent_names = (const char *[]) {
			"frcdiv_clk", "sys_pll", "posc_clk",
			"sosc_clk", "lprc_clk", "frcdiv_clk",
		},
		.num_parents = 6,
		.ops = &pic32_sclk_ops,
	},
	.parent_map = (const u32[]) {
		0, 1, 2, 4, 5, 7,
	},
};

static struct pic32_sys_pll sys_pll = {
	.ctrl_reg = (void __iomem *)0x020,
	.status_reg = (void __iomem *)0x1d0,
	.lock_mask = BIT(7),
	.hw.init = &(struct clk_init_data) {
		.name = "sys_pll",
		.parent_names = (const char *[]) {
			"spll_mux_clk"
		},
		.num_parents = 1,
		.ops = &pic32_spll_ops,
	},
};

static struct pic32_sec_osc sosc_clk = {
	.enable_reg = (void __iomem *)0x0,
	.status_reg = (void __iomem *)0x1d0,
	.enable_bitmask = BIT(1),
	.status_bitmask = BIT(4),
	.hw.init = &(struct clk_init_data) {
		.name = "sosc_clk",
		.parent_names = NULL,
		.num_parents = 0,
		.flags = CLK_IS_ROOT,
		.ops = &pic32_sosc_ops,
	},
};

static int pic32_fscm_nmi(struct notifier_block *nb,
			  unsigned long action, void *data)
{
	if (readl(pic32_clk_iobase) & BIT(2))
		pr_err("pic32-clk: FSCM detected clk failure.\n");

	return NOTIFY_OK;
}

static struct notifier_block failsafe_clk_notifier = {
	.notifier_call = pic32_fscm_nmi,
};

static int pic32mzda_clk_probe(struct platform_device *pdev)
{
	const char *const pll_mux_parents[] = {"posc_clk", "frc_clk"};
	struct device_node *np = pdev->dev.of_node;
	static struct clk_onecell_data onecell_data;
	static struct clk *clks[MAXCLKS];
	struct clk *pll_mux_clk;
	int nr_clks = 0, i;

	pic32_clk_iobase = of_io_request_and_map(np, 0, of_node_full_name(np));
	if (IS_ERR(pic32_clk_iobase))
		panic("pic32-clk: failed to map registers\n");

	/* register fixed rate clocks */
	clks[POSCCLK] = clk_register_fixed_rate(NULL, "posc_clk", NULL,
						CLK_IS_ROOT, 24000000);
	clks[FRCCLK] =  clk_register_fixed_rate(NULL, "frc_clk", NULL,
						CLK_IS_ROOT, 8000000);
	clks[BFRCCLK] = clk_register_fixed_rate(NULL, "bfrc_clk", NULL,
						CLK_IS_ROOT, 8000000);
	clks[LPRCCLK] = clk_register_fixed_rate(NULL, "lprc_clk", NULL,
						CLK_IS_ROOT, 32000);
	clks[UPLLCLK] = clk_register_fixed_rate(NULL, "usbphy_clk", NULL,
						CLK_IS_ROOT, 24000000);
	/* fixed rate (optional) clock */
	if (of_find_property(np, "microchip,pic32mzda-sosc", NULL)) {
		pr_info("pic32-clk: dt requests SOSC.\n");
		clks[SOSCCLK] = pic32_sosc_clk_register(&sosc_clk,
							pic32_clk_iobase);
	}
	/* divider clock */
	clks[FRCDIVCLK] = clk_register_divider(NULL, "frcdiv_clk",
					       "frc_clk", 0,
					       pic32_clk_iobase,
					       OSC_FRCDIV_SHIFT,
					       OSC_FRCDIV_MASK,
					       CLK_DIVIDER_POWER_OF_TWO,
					       &lock);
	/* PLL ICLK mux */
	pll_mux_clk = clk_register_mux(NULL, "spll_mux_clk",
				       pll_mux_parents, 2, 0,
				       pic32_clk_iobase + 0x020,
				       PLL_ICLK_SHIFT, 1, 0, &lock);
	if (IS_ERR(pll_mux_clk))
		panic("spll_mux_clk: clk register failed\n");
	/* PLL */
	clks[PLLCLK] = pic32_spll_clk_register(&sys_pll, pic32_clk_iobase);
	/* SYSTEM clock */
	clks[SCLK] = pic32_sys_clk_register(&sys_mux_clk, pic32_clk_iobase);
	/* Peripheral bus clocks */
	for (nr_clks = PB1CLK, i = 0; nr_clks <= PB7CLK; i++, nr_clks++)
		clks[nr_clks] = pic32_periph_clk_register(&periph_clocks[i],
							  pic32_clk_iobase);

	/* Reference oscillator clock */
	for (nr_clks = REF1CLK, i = 0; nr_clks <= REF5CLK; i++, nr_clks++)
		clks[nr_clks] = pic32_refo_clk_register(&ref_clks[i],
							pic32_clk_iobase);
	/* register clkdev */
	for (i = 0; i < MAXCLKS; i++) {
		if (IS_ERR(clks[i]))
			continue;
		clk_register_clkdev(clks[i], NULL, __clk_get_name(clks[i]));
	}
	/* register clock provider */
	onecell_data.clks = clks;
	onecell_data.clk_num = MAXCLKS;
	of_clk_add_provider(np, of_clk_src_onecell_get, &onecell_data);

	/* register NMI for failsafe clock monitor */
	return register_nmi_notifier(&failsafe_clk_notifier);
}

static const struct of_device_id pic32mzda_clk_match_table[] = {
	{ .compatible = "microchip,pic32mzda-clk", },
	{ }
};
MODULE_DEVICE_TABLE(of, pic32mzda_clk_match_table);

static struct platform_driver pic32mzda_clk_driver = {
	.probe		= pic32mzda_clk_probe,
	.driver		= {
		.name	= "clk-pic32mzda",
		.of_match_table = pic32mzda_clk_match_table,
	},
};

static int __init microchip_pic32mzda_clk_init(void)
{
	return platform_driver_register(&pic32mzda_clk_driver);
}
core_initcall(microchip_pic32mzda_clk_init);

MODULE_DESCRIPTION("Microchip PIC32MZDA Clock Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:clk-pic32mzda");
