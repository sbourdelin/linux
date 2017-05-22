/*
 * Watchdog support on powerpc systems.
 *
 * Copyright 2017, IBM Corporation.
 *
 * This uses code from arch/sparc/kernel/nmi.c and kernel/watchdog.c
 */
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/nmi.h>
#include <linux/module.h>
#include <linux/export.h>
#include <linux/kprobes.h>
#include <linux/hardirq.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/kdebug.h>
#include <linux/sched/debug.h>
#include <linux/delay.h>
#include <linux/smp.h>

#include <asm/paca.h>

/*
 * The watchdog has a simple timer that runs on each CPU, once per timer
 * period. This is the heartbeat.
 *
 * Then there are checks to see if the heartbeat has not triggered on a CPU
 * for the panic timeout period. Currently the watchdog only supports an
 * SMP check, so the heartbeat only turns on when we have 2 or more CPUs.
 *
 * This is not an NMI watchdog, but Linux uses that name for a generic
 * watchdog in some cases, so NMI gets used in some places.
 */

static cpumask_t wd_cpus_enabled __read_mostly;

static u64 wd_panic_timeout_tb __read_mostly; /* timebase ticks until panic */

static u64 wd_timer_period __read_mostly;  /* msec between checks */

static DEFINE_PER_CPU(struct timer_list, wd_timer);

/*
 * These are for the SMP checker. CPUs clear their pending bit in their
 * heartbeat. If the bitmask becomes empty, the time is noted and the
 * bitmask is refilled.
 *
 * All CPUs clear their bit in the pending mask every timer period.
 * Once all have cleared, the time is noted and the bits are reset.
 * If the time since all clear was greater than the panic timeout,
 * we can panic with the list of stuck CPUs.
 *
 * This will work best with NMI IPIs for crash code so the stuck CPUs
 * can be pulled out to get their backtraces.
 */
static unsigned long __wd_smp_lock = 0;
static int wd_smp_enabled __read_mostly = 0;
static cpumask_t wd_smp_cpus_pending;
static cpumask_t wd_smp_cpus_stuck;
static u64 wd_smp_last_reset_tb;

static inline void wd_smp_lock(unsigned long *flags)
{
	/*
	 * Avoid locking layers if possible.
	 * This may be called from low level interrupt handlers at some
	 * point in future.
	 */
	local_irq_save(*flags);
	while (unlikely(test_and_set_bit_lock(0, &__wd_smp_lock)))
		cpu_relax();
}

static inline void wd_smp_unlock(unsigned long *flags)
{
	clear_bit_unlock(0, &__wd_smp_lock);
	local_irq_restore(*flags);
}

static void wd_lockup_ipi(struct pt_regs *regs)
{
	pr_emerg("Watchdog CPU:%d Hard LOCKUP\n", smp_processor_id());
	if (regs)
		show_regs(regs);
	else
		dump_stack();
}

static void watchdog_smp_panic(int cpu, u64 tb)
{
	unsigned long flags;

	wd_smp_lock(&flags);
	if (!(tb - wd_smp_last_reset_tb >= wd_panic_timeout_tb)) {
		wd_smp_unlock(&flags);
		return;
	}

	pr_emerg("Watchdog CPU:%d detected Hard LOCKUP other CPUS:%*pbl\n",
			cpu, cpumask_pr_args(&wd_smp_cpus_pending));

	if (hardlockup_panic) {
		panic("Hard LOCKUP");
	} else {
		int c;

		for_each_cpu(c, &wd_smp_cpus_pending) {
			if (c == cpu)
				continue;
			smp_send_nmi_ipi(c, wd_lockup_ipi, 1000000);
		}
		smp_flush_nmi_ipi(1000000);
		printk_safe_flush();
		/*
		 * printk_safe_flush() seems to require another print
		 * before anything actually goes out to console.
		 */
	}

	pr_emerg("Watchdog removing stuck CPUS:%*pbl\n",
			cpumask_pr_args(&wd_smp_cpus_pending));

	/* Take the stuck CPU out of the watch group */
	cpumask_or(&wd_smp_cpus_stuck, &wd_smp_cpus_stuck, &wd_smp_cpus_pending);
	cpumask_andnot(&wd_smp_cpus_pending,
			&wd_cpus_enabled,
			&wd_smp_cpus_stuck);
	wd_smp_last_reset_tb = tb;

	wd_smp_unlock(&flags);
}

static void wd_smp_clear_cpu_pending(int cpu, u64 tb)
{
	if (!cpumask_test_cpu(cpu, &wd_smp_cpus_pending)) {
		if (unlikely(cpumask_test_cpu(cpu, &wd_smp_cpus_stuck))) {
			unsigned long flags;

			pr_emerg("Watchdog CPU:%d became unstuck\n", cpu);
			dump_stack();

			wd_smp_lock(&flags);
			cpumask_clear_cpu(cpu, &wd_smp_cpus_stuck);
			wd_smp_unlock(&flags);
		}
		return;
	}

	cpumask_clear_cpu(cpu, &wd_smp_cpus_pending);
	if (cpumask_empty(&wd_smp_cpus_pending)) {
		unsigned long flags;

		wd_smp_lock(&flags);
		if (cpumask_empty(&wd_smp_cpus_pending)) {
			wd_smp_last_reset_tb = tb;
			cpumask_andnot(&wd_smp_cpus_pending,
					&wd_cpus_enabled,
					&wd_smp_cpus_stuck);
		}
		wd_smp_unlock(&flags);
	}
}

