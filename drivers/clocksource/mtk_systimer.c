// SPDX-License-Identifier: GPL-2.0+
//
//  Copyright (C) 2018 MediaTek Inc.

#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/io.h>
#include "timer-of.h"

/* registers */
#define STMR_CON                  (0x0)
#define STMR_VAL                  (0x4)

#define TIMER_REG_CON(to)         (timer_of_base(to) + STMR_CON)
#define TIMER_REG_VAL(to)         (timer_of_base(to) + STMR_VAL)

/* STMR_CON */
#define STMR_CON_EN               BIT(0)
#define STMR_CON_IRQ_EN           BIT(1)
#define STMR_CON_IRQ_CLR          BIT(4)

#define TIMER_SYNC_TICKS          3

static void mtk_stmr_reset(struct timer_of *to)
{
	/* Clear IRQ */
	writel(STMR_CON_IRQ_CLR | STMR_CON_EN, TIMER_REG_CON(to));

	/* Reset counter */
	writel(0, TIMER_REG_VAL(to));

	/* Disable timer */
	writel(0, TIMER_REG_CON(to));
}

static void mtk_stmr_ack_irq(struct timer_of *to)
{
	mtk_stmr_reset(to);
}

static irqreturn_t mtk_stmr_handler(int irq, void *dev_id)
{
	struct clock_event_device *clkevt = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(clkevt);

	mtk_stmr_ack_irq(to);
	clkevt->event_handler(clkevt);

	return IRQ_HANDLED;
}

static int mtk_stmr_clkevt_next_event(unsigned long ticks,
				      struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	/*
	 * reset timer first because we do not expect interrupt is triggered
	 * by old compare value.
	 */
	mtk_stmr_reset(to);

	writel(STMR_CON_EN, TIMER_REG_CON(to));

	writel(ticks, TIMER_REG_VAL(to));

	writel(STMR_CON_EN | STMR_CON_IRQ_EN, TIMER_REG_CON(to));

	return 0;
}

static int mtk_stmr_clkevt_shutdown(struct clock_event_device *clkevt)
{
	mtk_stmr_reset(to_timer_of(clkevt));

	return 0;
}

static int mtk_stmr_clkevt_resume(struct clock_event_device *clkevt)
{
	return mtk_stmr_clkevt_shutdown(clkevt);
}

static int mtk_stmr_clkevt_oneshot(struct clock_event_device *clkevt)
{
	return 0;
}

static struct timer_of to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE | TIMER_OF_CLOCK,

	.clkevt = {
		.name = "mtk-clkevt",
		.rating = 300,
		.features = CLOCK_EVT_FEAT_DYNIRQ | CLOCK_EVT_FEAT_ONESHOT,
		.set_state_shutdown = mtk_stmr_clkevt_shutdown,
		.set_state_oneshot = mtk_stmr_clkevt_oneshot,
		.tick_resume = mtk_stmr_clkevt_resume,
		.set_next_event = mtk_stmr_clkevt_next_event,
		.cpumask = cpu_possible_mask,
	},

	.of_irq = {
		.handler = mtk_stmr_handler,
		.flags = IRQF_TIMER | IRQF_IRQPOLL | IRQF_TRIGGER_HIGH |
			 IRQF_PERCPU,
	},
};

static int __init mtk_stmr_init(struct device_node *node)
{
	int ret;

	ret = timer_of_init(node, &to);
	if (ret)
		return ret;

	mtk_stmr_reset(&to);

	clockevents_config_and_register(&to.clkevt, timer_of_rate(&to),
					TIMER_SYNC_TICKS, 0xffffffff);

	pr_info("mtk_stmr: irq=%d, rate=%lu, max_ns: %llu, min_ns: %llu\n",
		timer_of_irq(&to), timer_of_rate(&to),
		to.clkevt.max_delta_ns, to.clkevt.min_delta_ns);

	return ret;
}

TIMER_OF_DECLARE(mtk_systimer, "mediatek,sys_timer", mtk_stmr_init);
