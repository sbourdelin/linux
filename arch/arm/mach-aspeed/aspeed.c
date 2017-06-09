/*
 * Copyright 2017 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <asm/mach/arch.h>

const char *aspeed_timer_compatibles[] = {
	"aspeed,ast2400-timer",
	"aspeed,ast2500-timer",
	NULL,
};

/*
 * For backwards compatibility with pre-4.13 devicetrees, populate the
 * clock-names property in the clocksource node
 */
static void __init aspeed_timer_set_clock_names(void)
{
	const char **compatible = aspeed_timer_compatibles;
	struct device_node *np;

	while (*compatible) {
		for_each_compatible_node(np, NULL, *compatible) {
			struct property *clock_names;
			int rc;

			rc = of_property_count_strings(np, "clock-names");
			if (rc != -EINVAL)
				continue;

			clock_names = kzalloc(sizeof(*clock_names), GFP_KERNEL);

			clock_names->name = kstrdup("clock-names", GFP_KERNEL);
			clock_names->length = sizeof("PCLK");
			clock_names->value = kstrdup("PCLK", GFP_KERNEL);

			of_add_property(np, clock_names);
		}

		compatible++;
	}
}

static void __init aspeed_init_time(void)
{
	aspeed_timer_set_clock_names();

#ifdef CONFIG_COMMON_CLK
	of_clk_init(NULL);
#endif
	timer_probe();
}

static const char *const aspeed_dt_match[] __initconst = {
		"aspeed,ast2400",
		"aspeed,ast2500",
		NULL,
};

DT_MACHINE_START(aspeed_dt, "Aspeed SoC")
	.init_time	= aspeed_init_time,
	.dt_compat	= aspeed_dt_match,
MACHINE_END
