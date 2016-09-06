/*  KVM paravirtual clock driver. A clocksource implementation
    Copyright (C) 2008 Glauber de Oliveira Costa, Red Hat Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/kvm_para.h>
#include <asm/pvclock.h>
#include <asm/msr.h>
#include <asm/apic.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/memblock.h>
#include <linux/sched.h>

#include <asm/x86_init.h>
#include <asm/reboot.h>

static int kvmclock = 1;
static int msr_kvm_system_time = MSR_KVM_SYSTEM_TIME;
static int msr_kvm_wall_clock = MSR_KVM_WALL_CLOCK;
static cycle_t kvm_sched_clock_offset;

static int parse_no_kvmclock(char *arg)
{
	kvmclock = 0;
	return 0;
}
early_param("no-kvmclock", parse_no_kvmclock);

/* The hypervisor will put information about time periodically here */
static struct pvclock_vsyscall_time_info *hv_clock;
static struct pvclock_wall_clock wall_clock;

struct pvclock_vsyscall_time_info *pvclock_pvti_cpu0_va(void)
{
	return hv_clock;
}

/*
 * The wallclock is the time of day when we booted. Since then, some time may
 * have elapsed since the hypervisor wrote the data. So we try to account for
 * that with system time
 */
static void kvm_get_wallclock(struct timespec *now)
{
	struct pvclock_vcpu_time_info *vcpu_time;
	int low, high;
	int cpu;

	low = (int)__pa_symbol(&wall_clock);
	high = ((u64)__pa_symbol(&wall_clock) >> 32);

	native_write_msr(msr_kvm_wall_clock, low, high);

	cpu = get_cpu();

	vcpu_time = &hv_clock[cpu].pvti;
	pvclock_read_wallclock(&wall_clock, vcpu_time, now);

	put_cpu();
}

static int kvm_set_wallclock(const struct timespec *now)
{
	return -1;
}

static cycle_t kvm_clock_read(void)
{
	struct pvclock_vcpu_time_info *src;
	cycle_t ret;
	int cpu;

	preempt_disable_notrace();
	cpu = smp_processor_id();
	src = &hv_clock[cpu].pvti;
	ret = pvclock_clocksource_read(src);
	preempt_enable_notrace();
	return ret;
}

static cycle_t kvm_clock_get_cycles(struct clocksource *cs)
{
	return kvm_clock_read();
}

static cycle_t kvm_sched_clock_read(void)
{
	return kvm_clock_read() - kvm_sched_clock_offset;
}

static inline void kvm_sched_clock_init(bool stable)
{
	if (!stable) {
		pv_time_ops.sched_clock = kvm_clock_read;
		return;
	}

	kvm_sched_clock_offset = kvm_clock_read();
	pv_time_ops.sched_clock = kvm_sched_clock_read;
	set_sched_clock_stable();

	printk(KERN_INFO "kvm-clock: using sched offset of %llu cycles\n",
			kvm_sched_clock_offset);

	BUILD_BUG_ON(sizeof(kvm_sched_clock_offset) >
	         sizeof(((struct pvclock_vcpu_time_info *)NULL)->system_time));
}

/*
 * If we don't do that, there is the possibility that the guest
 * will calibrate under heavy load - thus, getting a lower lpj -
 * and execute the delays themselves without load. This is wrong,
 * because no delay loop can finish beforehand.
 * Any heuristics is subject to fail, because ultimately, a large
 * poll of guests can be running and trouble each other. So we preset
 * lpj here
 */
static unsigned long kvm_get_tsc_khz(void)
{
	struct pvclock_vcpu_time_info *src;
	int cpu;
	unsigned long tsc_khz;

	cpu = get_cpu();
	src = &hv_clock[cpu].pvti;
	tsc_khz = pvclock_tsc_khz(src);
	put_cpu();
	return tsc_khz;
}