static void watchdog_timer_interrupt(int cpu)
{
	u64 tb;

	if (wd_smp_enabled) {
		smp_rmb();

		tb = get_tb();

		wd_smp_clear_cpu_pending(cpu, tb);

		if (tb - wd_smp_last_reset_tb >= wd_panic_timeout_tb)
			watchdog_smp_panic(cpu, tb);
	}
}

static void wd_timer_reset(unsigned int cpu, struct timer_list *t)
{
	t->expires = jiffies + msecs_to_jiffies(wd_timer_period);
	if (wd_timer_period > 1000)
		t->expires = round_jiffies(t->expires);
	add_timer_on(t, cpu);
}

static void wd_timer_fn(unsigned long data)
{
	struct timer_list *t = this_cpu_ptr(&wd_timer);
	int cpu = smp_processor_id();

	watchdog_timer_interrupt(cpu);

	wd_timer_reset(cpu, t);
}

void arch_touch_nmi_watchdog(void)
{
	int cpu = smp_processor_id();

	watchdog_timer_interrupt(cpu);
}
EXPORT_SYMBOL(arch_touch_nmi_watchdog);

static void start_watchdog_timer_on(unsigned int cpu)
{
	struct timer_list *t = per_cpu_ptr(&wd_timer, cpu);

	setup_pinned_timer(t, wd_timer_fn, 0);
	wd_timer_reset(cpu, t);
}

static void stop_watchdog_timer_on(unsigned int cpu)
{
	struct timer_list *t = per_cpu_ptr(&wd_timer, cpu);

	del_timer_sync(t);
}

static int start_wd_on_cpu(unsigned int cpu)
{
	pr_info("Watchdog cpu:%d\n", cpu);

	if (cpumask_test_cpu(cpu, &wd_cpus_enabled)) {
		WARN_ON(1);
		return 0;
	}

	if (!cpumask_test_cpu(cpu, &watchdog_cpumask))
		return 0;

	if (cpumask_weight(&wd_cpus_enabled) > 0) {
		start_watchdog_timer_on(cpu);

		if (cpumask_weight(&wd_cpus_enabled) == 1)
			start_watchdog_timer_on(cpumask_first(&wd_cpus_enabled));
	}

	cpumask_set_cpu(cpu, &wd_cpus_enabled);

	if (cpumask_weight(&wd_cpus_enabled) == 2) {
		cpumask_copy(&wd_smp_cpus_pending, &wd_cpus_enabled);
		wd_smp_last_reset_tb = get_tb();
		smp_wmb();
		wd_smp_enabled = 1;

		pr_info("Watchdog starting cross-CPU SMP watchdog\n");
	}

	return 0;
}

static int stop_wd_on_cpu(unsigned int cpu)
{
	if (!cpumask_test_cpu(cpu, &wd_cpus_enabled)) {
		WARN_ON(1);
		return 0;
	}

	/* In case of == 1, the timer won't have started yet */
	if (cpumask_weight(&wd_cpus_enabled) > 1)
		stop_watchdog_timer_on(cpu);

	cpumask_clear_cpu(cpu, &wd_cpus_enabled);

	if (wd_smp_enabled) {
		smp_wmb();
		wd_smp_clear_cpu_pending(cpu, get_tb());

		if (cpumask_weight(&wd_cpus_enabled) == 1) {
			stop_watchdog_timer_on(cpumask_first(&wd_cpus_enabled));

			pr_info("Watchdog stopping cross-CPU SMP watchdog\n");
			wd_smp_last_reset_tb = get_tb();
			cpumask_copy(&wd_smp_cpus_pending, &wd_cpus_enabled);
			smp_wmb();
			wd_smp_enabled = 0;
		}
	}

	return 0;
}

static void watchdog_calc_timeouts(void)
{
	wd_panic_timeout_tb = watchdog_thresh * ppc_tb_freq;
	wd_timer_period = watchdog_thresh * 1000 / 3;
}

void watchdog_nmi_reconfigure(void)
{
	int cpu;

	watchdog_calc_timeouts();

	for_each_cpu(cpu, &wd_cpus_enabled) {
		stop_wd_on_cpu(cpu);
	}

	if (!(watchdog_enabled & NMI_WATCHDOG_ENABLED))
		return;

	if (watchdog_suspended)
		return;

	for_each_cpu_and(cpu, cpu_online_mask, &watchdog_cpumask) {
		start_wd_on_cpu(cpu);
	}
}

static int __init powerpc_watchdog_init(void)
{
	int err;

	if (!(watchdog_enabled & NMI_WATCHDOG_ENABLED))
		return 0;

	watchdog_calc_timeouts();

	err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "powerpc/watchdog:online",
				start_wd_on_cpu, stop_wd_on_cpu);
	if (err < 0)
		pr_warning("Watchdog could not be initialized");

	return 0;
}
arch_initcall(powerpc_watchdog_init);
