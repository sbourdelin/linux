// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Microsemi MIPS SoC support
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2017 Microsemi Corporation
 */
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/reboot.h>

#include <asm/time.h>
#include <asm/idle.h>
#include <asm/prom.h>
#include <asm/reboot.h>

static void __init ocelot_earlyprintk_init(void)
{
	void __iomem *uart_base;

	uart_base = ioremap_nocache(0x70100000, 0x0f);
	setup_8250_early_printk_port((unsigned long)uart_base, 2, 50000);
}

void __init prom_init(void)
{
	/* Sanity check for defunct bootloader */
	if (fw_arg0 < 10 && (fw_arg1 & 0xFFF00000) == 0x80000000) {
		unsigned int prom_argc = fw_arg0;
		const char **prom_argv = (const char **)fw_arg1;

		if (prom_argc > 1 && strlen(prom_argv[1]) > 0)
			/* ignore all built-in args if any f/w args given */
			strcpy(arcs_cmdline, prom_argv[1]);
	}
}

void __init prom_free_prom_memory(void)
{
}

unsigned int get_c0_compare_int(void)
{
	return CP0_LEGACY_COMPARE_IRQ;
}

void __init plat_time_init(void)
{
	struct device_node *np;
	u32 freq;

	np = of_find_node_by_name(NULL, "cpus");
	if (!np)
		panic("missing 'cpus' DT node");
	if (of_property_read_u32(np, "mips-hpt-frequency", &freq) < 0)
		panic("missing 'mips-hpt-frequency' property");
	of_node_put(np);

	mips_hpt_frequency = freq;
}

void __init arch_init_irq(void)
{
	irqchip_init();
}

const char *get_system_type(void)
{
	return "Microsemi Ocelot";
}

static void __init ocelot_late_init(void)
{
	ocelot_earlyprintk_init();
}

extern void (*late_time_init)(void);

void __init plat_mem_setup(void)
{
	/* This has to be done so late because ioremap needs to work */
	late_time_init = ocelot_late_init;

	__dt_setup_arch(__dtb_start);
}

void __init device_tree_init(void)
{
	if (!initial_boot_params)
		return;

	unflatten_and_copy_device_tree();
}

static int __init populate_machine(void)
{
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	return 0;
}
arch_initcall(populate_machine);