static void kvm_get_preset_lpj(void)
{
	unsigned long khz;
	u64 lpj;

	khz = kvm_get_tsc_khz();

	lpj = ((u64)khz * 1000);
	do_div(lpj, HZ);
	preset_lpj = lpj;
}

bool kvm_check_and_clear_guest_paused(void)
{
	bool ret = false;
	struct pvclock_vcpu_time_info *src;
	int cpu = smp_processor_id();

	if (!hv_clock)
		return ret;

	src = &hv_clock[cpu].pvti;
	if ((src->flags & PVCLOCK_GUEST_STOPPED) != 0) {
		src->flags &= ~PVCLOCK_GUEST_STOPPED;
		pvclock_touch_watchdogs();
		ret = true;
	}

	return ret;
}

static struct clocksource kvm_clock = {
	.name = "kvm-clock",
	.read = kvm_clock_get_cycles,
	.rating = 400,
	.mask = CLOCKSOURCE_MASK(64),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

int kvm_register_clock(char *txt)
{
	int cpu = smp_processor_id();
	int low, high, ret;
	struct pvclock_vcpu_time_info *src;

	if (!hv_clock)
		return 0;

	src = &hv_clock[cpu].pvti;
	low = (int)slow_virt_to_phys(src) | 1;
	high = ((u64)slow_virt_to_phys(src) >> 32);
	ret = native_write_msr_safe(msr_kvm_system_time, low, high);
	printk(KERN_INFO "kvm-clock: cpu %d, msr %x:%x, %s\n",
	       cpu, high, low, txt);

	return ret;
}

static void kvm_save_sched_clock_state(void)
{
}

static void kvm_restore_sched_clock_state(void)
{
	kvm_register_clock("primary cpu clock, resume");
}

#ifdef CONFIG_X86_LOCAL_APIC
static void kvm_setup_secondary_clock(void)
{
	/*
	 * Now that the first cpu already had this clocksource initialized,
	 * we shouldn't fail.
	 */
	WARN_ON(kvm_register_clock("secondary cpu clock"));
}
#endif

/*
 * After the clock is registered, the host will keep writing to the
 * registered memory location. If the guest happens to shutdown, this memory
 * won't be valid. In cases like kexec, in which you install a new kernel, this
 * means a random memory location will be kept being written. So before any
 * kind of shutdown from our side, we unregister the clock by writting anything
 * that does not have the 'enable' bit set in the msr
 */
#ifdef CONFIG_KEXEC_CORE
static void kvm_crash_shutdown(struct pt_regs *regs)
{
	native_write_msr(msr_kvm_system_time, 0, 0);
	kvm_disable_steal_time();
	native_machine_crash_shutdown(regs);
}
#endif

static void kvm_shutdown(void)
{
	native_write_msr(msr_kvm_system_time, 0, 0);
	kvm_disable_steal_time();
	native_machine_shutdown();
}

#ifdef CONFIG_X86_LOCAL_APIC
/*
 * kvmclock-based clock event implementation, used only together with the
 * TSC deadline timer.  A subset of the normal LAPIC clockevent, but it
 * uses kvmclock to convert nanoseconds to TSC.  This is necessary to
 * handle changes to the TSC frequency, e.g. from live migration.
 */

static void kvmclock_lapic_timer_setup(unsigned lvtt_value)
{
	lvtt_value |= LOCAL_TIMER_VECTOR | APIC_LVT_TIMER_TSCDEADLINE;
	apic_write(APIC_LVTT, lvtt_value);
}

static int kvmclock_lapic_timer_set_oneshot(struct clock_event_device *evt)
{
	kvmclock_lapic_timer_setup(0);
	printk_once(KERN_DEBUG "kvmclock: TSC deadline timer enabled\n");

	/*
	 * See Intel SDM: TSC-Deadline Mode chapter. In xAPIC mode,
	 * writing to the APIC LVTT and TSC_DEADLINE MSR isn't serialized.
	 * According to Intel, MFENCE can do the serialization here.
	 */
	asm volatile("mfence" : : : "memory");
	return 0;
}

static int kvmclock_lapic_timer_stop(struct clock_event_device *evt)
{
	kvmclock_lapic_timer_setup(APIC_LVT_MASKED);
	wrmsrl(MSR_IA32_TSC_DEADLINE, -1);
	return 0;
}

/*
 * We already have the inverse of the (mult,shift) pair, though this means
 * we need a division.  To avoid it we could compute a multiplicative inverse
 * every time src->version changes.
 */
#define KVMCLOCK_TSC_DEADLINE_MAX_BITS	38
#define KVMCLOCK_TSC_DEADLINE_MAX	((1ull << KVMCLOCK_TSC_DEADLINE_MAX_BITS) - 1)

static int kvmclock_lapic_next_ktime(ktime_t expires,
				     struct clock_event_device *evt)
{
	u64 ns, tsc;
	u32 version;
	int cpu;
	struct pvclock_vcpu_time_info *src;

