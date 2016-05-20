/*
 * J-Core SoC PIT/RTC driver
 *
 * Copyright (C) 2015-2016 Smart Energy Instruments, Inc.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/interrupt.h>
#include <linux/profile.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqchip.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/rtc.h>

static unsigned char __iomem *pit_base;
static int pit_irq;
static u32 percpu_offset;
static u32 enable_val;

static struct clock_event_device __percpu *pit_percpu;

#define REG_PITEN 0x00
#define REG_THROT 0x10
#define REG_COUNT 0x14
#define REG_BUSPD 0x18
#define REG_SECHI 0x20
#define REG_SECLO 0x24
#define REG_NSEC  0x28

static cycle_t rtc_read(struct clocksource *cs)
{
	u32 sechi, seclo, nsec, sechi0, seclo0;

	sechi = __raw_readl(pit_base + REG_SECHI);
	seclo = __raw_readl(pit_base + REG_SECLO);
	do {
		sechi0 = sechi;
		seclo0 = seclo;
		nsec  = __raw_readl(pit_base + REG_NSEC);
		sechi = __raw_readl(pit_base + REG_SECHI);
		seclo = __raw_readl(pit_base + REG_SECLO);
	} while (sechi0 != sechi || seclo0 != seclo);

	return ((u64)sechi << 32 | seclo) * 1000000000 + nsec;
}

struct clocksource rtc_csd = {
	.name = "rtc",
	.rating = 400,
	.read = rtc_read,
	.mult = 1,
	.shift = 0,
	.mask = CLOCKSOURCE_MASK(64),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static int pit_disable(struct clock_event_device *ced)
{
	unsigned cpu = smp_processor_id();
	writel(0, pit_base + percpu_offset*cpu + REG_PITEN);
	return 0;
}

static int pit_set(unsigned long delta, struct clock_event_device *ced)
{
	unsigned cpu = smp_processor_id();

	pit_disable(ced);

	writel(delta, pit_base + percpu_offset*cpu + REG_THROT);
	writel(enable_val, pit_base + percpu_offset*cpu + REG_PITEN);

	return 0;
}

static int pit_set_periodic(struct clock_event_device *ced)
{
	unsigned cpu = smp_processor_id();
	unsigned long per = readl(pit_base + percpu_offset*cpu + REG_BUSPD);

	return pit_set(DIV_ROUND_CLOSEST(1000000000, HZ*per), ced);
}

static int pit_local_init(struct clock_event_device *ced)
{
	unsigned cpu = smp_processor_id();
	unsigned long per = readl(pit_base + percpu_offset*cpu + REG_BUSPD);

	pr_info("Local PIT init on cpu %u\n", cpu);

	ced->name = "pit";
	ced->features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT
		| CLOCK_EVT_FEAT_PERCPU;
	ced->cpumask = cpumask_of(cpu);
	ced->rating = 400;
	ced->irq = pit_irq;
	ced->set_state_shutdown = pit_disable;
	ced->set_state_periodic = pit_set_periodic;
	ced->set_state_oneshot = pit_disable;
	ced->set_next_event = pit_set;

	clockevents_config_and_register(ced, DIV_ROUND_CLOSEST(1000000000, per),
	                                1, 0xffffffff);

	pit_set_periodic(ced);

	return 0;
}

static int pit_cpu_notify(struct notifier_block *self, unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_STARTING:
		pit_local_init(this_cpu_ptr(pit_percpu));
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block pit_cpu_nb = {
	.notifier_call = pit_cpu_notify,
};

static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ced = this_cpu_ptr(dev_id);

	if (clockevent_state_oneshot(ced)) pit_disable(ced);

	ced->event_handler(ced);

	return IRQ_HANDLED;
}

static void __init pit_init(struct device_node *node)
{
	unsigned long hwirq;
	int err;

	pit_base = of_iomap(node, 0);
	pit_irq = irq_of_parse_and_map(node, 0);
	of_property_read_u32(node, "cpu-offset", &percpu_offset);

	pr_info("Initializing J-Core PIT at %p IRQ %d\n", pit_base, pit_irq);

	clocksource_register_hz(&rtc_csd, 1000000000);

	pit_percpu = alloc_percpu(struct clock_event_device);
	register_cpu_notifier(&pit_cpu_nb);

	err = request_irq(pit_irq, timer_interrupt,
		IRQF_TIMER | IRQF_PERCPU, "pit", pit_percpu);
	if (err) pr_err("pit irq request failed: %d\n", err);

	hwirq = irq_get_irq_data(pit_irq)->hwirq;
	enable_val = (1<<26) | ((hwirq&0x3c)<<18) | (hwirq<<12);

	pit_local_init(this_cpu_ptr(pit_percpu));
}

CLOCKSOURCE_OF_DECLARE(jcore_pit, "jcore,pit", pit_init);
