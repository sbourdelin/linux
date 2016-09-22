/*
 *  Setup code for SAMx7
 *
 *  Copyright (C) 2013 Atmel,
 *                2016 Andras Szemzo <szemzo.andras@gmail.com>
 *
 * Licensed under GPLv2 or later.
 */
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>
#include "generic.h"
#include "soc.h"


#ifdef CONFIG_PM
/* This function has to be defined for various drivers that are using it */
int at91_suspend_entering_slow_clock(void)
{
	return 0;
}
EXPORT_SYMBOL(at91_suspend_entering_slow_clock);
#endif

static const struct at91_soc samx7_socs[] = {
	AT91_SOC(SAME70Q21_CIDR_MATCH, SAME70Q21_EXID_MATCH,
		 "same70q21", "samx7"),
	AT91_SOC(SAME70Q20_CIDR_MATCH, SAME70Q20_EXID_MATCH,
		 "same70q20", "samx7"),
	AT91_SOC(SAME70Q19_CIDR_MATCH, SAME70Q19_EXID_MATCH,
		 "same70q19", "samx7"),
	AT91_SOC(SAMS70Q21_CIDR_MATCH, SAMS70Q21_EXID_MATCH,
		 "sams70q21", "samx7"),
	AT91_SOC(SAMS70Q20_CIDR_MATCH, SAMS70Q20_EXID_MATCH,
		 "sams70q20", "samx7"),
	AT91_SOC(SAMS70Q19_CIDR_MATCH, SAMS70Q19_EXID_MATCH,
		 "sams70q19", "samx7"),
	AT91_SOC(SAMV71Q21_CIDR_MATCH, SAMV71Q21_EXID_MATCH,
		 "samv71q21", "samx7"),
	AT91_SOC(SAMV71Q20_CIDR_MATCH, SAMV71Q20_EXID_MATCH,
		 "samv71q20", "samx7"),
	AT91_SOC(SAMV71Q19_CIDR_MATCH, SAMV71Q19_EXID_MATCH,
		 "samv71q19", "samx7"),
	{ /* sentinel */ },
};

static void __init samx7_dt_device_init(void)
{
	struct soc_device *soc;
	struct device *soc_dev = NULL;

	soc = at91_soc_init(samx7_socs);
	if (soc)
		soc_dev = soc_device_to_device(soc);

	of_platform_populate(NULL, of_default_bus_match_table, NULL, soc_dev);
}

static const char *const samx7_dt_board_compat[] __initconst = {
	"atmel,samx7",
	NULL
};

DT_MACHINE_START(samx7_dt, "Atmel SAMx7")
	.init_machine	= samx7_dt_device_init,
	.dt_compat	= samx7_dt_board_compat,
MACHINE_END