	cpu = smp_processor_id();
	src = &hv_clock[cpu].pvti;
	ns = ktime_to_ns(expires);

	do {
		u64 delta_ns;
		int shift;

		version = pvclock_read_begin(src);
		if (unlikely(ns < src->system_time)) {
			tsc = src->tsc_timestamp;
			virt_rmb();
			continue;
		}

		delta_ns = ns - src->system_time;

		/* Cap the wait to avoid overflow.  */
		if (unlikely(delta_ns > KVMCLOCK_TSC_DEADLINE_MAX))
			delta_ns = KVMCLOCK_TSC_DEADLINE_MAX;

		/*
		 * delta_tsc = delta_ns << (32-tsc_shift) / tsc_to_system_mul.
		 * The shift is split in two steps so that a 38 bits (275 s)
		 * deadline fits into the 64-bit dividend.
		 */
		shift = 32 - src->tsc_shift;
		
		/* First shift step... */
		delta_ns <<= 64 - KVMCLOCK_TSC_DEADLINE_MAX_BITS;
		shift -= 64 - KVMCLOCK_TSC_DEADLINE_MAX_BITS;

		/* ... division... */
		tsc = div_u64(delta_ns, src->tsc_to_system_mul);

		/* ... and second shift step for the remaining bits.  */
		if (shift >= 0)
			tsc <<= shift;
		else
			tsc >>= -shift;

		tsc += src->tsc_timestamp;
	} while (pvclock_read_retry(src, version));

