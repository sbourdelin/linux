/*
 * Copyright (C) 2015 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/of_irq.h>

#include <asm/irq_cpu.h>
#include <asm/mips-cm.h>
#include <asm/traps.h>

static int be_handler(struct pt_regs *regs, int is_fixup)
{
	mips_cm_error_report();
	return is_fixup ? MIPS_BE_FIXUP : MIPS_BE_FATAL;
}

void __init arch_init_irq(void)
{
	board_be_handler = be_handler;

	if (!cpu_has_veic)
		mips_cpu_irq_init();

	irqchip_init();
}
