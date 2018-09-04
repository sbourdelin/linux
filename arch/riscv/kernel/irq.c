// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2018 Christoph Hellwig
 */

#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>

#include <asm/sbi.h>

/*
 * Possible interrupt causes:
 */
#define INTERRUPT_CAUSE_SOFTWARE    1
#define INTERRUPT_CAUSE_TIMER       5
#define INTERRUPT_CAUSE_EXTERNAL    9

/*
 * The high order bit of the trap cause register is always set for
 * interrupts, which allows us to differentiate them from exceptions
 * quickly.  The INTERRUPT_CAUSE_* macros don't contain that bit, so we
 * need to mask it off.
 */
#define INTERRUPT_CAUSE_FLAG	(1UL << (__riscv_xlen - 1))

asmlinkage void __irq_entry do_IRQ(struct pt_regs *regs)
{
	struct pt_regs *old_regs;

	switch (regs->scause & ~INTERRUPT_CAUSE_FLAG) {
	case INTERRUPT_CAUSE_TIMER:
		old_regs = set_irq_regs(regs);
		irq_enter();
		riscv_timer_interrupt();
		irq_exit();
		set_irq_regs(old_regs);
		break;
#ifdef CONFIG_SMP
	case INTERRUPT_CAUSE_SOFTWARE:
		/*
		 * We only use software interrupts to pass IPIs, so if a non-SMP
		 * system gets one, then we don't know what to do.
		 */
		handle_IPI(regs);
		break;
#endif
	case INTERRUPT_CAUSE_EXTERNAL:
		old_regs = set_irq_regs(regs);
		irq_enter();
		handle_arch_irq(regs);
		irq_exit();
		set_irq_regs(old_regs);
		break;
	default:
		panic("unexpected interrupt cause");
	}
}

#ifdef CONFIG_SMP
static void smp_ipi_trigger_sbi(const struct cpumask *to_whom)
{
	sbi_send_ipi(cpumask_bits(to_whom));
}
#endif

void __init init_IRQ(void)
{
	irqchip_init();
#ifdef CONFIG_SMP
	set_smp_ipi_trigger(smp_ipi_trigger_sbi);
#endif
}