	wrmsrl(MSR_IA32_TSC_DEADLINE, tsc);
	return 0;
}

/*
 * The local apic timer can be used for any function which is CPU local.
 */
static struct clock_event_device kvm_clockevent = {
	.name			= "lapic",
	/* Under KVM the LAPIC timer always runs in deep C-states.  */
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_KTIME,
	.set_state_shutdown	= kvmclock_lapic_timer_stop,
	.set_state_oneshot	= kvmclock_lapic_timer_set_oneshot,
	.set_next_ktime		= kvmclock_lapic_next_ktime,
	.mult			= 1,
	/* Make LAPIC timer preferrable over percpu HPET */
	.rating			= 150,
	.irq			= -1,
};
static DEFINE_PER_CPU(struct clock_event_device, kvm_events);

static void kvmclock_local_apic_timer_interrupt(void)
{
	int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(kvm_events, cpu);

	/*
	 * Defer to the native clockevent if ours hasn't been setup yet.
	 */
	if (!evt->event_handler) {
		native_local_apic_timer_interrupt();
		return;
	}

	inc_irq_stat(apic_timer_irqs);
	evt->event_handler(evt);
}

/*
 * Setup the local APIC timer for this CPU. Copy the initialized values
 * of the boot CPU and register the clock event in the framework.
 */
static void setup_kvmclock_timer(void)
{
	struct clock_event_device *evt = this_cpu_ptr(&kvm_events);

	kvmclock_lapic_timer_stop(evt);

	memcpy(evt, &kvm_clockevent, sizeof(*evt));
	evt->cpumask = cpumask_of(smp_processor_id());
	clockevents_register_device(evt);
}
#endif

void __init kvmclock_init(void)
{
	struct pvclock_vcpu_time_info *vcpu_time;
	unsigned long mem;
	int size, cpu;
	u8 flags;

	size = PAGE_ALIGN(sizeof(struct pvclock_vsyscall_time_info)*NR_CPUS);

	if (!kvm_para_available())
		return;

	if (kvmclock && kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE2)) {
		msr_kvm_system_time = MSR_KVM_SYSTEM_TIME_NEW;
		msr_kvm_wall_clock = MSR_KVM_WALL_CLOCK_NEW;
	} else if (!(kvmclock && kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE)))
		return;

	printk(KERN_INFO "kvm-clock: Using msrs %x and %x",
		msr_kvm_system_time, msr_kvm_wall_clock);

	mem = memblock_alloc(size, PAGE_SIZE);
	if (!mem)
		return;
	hv_clock = __va(mem);
	memset(hv_clock, 0, size);

	if (kvm_register_clock("primary cpu clock")) {
		hv_clock = NULL;
		memblock_free(mem, size);
		return;
	}

	if (kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE_STABLE_BIT))
		pvclock_set_flags(PVCLOCK_TSC_STABLE_BIT);

	cpu = get_cpu();
	vcpu_time = &hv_clock[cpu].pvti;
	flags = pvclock_read_flags(vcpu_time);

	kvm_sched_clock_init(flags & PVCLOCK_TSC_STABLE_BIT);
	put_cpu();

	x86_platform.calibrate_tsc = kvm_get_tsc_khz;
	x86_platform.get_wallclock = kvm_get_wallclock;
	x86_platform.set_wallclock = kvm_set_wallclock;
#ifdef CONFIG_X86_LOCAL_APIC
	if (boot_cpu_has(X86_FEATURE_TSC_DEADLINE_TIMER) &&
	    !disable_apic && !disable_apic_timer) {
		pv_time_ops.local_apic_timer_interrupt = kvmclock_local_apic_timer_interrupt;
		x86_init.timers.setup_percpu_clockev = setup_kvmclock_timer;
		x86_cpuinit.setup_percpu_clockev = setup_kvmclock_timer;
	}
	x86_cpuinit.early_percpu_clock_init =
		kvm_setup_secondary_clock;
#endif
	x86_platform.save_sched_clock_state = kvm_save_sched_clock_state;
	x86_platform.restore_sched_clock_state = kvm_restore_sched_clock_state;
	machine_ops.shutdown  = kvm_shutdown;
#ifdef CONFIG_KEXEC_CORE
	machine_ops.crash_shutdown  = kvm_crash_shutdown;
#endif
	kvm_get_preset_lpj();
	clocksource_register_hz(&kvm_clock, NSEC_PER_SEC);
	pv_info.name = "KVM";
}

int __init kvm_setup_vsyscall_timeinfo(void)
{
#ifdef CONFIG_X86_64
	int cpu;
	u8 flags;
	struct pvclock_vcpu_time_info *vcpu_time;
	unsigned int size;

	if (!hv_clock)
		return 0;

	size = PAGE_ALIGN(sizeof(struct pvclock_vsyscall_time_info)*NR_CPUS);

	cpu = get_cpu();

	vcpu_time = &hv_clock[cpu].pvti;
	flags = pvclock_read_flags(vcpu_time);

	if (!(flags & PVCLOCK_TSC_STABLE_BIT)) {
		put_cpu();
		return 1;
	}

	put_cpu();

	kvm_clock.archdata.vclock_mode = VCLOCK_PVCLOCK;
#endif
	return 0;
}
