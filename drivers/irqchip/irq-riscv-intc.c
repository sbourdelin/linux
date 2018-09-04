// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017-2018 SiFive
 * Copyright (C) 2018 Anup Patel
 */

#define pr_fmt(fmt) "riscv-intc: " fmt
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <asm/sbi.h>

static struct irq_domain *intc_domain;
static atomic_t intc_init = ATOMIC_INIT(0);

static void riscv_intc_irq(struct pt_regs *regs)
{
	unsigned long cause = regs->scause & ~INTERRUPT_CAUSE_FLAG;

	if (unlikely(cause >= BITS_PER_LONG))
		panic("unexpected interrupt cause");

	switch (cause) {
#ifdef CONFIG_SMP
	case INTERRUPT_CAUSE_SOFTWARE:
		/*
		 * We only use software interrupts to pass IPIs, so if a non-SMP
		 * system gets one, then we don't know what to do.
		 */
		handle_IPI(regs);
		break;
#endif
	default:
		handle_domain_irq(intc_domain, cause, regs);
		break;
	}
}

/*
 * On RISC-V systems local interrupts are masked or unmasked by writing the SIE
 * (Supervisor Interrupt Enable) CSR.  As CSRs can only be written on the local
 * hart, these functions can only be called on the hart that corresponds to the
 * IRQ chip.  They are only called internally to this module, so they BUG_ON if
 * this condition is violated rather than attempting to handle the error by
 * forwarding to the target hart, as that's already expected to have been done.
 */
static void riscv_intc_irq_mask(struct irq_data *d)
{
	csr_clear(sie, 1 << (long)d->hwirq);
}

static void riscv_intc_irq_unmask(struct irq_data *d)
{
	csr_set(sie, 1 << (long)d->hwirq);
}

#ifdef CONFIG_SMP
static void riscv_intc_ipi_trigger(const struct cpumask *to_whom)
{
	sbi_send_ipi(cpumask_bits(to_whom));
}

static int riscv_intc_cpu_starting(unsigned int cpu)
{
	csr_set(sie, 1 << INTERRUPT_CAUSE_SOFTWARE);
	return 0;
}

static int riscv_intc_cpu_dying(unsigned int cpu)
{
	csr_clear(sie, 1 << INTERRUPT_CAUSE_SOFTWARE);
	return 0;
}

static void riscv_intc_smp_init(void)
{
	csr_write(sie, 0);
	csr_write(sip, 0);

	set_smp_ipi_trigger(riscv_intc_ipi_trigger);

	cpuhp_setup_state(CPUHP_AP_IRQ_RISCV_STARTING,
			  "irqchip/riscv/intc:starting",
			  riscv_intc_cpu_starting,
			  riscv_intc_cpu_dying);

}
#else
static void riscv_intc_smp_init(void)
{
	csr_write(sie, 0);
	csr_write(sip, 0);
}
#endif

static struct irq_chip riscv_intc_chip = {
	.name = "RISC-V INTC",
	.irq_mask = riscv_intc_irq_mask,
	.irq_unmask = riscv_intc_irq_unmask,
};

static int riscv_intc_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	irq_set_percpu_devid(irq);
	irq_domain_set_info(d, irq, hwirq, &riscv_intc_chip, d->host_data,
			    handle_percpu_devid_irq, NULL, NULL);
	irq_set_status_flags(irq, IRQ_NOAUTOEN);

	return 0;
}

static const struct irq_domain_ops riscv_intc_domain_ops = {
	.map	= riscv_intc_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static int __init riscv_intc_init(struct device_node *node,
				  struct device_node *parent)
{
	/*
	 * RISC-V device trees can have one INTC DT node under
	 * each CPU DT node so INTC init function will be called
	 * once for each INTC DT node. We only need to do INTC
	 * init once for boot CPU so we use atomic counter to
	 * achieve this.
	 */
	if (atomic_inc_return(&intc_init) > 1)
		return 0;

	intc_domain = irq_domain_add_linear(node, BITS_PER_LONG,
					    &riscv_intc_domain_ops, NULL);
	if (!intc_domain)
		goto error_add_linear;

	set_handle_irq(&riscv_intc_irq);

	riscv_intc_smp_init();

	pr_info("%lu local interrupts mapped\n", (long)BITS_PER_LONG);

	return 0;

error_add_linear:
	pr_warn("unable to add IRQ domain\n");
	return -ENXIO;
}

IRQCHIP_DECLARE(riscv, "riscv,cpu-intc", riscv_intc_init);
