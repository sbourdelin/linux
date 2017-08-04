/*
 * Workarounds for Adaptrum Anarion SOC
 *
 * Copyright (C) 2017, Adaptrum, Inc.
 * (Written by Alexandru Gagniuc <alex.g at adaptrum.com> for Adaptrum, Inc.)
 * Licensed under the GPLv2 or (at your option) any later version.
 */

#include <asm/io.h>
#include <linux/init.h>
#include <asm/mach_desc.h>

#define GMAC0_RESET		0xf2018000
#define GMAC1_RESET		0xf2018100

/* This works around an issue where the GMAC will generate interrupts before
 * the driver is probed, confusing the heck out of the early boot.
 */
static void __init anarion_gmac_irq_storm_workaround(void)
{
	writel(1, (void *)GMAC0_RESET);
	writel(1, (void *)GMAC1_RESET);
}

static void __init anarion_early_init(void)
{
	anarion_gmac_irq_storm_workaround();
	/* Please, no more workarounds!!! */
}

static const char *anarion_compat[] __initconst = {
	"adaptrum,anarion",
	NULL,
};

MACHINE_START(ANARION, "anarion")
	.dt_compat	= anarion_compat,
	.init_early	= anarion_early_init,
MACHINE_END
