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
#include <linux/irq.h>
#include <linux/of_irq.h>
#include <linux/irqchip/mips-gic.h>
#include <linux/io.h>

#include <asm/irq_cpu.h>
#include <asm/setup.h>

static const struct of_device_id of_irq_ids[] __initconst = {
	{
		.compatible = "mti,cpu-interrupt-controller",
		.data = mips_cpu_irq_of_init
	},
	{},
};

void __init arch_init_irq(void)
{
	of_irq_init(of_irq_ids);
}

