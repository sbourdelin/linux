/*
 * Actions Semi Owl timer
 *
 * Copyright 2012 Actions Semi Inc.
 * Author: Actions Semi, Inc.
 *
 * Copyright (c) 2017 SUSE Linux GmbH
 * Author: Andreas Färber
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/sched_clock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define OWL_Tx_CTL		0x0
#define OWL_Tx_CMP		0x4
#define OWL_Tx_VAL		0x8

#define OWL_Tx_CTL_PD		BIT(0)
#define OWL_Tx_CTL_INTEN	BIT(1)
#define OWL_Tx_CTL_EN		BIT(2)

#define OWL_MAX_Tx 4

struct owl_timer_info {
	int timer_offset[OWL_MAX_Tx];
};

static const struct owl_timer_info *owl_timer_info;

static void __iomem *owl_timer_base;

static inline void __iomem *owl_timer_get_base(unsigned timer_nr)
{
	if (timer_nr >= OWL_MAX_Tx ||
	    owl_timer_info->timer_offset[timer_nr] == -1)
		return NULL;

	return owl_timer_base + owl_timer_info->timer_offset[timer_nr];
}

static inline void owl_timer_reset(unsigned index)
{
	void __iomem *base;

	base = owl_timer_get_base(index);
	if (!base)
		return;

	writel(0, base + OWL_Tx_CTL);
	writel(0, base + OWL_Tx_VAL);
	writel(0, base + OWL_Tx_CMP);
}

static u64 notrace owl_timer_sched_read(void)
{
	return (u64)readl(owl_timer_get_base(0) + OWL_Tx_VAL);
}

static int owl_timer_set_state_shutdown(struct clock_event_device *evt)
{
	writel(0, owl_timer_get_base(0) + OWL_Tx_CTL);

	return 0;
}

static int owl_timer_set_state_oneshot(struct clock_event_device *evt)
{
	owl_timer_reset(1);

	return 0;
}

static int owl_timer_tick_resume(struct clock_event_device *evt)
{
	return 0;
}

static int owl_timer_set_next_event(unsigned long evt,
				    struct clock_event_device *ev)
{
	void __iomem *base = owl_timer_get_base(1);

	writel(0, base + OWL_Tx_CTL);

	writel(0, base + OWL_Tx_VAL);
	writel(evt, base + OWL_Tx_CMP);

	writel(OWL_Tx_CTL_EN | OWL_Tx_CTL_INTEN, base + OWL_Tx_CTL);

	return 0;
}

static struct clock_event_device owl_clockevent = {
	.name			= "owl_tick",
	.rating			= 200,
	.features		= CLOCK_EVT_FEAT_ONESHOT |
				  CLOCK_EVT_FEAT_DYNIRQ,
	.set_state_shutdown	= owl_timer_set_state_shutdown,
	.set_state_oneshot	= owl_timer_set_state_oneshot,
	.tick_resume		= owl_timer_tick_resume,
	.set_next_event		= owl_timer_set_next_event,
};

static irqreturn_t owl_timer1_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = (struct clock_event_device *)dev_id;

	writel(OWL_Tx_CTL_PD, owl_timer_get_base(1) + OWL_Tx_CTL);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static const struct owl_timer_info s500_timer_info = {
	.timer_offset[0] = 0x08,
	.timer_offset[1] = 0x14,
	.timer_offset[2] = -1,
	.timer_offset[3] = -1,
};

static const struct owl_timer_info s900_timer_info = {
	.timer_offset[0] = 0x08,
	.timer_offset[1] = 0x14,
	.timer_offset[2] = 0x30,
	.timer_offset[3] = 0x3c,
};

static const struct of_device_id owl_timer_of_matches[] = {
	{ .compatible = "actions,s500-timer", .data = &s500_timer_info },
	{ .compatible = "actions,s900-timer", .data = &s900_timer_info },
	{ }
};

static int __init owl_timer_init(struct device_node *node)
{
	const struct of_device_id *match;
	struct clk *clk;
	unsigned long rate;
	int timer1_irq, i, ret;

	match = of_match_node(owl_timer_of_matches, node);
	if (!match || !match->data) {
		pr_err("Unknown compatible");
		return -EINVAL;
	}

	owl_timer_info = match->data;

	owl_timer_base = of_io_request_and_map(node, 0, "owl-timer");
	if (IS_ERR(owl_timer_base)) {
		pr_err("Can't map timer registers");
		return PTR_ERR(owl_timer_base);
	}

	timer1_irq = of_irq_get_byname(node, "Timer1");
	if (timer1_irq <= 0) {
		pr_err("Can't parse Timer1 IRQ");
		return -EINVAL;
	}

	clk = of_clk_get(node, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	rate = clk_get_rate(clk);

	for (i = 0; i < OWL_MAX_Tx; i++)
		owl_timer_reset(i);

	writel(OWL_Tx_CTL_EN, owl_timer_get_base(0) + OWL_Tx_CTL);

	sched_clock_register(owl_timer_sched_read, 32, rate);
	clocksource_mmio_init(owl_timer_get_base(0) + OWL_Tx_VAL, node->name,
			      rate, 200, 32, clocksource_mmio_readl_up);

	ret = request_irq(timer1_irq, owl_timer1_interrupt, IRQF_TIMER,
			  "owl-timer", &owl_clockevent);
	if (ret) {
		pr_err("failed to request irq %d\n", timer1_irq);
		return ret;
	}

	owl_clockevent.cpumask = cpumask_of(0);
	owl_clockevent.irq = timer1_irq;

	clockevents_config_and_register(&owl_clockevent, rate,
					0xf, 0xffffffff);

	return 0;
}
CLOCKSOURCE_OF_DECLARE(owl_s500, "actions,s500-timer", owl_timer_init);
CLOCKSOURCE_OF_DECLARE(owl_s900, "actions,s900-timer", owl_timer_init);
