/*  Copyright 2016 National Instruments Corporation
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 */
#include <linux/init.h>
#include <linux/clk-provider.h>
#include <linux/libfdt.h>
#include <linux/of_platform.h>
#include <linux/of_fdt.h>

#include <asm/prom.h>
#include <asm/fw/fw.h>

#include <asm/mips-boards/generic.h>

const char *get_system_type(void)
{
	return "NI 169445 FPGA";
}

void __init plat_mem_setup(void)
{
	/*
	 * Load the builtin devicetree. This causes the chosen node to be
	 * parsed resulting in our memory appearing
	 */
	__dt_setup_arch(__dtb_start);
}

void __init device_tree_init(void)
{
	if (!initial_boot_params)
		return;

	unflatten_and_copy_device_tree();
}

static int __init customize_machine(void)
{
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	return 0;
}
arch_initcall(customize_machine);

static int __init plat_dev_init(void)
{
	of_clk_init(NULL);
	return 0;
}
device_initcall(plat_dev_init);
