/*
 * Copyright (C) 2015 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/string.h>

#include <asm/fw/fw.h>
#include <asm/mips-cm.h>
#include <asm/mips-cpc.h>
#include <asm/prom.h>
#include <asm/smp-ops.h>

void __init plat_mem_setup(void)
{
	if (fw_arg0 != -2)
		panic("Device-tree not present");

	__dt_setup_arch((void *)fw_arg1);
	strlcpy(arcs_cmdline, boot_command_line, COMMAND_LINE_SIZE);
}

void __init device_tree_init(void)
{
	unflatten_and_copy_device_tree();
}

static int __init publish_devices(void)
{
	if (!of_have_populated_dt())
		panic("Device-tree not present");

	if (of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL))
		panic("Failed to populate DT");

	return 0;
}
arch_initcall(publish_devices);

phys_addr_t mips_cpc_default_phys_base(void)
{
	return 0x16200000;
}

phys_addr_t mips_cdmm_phys_base(void)
{
	return 0x16140000;
}

const char *get_system_type(void)
{
	return "MIPS Boston";
}

void __init prom_init(void)
{
	fw_init_cmdline();
	mips_cm_probe();
	mips_cpc_probe();
	register_cps_smp_ops();
}

void __init prom_free_prom_memory(void)
{
}
