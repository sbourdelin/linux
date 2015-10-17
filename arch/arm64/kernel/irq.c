/*
 * Based on arch/arm/kernel/irq.c
 *
 * Copyright (C) 1992 Linus Torvalds
 * Modifications for ARM processor Copyright (C) 1995-2000 Russell King.
 * Support for Dynamic Tick Timer Copyright (C) 2004-2005 Nokia Corporation.
 * Dynamic Tick Timer written by Tony Lindgren <tony@atomide.com> and
 * Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>.
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel_stat.h>
#include <linux/irq.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/seq_file.h>

unsigned long irq_err_count;

DEFINE_PER_CPU(void *, irq_stacks);

int arch_show_interrupts(struct seq_file *p, int prec)
{
	show_ipi_list(p, prec);
	seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	return 0;
}

void (*handle_arch_irq)(struct pt_regs *) = NULL;

void __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq)
		return;

	handle_arch_irq = handle_irq;
}

static char boot_irq_stack[IRQ_STACK_SIZE] __aligned(IRQ_STACK_SIZE);

void __init init_IRQ(void)
{
	unsigned int cpu = smp_processor_id();

	per_cpu(irq_stacks, cpu) = boot_irq_stack + IRQ_STACK_START_SP;

	irqchip_init();
	if (!handle_arch_irq)
		panic("No interrupt controller found.");
}

int alloc_irq_stack(unsigned int cpu)
{
	void *stack;

	if (per_cpu(irq_stacks, cpu))
		return 0;

	stack = __alloc_irq_stack();
	if (!stack)
		return -ENOMEM;

	per_cpu(irq_stacks, cpu) = stack + IRQ_STACK_START_SP;

	return 0;
}
