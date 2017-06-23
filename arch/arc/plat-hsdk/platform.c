/*
 * ARC HSDK Platform support code
 *
 * Copyright (C) 2017 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/smp.h>
#include <asm/arcregs.h>
#include <asm/io.h>
#include <asm/mach_desc.h>

#define ARC_CCM_UNUSED_ADDR	0x60000000

/*
 * By default ICCM is mapped to 0x7z while this area is used for
 * Virtual kernel mappings, so move it to currently unused area.
 */
static void __init relocate_iccm(void)
{
	if (cpuinfo_arc700[smp_processor_id()].iccm.sz)
		write_aux_reg(ARC_REG_AUX_ICCM, ARC_CCM_UNUSED_ADDR);
}

/*
 * By default DCCM is mapped to 0x8z while this area is used by kernel,
 * so move it to currently unused area.
 */
static void __init relocate_dccm(void)
{
	if (cpuinfo_arc700[smp_processor_id()].dccm.sz)
		write_aux_reg(ARC_REG_AUX_DCCM, ARC_CCM_UNUSED_ADDR);
}

static void __init hsdk_init_per_cpu(unsigned int cpu)
{
	relocate_iccm();
	relocate_dccm();
}

static const char *hsdk_compat[] __initconst = {
	"snps,hsdk",
	NULL,
};

MACHINE_START(SIMULATION, "hsdk")
	.dt_compat	= hsdk_compat,
	.init_per_cpu	= hsdk_init_per_cpu,
MACHINE_END
