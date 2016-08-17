/*
 * x86 specific code for irq_work
 *
 * Copyright (C) 2010 Red Hat, Inc., Peter Zijlstra
 */

#include <linux/kernel.h>
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <asm/apic.h>
#include <asm/trace/irq_vectors.h>

/*
 * I'm sure header recursion will bite my head off
 */
#ifdef CONFIG_PERF_EVENTS
extern int perf_swevent_get_recursion_context(void);
extern void perf_swevent_put_recursion_context(int rctx);
#else
static inline int  perf_swevent_get_recursion_context(void)		{ return -1; }
static inline void perf_swevent_put_recursion_context(int rctx)		{ }
#endif

static inline void __smp_irq_work_interrupt(void)
{
	inc_irq_stat(apic_irq_work_irqs);
	irq_work_run();
}

__visible notrace void smp_irq_work_interrupt(struct pt_regs *regs)
{
	int rctx = perf_swevent_get_recursion_context();
	ipi_entering_ack_irq();
	__smp_irq_work_interrupt();
	exiting_irq();
	perf_swevent_put_recursionn_context(rctx);
}

__visible notrace void smp_trace_irq_work_interrupt(struct pt_regs *regs)
{
	int rctx = perf_swevent_get_recursion_context();
	ipi_entering_ack_irq();
	trace_irq_work_entry(IRQ_WORK_VECTOR);
	__smp_irq_work_interrupt();
	trace_irq_work_exit(IRQ_WORK_VECTOR);
	exiting_irq();
	perf_swevent_put_recursionn_context(rctx);
}

void arch_irq_work_raise(void)
{
#ifdef CONFIG_X86_LOCAL_APIC
	if (!arch_irq_work_has_interrupt())
		return;

	apic->send_IPI_self(IRQ_WORK_VECTOR);
	apic_wait_icr_idle();
#endif
}
