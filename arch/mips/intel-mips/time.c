// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 Intel Corporation.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/of.h>

#include <asm/time.h>

void __init plat_time_init(void)
{
	unsigned long cpuclk;
	struct device_node *np;
	struct clk *clk;

	of_clk_init(NULL);

	np = of_get_cpu_node(0, NULL);
	if (!np) {
		pr_err("Failed to get CPU node\n");
		return;
	}

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk)) {
		pr_err("Failed to get CPU clock: %ld\n", PTR_ERR(clk));
		return;
	}

	cpuclk = clk_get_rate(clk);
	/* the chip resolution is the half of the clock*/
	mips_hpt_frequency = cpuclk / 2;
	clk_put(clk);

	write_c0_compare(read_c0_count());
	pr_info("CPU Clock: %ldHz  mips_hpt_frequency %dHz\n",
		cpuclk, mips_hpt_frequency);
	timer_probe();
}
