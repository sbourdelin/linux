/*
 * Copyright (C) Maxime Coquelin 2015
 * Author:  Maxime Coquelin <mcoquelin.stm32@gmail.com>
 * License terms:  GNU General Public License (GPL), version 2
 *
 * Inspired by time-efm32.c from Uwe Kleine-Koenig
 */

#include <linux/kernel.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>

#include "timer-of.h"

#define TIM_CR1		0x00
#define TIM_DIER	0x0c
#define TIM_SR		0x10
#define TIM_EGR		0x14
#define TIM_CNT		0x24
#define TIM_PSC		0x28
#define TIM_ARR		0x2c
#define TIM_CCR1	0x34

#define TIM_CR1_CEN	BIT(0)
#define TIM_CR1_UDIS	BIT(1)
#define TIM_CR1_ARPE	BIT(7)

#define TIM_DIER_CC1IE	BIT(1)

#define TIM_EGR_UG	BIT(0)

static int stm32_clock_event_shutdown(struct clock_event_device *evt)
{
	struct timer_of *to = to_timer_of(evt);

	writel_relaxed(0, timer_of_base(to) + TIM_DIER);

	return 0;
}

static int stm32_clock_event_set_next_event(unsigned long evt,
					    struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);
	unsigned long cnt;

	cnt = readl_relaxed(timer_of_base(to) + TIM_CNT);
	writel_relaxed(cnt + evt, timer_of_base(to) + TIM_CCR1);
	writel_relaxed(TIM_DIER_CC1IE, timer_of_base(to) + TIM_DIER);

	return 0;
}

static int stm32_clock_event_set_periodic(struct clock_event_device *evt)
{
	struct timer_of *to = to_timer_of(evt);

	return stm32_clock_event_set_next_event(timer_of_period(to), evt);
}

static int stm32_clock_event_set_oneshot(struct clock_event_device *evt)
{
	return stm32_clock_event_set_next_event(0, evt);
}

static irqreturn_t stm32_clock_event_handler(int irq, void *dev_id)
{
	struct clock_event_device *evt = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(evt);

	writel_relaxed(0, timer_of_base(to) + TIM_SR);

	if (clockevent_state_periodic(evt))
		stm32_clock_event_set_periodic(evt);
	else
		stm32_clock_event_shutdown(evt);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static void __init stm32_clockevent_init(struct timer_of *to)
{
	writel_relaxed(0, timer_of_base(to) + TIM_DIER);
	writel_relaxed(0, timer_of_base(to) + TIM_SR);

	clockevents_config_and_register(&to->clkevt,
					timer_of_rate(to), 0x60, ~0U);
}

static void __iomem *stm32_timer_cnt __read_mostly;
static u64 notrace stm32_read_sched_clock(void)
{
	return readl_relaxed(stm32_timer_cnt);
}

static int __init stm32_clocksource_init(struct timer_of *to)
{
	writel_relaxed(~0U, timer_of_base(to) + TIM_ARR);
	writel_relaxed(0, timer_of_base(to) + TIM_PSC);
	writel_relaxed(0, timer_of_base(to) + TIM_SR);
	writel_relaxed(0, timer_of_base(to) + TIM_DIER);
	writel_relaxed(0, timer_of_base(to) + TIM_SR);
	writel_relaxed(TIM_CR1_ARPE | TIM_CR1_UDIS,
		       timer_of_base(to) + TIM_CR1);

	/* Make sure that registers are updated */
	writel_relaxed(TIM_EGR_UG, timer_of_base(to) + TIM_EGR);

	/* Enable controller */
	writel_relaxed(TIM_CR1_ARPE | TIM_CR1_UDIS | TIM_CR1_CEN,
		       timer_of_base(to) + TIM_CR1);

	stm32_timer_cnt = timer_of_base(to) + TIM_CNT;
	sched_clock_register(stm32_read_sched_clock, 32, timer_of_rate(to));

	return clocksource_mmio_init(stm32_timer_cnt, "stm32_timer",
				     timer_of_rate(to), 250, 32,
				     clocksource_mmio_readl_up);
}

static int __init stm32_timer_init(struct device_node *node)
{
	struct reset_control *rstc;
	unsigned long max_arr;
	struct timer_of *to;
	int ret;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	to->flags = TIMER_OF_IRQ | TIMER_OF_CLOCK | TIMER_OF_BASE;

	to->clkevt.name = "stm32_clockevent";
	to->clkevt.rating = 200;
	to->clkevt.features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC;
	to->clkevt.set_state_shutdown = stm32_clock_event_shutdown;
	to->clkevt.set_state_periodic = stm32_clock_event_set_periodic;
	to->clkevt.set_state_oneshot = stm32_clock_event_set_oneshot;
	to->clkevt.tick_resume = stm32_clock_event_shutdown;
	to->clkevt.set_next_event = stm32_clock_event_set_next_event;

	to->of_irq.handler = stm32_clock_event_handler;

	ret = timer_of_init(node, to);
	if (ret)
		return ret;

	rstc = of_reset_control_get(node, NULL);
	if (!IS_ERR(rstc)) {
		reset_control_assert(rstc);
		reset_control_deassert(rstc);
	}

	/* Detect whether the timer is 16 or 32 bits */
	writel_relaxed(~0U, timer_of_base(to) + TIM_ARR);
	max_arr = readl_relaxed(timer_of_base(to) + TIM_ARR);
	if (max_arr != ~0U) {
		pr_err("32 bits timer is needed\n");
		return -EINVAL;
	}

	ret = stm32_clocksource_init(to);
	if (ret)
		return ret;

	stm32_clockevent_init(to);

	return 0;
}

TIMER_OF_DECLARE(stm32, "st,stm32-timer", stm32_timer_init);
