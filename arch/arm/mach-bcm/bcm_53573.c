/*
 * Copyright (C) 2016 Rafał Miłecki <rafal@milecki.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <asm/mach/arch.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/clk.h>

static inline void arch_timer_set_cntfrq(u32 cntfrq)
{
	asm volatile("mcr p15, 0, %0, c14, c0, 0" : : "r" (cntfrq));
}

/*
 * CFE bootloader doesn't meet arch requirements. It doesn't enable ILP clock
 * which is required for arch timer and doesn't set CNTFRQ.
 * Fix is up here.
 */
static void __init bcm_53573_setup_arch_timer(void)
{
	struct of_phandle_args out_args = { };
	struct clk *clk;

	out_args.np = of_find_compatible_node(NULL, NULL, "brcm,bcm53573-ilp");
	if (!out_args.np) {
		pr_warn("Failed to find ILP node\n");
		return;
	}

	clk = of_clk_get_from_provider(&out_args);
	if (!IS_ERR(clk)) {
		if (!clk_prepare_enable(clk))
			arch_timer_set_cntfrq(clk_get_rate(clk));
	}

	of_node_put(out_args.np);
}

/* A copy of ARM's time_init with workaround inserted */
static void __init bcm_53573_init_time(void)
{
#ifdef CONFIG_COMMON_CLK
	of_clk_init(NULL);
#endif
	bcm_53573_setup_arch_timer();
	clocksource_probe();
}

static const char *const bcm_53573_dt_compat[] __initconst = {
	"brcm,bcm53573",
	NULL,
};

DT_MACHINE_START(BCM5301X, "BCM53573")
	.init_time	= bcm_53573_init_time,
	.dt_compat	= bcm_53573_dt_compat,
MACHINE_END
