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

struct stm32_clock_event {
	struct clock_event_device evtdev;
	unsigned periodic_top;
	void __iomem *regs;
};

static int stm32_clock_event_shutdown(struct clock_event_device *evtdev)
{
	struct stm32_clock_event *ce =
		container_of(evtdev, struct stm32_clock_event, evtdev);

	writel_relaxed(0, ce->regs + TIM_DIER);

	return 0;
}

static int stm32_clock_event_set_next_event(unsigned long evt,
					    struct clock_event_device *evtdev)
{
	struct stm32_clock_event *ce =
		container_of(evtdev, struct stm32_clock_event, evtdev);
	unsigned long cnt;

	cnt = readl_relaxed(ce->regs + TIM_CNT);
	writel_relaxed(cnt + evt, ce->regs + TIM_CCR1);
	writel_relaxed(TIM_DIER_CC1IE, ce->regs + TIM_DIER);

	return 0;
}

static int stm32_clock_event_set_periodic(struct clock_event_device *evtdev)
{
	struct stm32_clock_event *ce =
		container_of(evtdev, struct stm32_clock_event, evtdev);

	return stm32_clock_event_set_next_event(ce->periodic_top, evtdev);
}

static int stm32_clock_event_set_oneshot(struct clock_event_device *evtdev)
{
	return stm32_clock_event_set_next_event(0, evtdev);
}

static irqreturn_t stm32_clock_event_handler(int irq, void *dev_id)
{
	struct stm32_clock_event *ce = dev_id;

	writel_relaxed(0, ce->regs + TIM_SR);

	if (clockevent_state_periodic(&ce->evtdev))
		stm32_clock_event_set_periodic(&ce->evtdev);

	if (clockevent_state_oneshot(&ce->evtdev))
		stm32_clock_event_shutdown(&ce->evtdev);

	ce->evtdev.event_handler(&ce->evtdev);

	return IRQ_HANDLED;
}

static int __init stm32_clockevent_init(struct device_node *np,
					void __iomem *base,
					struct clk *clk, int irq)
{
	struct stm32_clock_event *ce;
	unsigned long rate;
	int err;

	ce = kzalloc(sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return -ENOMEM;

	ce->regs = base;
	ce->evtdev.name = "stm32_clockevent";
	ce->evtdev.features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC;
	ce->evtdev.set_state_shutdown = stm32_clock_event_shutdown;
	ce->evtdev.set_state_periodic = stm32_clock_event_set_periodic;
	ce->evtdev.set_state_oneshot = stm32_clock_event_set_oneshot;
	ce->evtdev.tick_resume = stm32_clock_event_shutdown;
	ce->evtdev.set_next_event = stm32_clock_event_set_next_event;
	ce->evtdev.rating = 200;

	rate = clk_get_rate(clk);
	ce->periodic_top = DIV_ROUND_CLOSEST(rate, HZ);

	writel_relaxed(0, ce->regs + TIM_DIER);
	writel_relaxed(0, ce->regs + TIM_SR);

	err = request_irq(irq, stm32_clock_event_handler, IRQF_TIMER,
			  "stm32 clockevent", ce);
	if (err) {
		kfree(ce);
		return err;
	}

	clockevents_config_and_register(&ce->evtdev, rate, 0x60, ~0U);

	return 0;
}

static void __iomem *stm32_timer_cnt __read_mostly;
static u64 notrace stm32_read_sched_clock(void)
{
	return readl_relaxed(stm32_timer_cnt);
}

static int __init stm32_clocksource_init(struct device_node *node,
					 void __iomem *regs,
					 struct clk *clk)
{
	unsigned long rate;

	rate = clk_get_rate(clk);

	writel_relaxed(~0U, regs + TIM_ARR);
	writel_relaxed(0, regs + TIM_PSC);
	writel_relaxed(0, regs + TIM_SR);
	writel_relaxed(0, regs + TIM_DIER);
	writel_relaxed(0, regs + TIM_SR);
	writel_relaxed(TIM_CR1_ARPE | TIM_CR1_UDIS, regs + TIM_CR1);

	/* Make sure that registers are updated */
	writel_relaxed(TIM_EGR_UG, regs + TIM_EGR);

	/* Enable controller */
	writel_relaxed(TIM_CR1_ARPE | TIM_CR1_UDIS | TIM_CR1_CEN,
		       regs + TIM_CR1);

	stm32_timer_cnt = regs + TIM_CNT;
	sched_clock_register(stm32_read_sched_clock, 32, rate);

	return clocksource_mmio_init(stm32_timer_cnt, "stm32_timer",
				     rate, 250, 32, clocksource_mmio_readl_up);
}

static int __init stm32_timer_init(struct device_node *node)
{
	struct reset_control *rstc;
	void __iomem *timer_base;
	unsigned long max_arr;
	struct clk *clk;
	int irq, err = -EINVAL;

	timer_base = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(timer_base)) {
		pr_err("Can't map registers\n");
		goto out;
	}

	irq = irq_of_parse_and_map(node, 0);
	if (irq <= 0) {
		pr_err("Can't parse IRQ\n");
		goto out_unmap;
	}

	clk = of_clk_get(node, 0);
	if (IS_ERR(clk)) {
		pr_err("Can't get timer clock\n");
		goto out_unmap;
	}

	rstc = of_reset_control_get(node, NULL);
	if (!IS_ERR(rstc)) {
		reset_control_assert(rstc);
		reset_control_deassert(rstc);
	}

	err = clk_prepare_enable(clk);
	if (err) {
		pr_err("Couldn't enable parent clock\n");
		goto out_clk;
	}

	/* Detect whether the timer is 16 or 32 bits */
	writel_relaxed(~0U, timer_base + TIM_ARR);
	max_arr = readl_relaxed(timer_base + TIM_ARR);
	if (max_arr != ~0U) {
		err = -EINVAL;
		pr_err("32 bits timer is needed\n");
		goto out_unprepare;
	}

	err = stm32_clocksource_init(node, timer_base, clk);
	if (err)
		goto out_unprepare;

	err = stm32_clockevent_init(node, timer_base, clk, irq);
	if (err)
		goto out_unprepare;

	return 0;

out_unprepare:
	clk_disable_unprepare(clk);
out_clk:
	clk_put(clk);
out_unmap:
	iounmap(timer_base);
out:
	return err;
}

CLOCKSOURCE_OF_DECLARE(stm32, "st,stm32-timer", stm32_timer_init);
