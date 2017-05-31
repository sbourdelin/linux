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

static const char *const samx7_dt_board_compat[] __initconst = {
	"atmel,samx7",
	NULL
};

DT_MACHINE_START(samx7_dt, "Atmel SAMx7")
	.dt_compat	= samx7_dt_board_compat,
MACHINE_END
