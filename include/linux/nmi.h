/*
 *  linux/include/linux/nmi.h
 */
#ifndef LINUX_NMI_H
#define LINUX_NMI_H

#include <linux/sched.h>
#include <asm/irq.h>

/**
 * touch_nmi_watchdog - restart NMI watchdog timeout.
 * 
 * If the architecture supports the NMI watchdog, touch_nmi_watchdog()
 * may be used to reset the timeout - for code which intentionally
 * disables interrupts for a long time. This call is stateless.
 */
#if defined(CONFIG_HAVE_NMI_WATCHDOG) || defined(CONFIG_HARDLOCKUP_DETECTOR)
#include <asm/nmi.h>
extern void touch_nmi_watchdog(void);
#else
static inline void touch_nmi_watchdog(void)
{
	touch_softlockup_watchdog();
}
#endif

#if defined(CONFIG_HARDLOCKUP_DETECTOR)
extern void hardlockup_detector_disable(void);
#else
static inline void hardlockup_detector_disable(void) {}
#endif

/*
 * Create trigger_all_cpu_backtrace() etc out of the arch-provided
 * base function(s). Return whether such support was available,
 * to allow calling code to fall back to some other mechanism:
 */
static inline bool trigger_all_cpu_backtrace(void)
{
#if defined(arch_trigger_all_cpu_backtrace)
	arch_trigger_all_cpu_backtrace(true);
	return true;
#elif defined(arch_trigger_cpumask_backtrace)
	arch_trigger_cpumask_backtrace(cpu_online_mask);
	return true;
#else
	return false;
#endif
}

static inline bool trigger_allbutself_cpu_backtrace(void)
{
#if defined(arch_trigger_all_cpu_backtrace)
	arch_trigger_all_cpu_backtrace(false);
	return true;
#elif defined(arch_trigger_cpumask_backtrace)
	cpumask_var_t mask;
	int cpu = get_cpu();

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return false;
	cpumask_copy(mask, cpu_online_mask);
	cpumask_clear_cpu(cpu, mask);
	arch_trigger_cpumask_backtrace(mask);
	put_cpu();
	free_cpumask_var(mask);
	return true;
#else
	return false;
#endif
}

static inline bool trigger_cpumask_backtrace(struct cpumask *mask)
{
#if defined(arch_trigger_cpumask_backtrace)
	arch_trigger_cpumask_backtrace(mask);
	return true;
#else
	return false;
#endif
}

static inline bool trigger_single_cpu_backtrace(int cpu)
{
#if defined(arch_trigger_cpumask_backtrace)
	cpumask_var_t mask;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return false;
	cpumask_set_cpu(cpu, mask);
	arch_trigger_cpumask_backtrace(mask);
	free_cpumask_var(mask);
	return true;
#else
	return false;
#endif
}

/* generic implementation */
void nmi_trigger_cpumask_backtrace(const cpumask_t *mask,
				   void (*raise)(cpumask_t *mask));
bool nmi_cpu_backtrace(struct pt_regs *regs);

#ifdef CONFIG_LOCKUP_DETECTOR
int hw_nmi_is_cpu_stuck(struct pt_regs *);
u64 hw_nmi_get_sample_period(int watchdog_thresh);
extern int nmi_watchdog_enabled;
extern int soft_watchdog_enabled;
extern int watchdog_user_enabled;
extern int watchdog_thresh;
extern unsigned long *watchdog_cpumask_bits;
extern int sysctl_softlockup_all_cpu_backtrace;
extern int sysctl_hardlockup_all_cpu_backtrace;
struct ctl_table;
extern int proc_watchdog(struct ctl_table *, int ,
			 void __user *, size_t *, loff_t *);
extern int proc_nmi_watchdog(struct ctl_table *, int ,
			     void __user *, size_t *, loff_t *);
extern int proc_soft_watchdog(struct ctl_table *, int ,
			      void __user *, size_t *, loff_t *);
extern int proc_watchdog_thresh(struct ctl_table *, int ,
				void __user *, size_t *, loff_t *);
extern int proc_watchdog_cpumask(struct ctl_table *, int,
				 void __user *, size_t *, loff_t *);
extern int lockup_detector_suspend(void);
extern void lockup_detector_resume(void);
#else
static inline int lockup_detector_suspend(void)
{
	return 0;
}

static inline void lockup_detector_resume(void)
{
}
#endif

#ifdef CONFIG_HAVE_ACPI_APEI_NMI
#include <asm/nmi.h>
#endif

#endif
