/*
 * Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/clk/bcm2835.h>
#include <linux/irqchip/irq-bcm2836.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#ifdef CONFIG_SMP
static const struct of_device_id bcm2836_intc[] = {
	{ .compatible = "brcm,bcm2836-l1-intc" },
	{ },
};

static int bcm2836_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	struct device_node *np;
	void __iomem *base;

	np = of_find_matching_node(NULL, bcm2836_intc);
	if (!np)
		return -ENODEV;

	base = of_iomap(np, 0);
	if (!base)
		return -ENOMEM;

	writel(virt_to_phys(secondary_startup),
	       base + LOCAL_MAILBOX3_SET0 + 16 * cpu);

	iounmap(base);

	return 0;
}

static const struct smp_operations bcm2836_smp_ops = {
	.smp_boot_secondary	= bcm2836_smp_boot_secondary,
};
#endif

static void __init bcm2835_init(void)
{
	bcm2835_init_clocks();
}

static const char * const bcm2835_compat[] = {
#ifdef CONFIG_ARCH_MULTI_V6
	"brcm,bcm2835",
#endif
#ifdef CONFIG_ARCH_MULTI_V7
	"brcm,bcm2836",
#endif
	NULL
};

DT_MACHINE_START(BCM2835, "BCM2835")
	.init_machine = bcm2835_init,
	.dt_compat = bcm2835_compat,
	.smp = smp_ops(bcm2836_smp_ops),
MACHINE_END
