// SPDX-License-Identifier: GPL-2.0+
//
// Copyright 2017-2018 NXP

#include <linux/interrupt.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define CNTCV_LO	0x8
#define CNTCV_HI	0xc
#define CMPCV_LO	0x20
#define CMPCV_HI	0x24
#define CMPCR		0x2c

#define SYS_CTR_EN		0x1
#define SYS_CTR_IRQ_MASK	0x2

static void __iomem *sys_ctr_rd_base;
static void __iomem *sys_ctr_cmp_base;
static struct clock_event_device clockevent_sysctr;

static inline void sysctr_timer_enable(bool enable)
{
	u32 val;

	val = readl(sys_ctr_cmp_base + CMPCR);
	val &= ~SYS_CTR_EN;
	if (enable)
		val |= SYS_CTR_EN;

	writel(val, sys_ctr_cmp_base + CMPCR);
}

static void sysctr_irq_acknowledge(void)
{
	u32 val;

	/* clear th enable bit(EN=0) to clear the ISTAT */
	val = readl(sys_ctr_cmp_base + CMPCR);
	val &= ~SYS_CTR_EN;
	writel(val, sys_ctr_cmp_base + CMPCR);
}

static inline u64 sysctr_read_counter(void)
{
	u32 cnt_hi, tmp_hi, cnt_lo;

	do {
		cnt_hi = readl_relaxed(sys_ctr_rd_base + CNTCV_HI);
		cnt_lo = readl_relaxed(sys_ctr_rd_base + CNTCV_LO);
		tmp_hi = readl_relaxed(sys_ctr_rd_base + CNTCV_HI);
	} while (tmp_hi != cnt_hi);

	return  ((u64) cnt_hi << 32) | cnt_lo;
}

static u64 notrace sysctr_read_sched_clock(void)
{
	return sysctr_read_counter();
}

static u64 sysctr_clocksource_read(struct clocksource *cs)
{
	return sysctr_read_counter();
}

static int __init sysctr_clocksource_init(unsigned int rate)
{
	sched_clock_register(sysctr_read_sched_clock, 56, rate);
	return clocksource_mmio_init(sys_ctr_rd_base, "i.MX sys_ctr",
				     rate, 200, 56, sysctr_clocksource_read);
}

static int sysctr_set_next_event(unsigned long delta,
				 struct clock_event_device *evt)
{
	u32 cmp_hi, cmp_lo;
	u64 next;

	sysctr_timer_enable(false);

	next = sysctr_read_counter();

	next += delta;

	cmp_hi = (next >> 32) & 0x00fffff;
	cmp_lo = next & 0xffffffff;

	writel_relaxed(cmp_hi, sys_ctr_cmp_base + CMPCV_HI);
	writel_relaxed(cmp_lo, sys_ctr_cmp_base + CMPCV_LO);

	sysctr_timer_enable(true);

	return 0;
}

static int sysctr_set_state_oneshot(struct clock_event_device *evt)
{
	/* enable timer */
	sysctr_timer_enable(true);

	return 0;
}

static int sysctr_set_state_shutdown(struct clock_event_device *evt)
{
	/* disable the timer */
	sysctr_timer_enable(false);

	return 0;
}

static irqreturn_t sysctr_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = &clockevent_sysctr;

	sysctr_irq_acknowledge();

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct clock_event_device clockevent_sysctr = {
	.name			= "i.MX system counter timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_DYNIRQ,
	.set_state_oneshot	= sysctr_set_state_oneshot,
	.set_next_event		= sysctr_set_next_event,
	.set_state_shutdown	= sysctr_set_state_shutdown,
	.rating			= 200,
};

static int __init sysctr_clockevent_init(unsigned long rate, int irq)
{
	int ret;

	ret = request_irq(irq, sysctr_timer_interrupt, IRQF_TIMER | IRQF_IRQPOLL,
		    "i.MX system counter timer", &clockevent_sysctr);
	if (ret) {
		pr_err("Failed to request i.MX sysctr timer irq\n");
		return ret;
	}

	clockevent_sysctr.cpumask = cpumask_of(0);
	clockevent_sysctr.irq = irq;
	clockevents_config_and_register(&clockevent_sysctr,
			rate, 0xff, 0x7fffffff);

	return 0;
}

static int __init sysctr_timer_init(struct device_node *np)
{
	u32 rate;
	int irq, ret = 0;

	/* map the system counter's CNTreadbase */
	sys_ctr_rd_base = of_iomap(np, 0);
	if (!sys_ctr_rd_base) {
		pr_err("Failed to map sys_ctr rd base%pOF\n", np);
		return -ENXIO;
	}

	/* map the system counter's CNTcomparebase */
	sys_ctr_cmp_base = of_iomap(np, 1);
	if (!sys_ctr_cmp_base) {
		pr_err("Failed to map sys_ctr compare base%pOF\n", np);
		ret = -ENXIO;
		goto out_free2;
	}

	/*
	 * the purpose of this driver is to provide a global timer,
	 * So only use one compare frame, request frame0's irq only.
	 */
	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		pr_err("Failed to map interrupt for %pOF\n", np);
		ret = -EINVAL;
		goto out_free1;
	}

	if (of_property_read_u32(np, "clock-frequency", &rate)) {
		pr_err("Failed to get clock frequency %pOF\n", np);
		ret = -EINVAL;
		goto out_free1;
	}

	sysctr_clocksource_init(rate);
	sysctr_clockevent_init(rate, irq);

	return 0;

out_free1:
	iounmap(sys_ctr_cmp_base);
out_free2:
	iounmap(sys_ctr_rd_base);
	return ret;
}
TIMER_OF_DECLARE(sysctr_timer, "nxp,sysctr-timer", sysctr_timer_init);
