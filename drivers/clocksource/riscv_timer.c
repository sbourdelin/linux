// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */
#define pr_fmt(fmt) "riscv-timer: " fmt
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <asm/sbi.h>

/*
 * All RISC-V systems have a timer attached to every hart.  These timers can be
 * read by the 'rdcycle' pseudo instruction, and can use the SBI to setup
 * events.  In order to abstract the architecture-specific timer reading and
 * setting functions away from the clock event insertion code, we provide
 * function pointers to the clockevent subsystem that perform two basic
 * operations: rdtime() reads the timer on the current CPU, and
 * next_event(delta) sets the next timer event to 'delta' cycles in the future.
 * As the timers are inherently a per-cpu resource, these callbacks perform
 * operations on the current hart.  There is guaranteed to be exactly one timer
 * per hart on all RISC-V systems.
 */

static int riscv_clock_next_event(unsigned long delta,
		struct clock_event_device *ce)
{
	csr_set(sie, SIE_STIE);
	sbi_set_timer(get_cycles64() + delta);
	return 0;
}

static unsigned int riscv_clock_event_irq;
static DEFINE_PER_CPU(struct clock_event_device, riscv_clock_event) = {
	.name			= "riscv_timer_clockevent",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 100,
	.set_next_event		= riscv_clock_next_event,
};

static u64 riscv_sched_clock(void)
{
	return get_cycles64();
}

/*
 * It is guaranteed that all the timers across all the harts are synchronized
 * within one tick of each other, so while this could technically go
 * backwards when hopping between CPUs, practically it won't happen.
 */
static unsigned long long riscv_clocksource_rdtime(struct clocksource *cs)
{
	return get_cycles64();
}

static struct clocksource riscv_clocksource = {
	.name		= "riscv_clocksource",
	.rating		= 300,
	.mask		= CLOCKSOURCE_MASK(BITS_PER_LONG),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
	.read		= riscv_clocksource_rdtime,
};

static int riscv_timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *ce = per_cpu_ptr(&riscv_clock_event, cpu);

	ce->cpumask = cpumask_of(cpu);
	ce->irq = riscv_clock_event_irq;
	clockevents_config_and_register(ce, riscv_timebase, 100, 0x7fffffff);

	enable_percpu_irq(riscv_clock_event_irq, IRQ_TYPE_NONE);
	return 0;
}

static int riscv_timer_dying_cpu(unsigned int cpu)
{
	disable_percpu_irq(riscv_clock_event_irq);
	return 0;
}

static irqreturn_t riscv_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evdev = this_cpu_ptr(&riscv_clock_event);

	csr_clear(sie, SIE_STIE);
	evdev->event_handler(evdev);

	return IRQ_HANDLED;
}

static int __init riscv_timer_init_dt(struct device_node *n)
{
	int error;
	struct irq_domain *domain;
	struct of_phandle_args oirq;

	/*
	 * Either we have one INTC DT node under each CPU DT node
	 * or a single system wide INTC DT node. Irrespective to
	 * number of INTC DT nodes, we only proceed if we are able
	 * to find irq_domain of INTC.
	 *
	 * Once we have INTC irq_domain, we create mapping for timer
	 * interrupt HWIRQ and request_percpu_irq() on it.
	 */

	if (riscv_clock_event_irq)
		return 0;

	oirq.np = n;
	oirq.args_count = 1;
	oirq.args[0] = INTERRUPT_CAUSE_TIMER;

	domain = irq_find_host(oirq.np);
	if (!domain)
		return -ENODEV;

	riscv_clock_event_irq = irq_create_of_mapping(&oirq);
	if (!riscv_clock_event_irq)
		return -ENODEV;

	clocksource_register_hz(&riscv_clocksource, riscv_timebase);
	sched_clock_register(riscv_sched_clock,
			     BITS_PER_LONG, riscv_timebase);

	error = request_percpu_irq(riscv_clock_event_irq,
				   riscv_timer_interrupt,
				   "riscv_timer", &riscv_clock_event);
	if (error)
		return error;

	error = cpuhp_setup_state(CPUHP_AP_RISCV_TIMER_STARTING,
			 "clockevents/riscv/timer:starting",
			 riscv_timer_starting_cpu, riscv_timer_dying_cpu);
	if (error)
		pr_err("RISCV timer register failed error %d\n", error);

	pr_info("running at %lu.%02luMHz frequency\n",
		(unsigned long)riscv_timebase / 1000000,
		(unsigned long)(riscv_timebase / 10000) % 100);

	return error;
}

TIMER_OF_DECLARE(riscv_timer, "riscv,cpu-intc", riscv_timer_init_dt);
