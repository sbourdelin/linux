// SPDX-License-Identifier: GPL-2.0+
//
//  Copyright (C) 2018 MediaTek Inc.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/jiffies.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <clocksource/arm_arch_timer.h>

#define STMR_CLKEVT_DEFAULT_RATE  (13000000)

/* registers */
#define STMR_CON                  (0x0)
#define STMR_VAL                  (0x4)

/* STMR_CON */
#define STMR_CON_EN               BIT(0)
#define STMR_CON_IRQ_EN           BIT(1)
#define STMR_CON_IRQ_CLR          BIT(4)

struct mtk_stmr_clkevt_device {
	void __iomem *base;
	struct clock_event_device dev;
};

static inline struct mtk_stmr_clkevt_device*
	to_mtk_clkevt_device(struct clock_event_device *c)
{
	return container_of(c, struct mtk_stmr_clkevt_device, dev);
}

static void mtk_stmr_reset(struct mtk_stmr_clkevt_device *evt)
{
	/* Clear IRQ */
	writel(STMR_CON_IRQ_CLR | STMR_CON_EN, evt->base + STMR_CON);

	/* Reset counter */
	writel(0, evt->base + STMR_VAL);

	/* Disable timer */
	writel(0, evt->base + STMR_CON);
}

static void mtk_stmr_ack_irq(struct mtk_stmr_clkevt_device *evt)
{
	mtk_stmr_reset(evt);
}

static irqreturn_t mtk_stmr_handler(int irq, void *priv)
{
	struct mtk_stmr_clkevt_device *evt = priv;

	mtk_stmr_ack_irq(evt);
	evt->dev.event_handler(&evt->dev);

	return IRQ_HANDLED;
}

static int mtk_stmr_clkevt_next_event(unsigned long ticks,
				      struct clock_event_device *dev)
{
	struct mtk_stmr_clkevt_device *evt = to_mtk_clkevt_device(dev);

	/*
	 * reset timer first because we do not expect interrupt is triggered
	 * by old compare value.
	 */
	mtk_stmr_reset(evt);

	writel(STMR_CON_EN, evt->base + STMR_CON);

	writel(ticks, evt->base + STMR_VAL);

	writel(STMR_CON_EN | STMR_CON_IRQ_EN, evt->base + STMR_CON);

	return 0;
}

static int mtk_stmr_clkevt_shutdown(struct clock_event_device *dev)
{
	struct mtk_stmr_clkevt_device *evt = to_mtk_clkevt_device(dev);

	mtk_stmr_reset(evt);

	return 0;
}

static int mtk_stmr_clkevt_resume(struct clock_event_device *clk)
{
	return mtk_stmr_clkevt_shutdown(clk);
}

static int mtk_stmr_clkevt_oneshot(struct clock_event_device *clk)
{
	return 0;
}

static int __init mtk_stmr_init(struct device_node *node)
{
	struct mtk_stmr_clkevt_device *evt;
	struct resource res;
	u32 freq;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return -ENOMEM;

	evt->dev.name = "mtk-clkevt";
	evt->dev.shift = 32;
	evt->dev.rating = 300;
	evt->dev.features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_DYNIRQ;
	evt->dev.set_state_shutdown = mtk_stmr_clkevt_shutdown;
	evt->dev.set_state_oneshot = mtk_stmr_clkevt_oneshot;
	evt->dev.tick_resume = mtk_stmr_clkevt_resume;
	evt->dev.set_next_event = mtk_stmr_clkevt_next_event;
	evt->dev.cpumask = cpu_possible_mask;

	evt->base = of_iomap(node, 0);
	if (IS_ERR(evt->base)) {
		pr_err("Can't get resource\n");
		goto err_kzalloc;
	}

	mtk_stmr_reset(evt);

	evt->dev.irq = irq_of_parse_and_map(node, 0);
	if (evt->dev.irq <= 0) {
		pr_err("Can't parse IRQ\n");
		goto err_mem;
	}

	if (of_property_read_u32(node, "clock-frequency", &freq)) {
		pr_err("Can't get clk rate\n");
		freq = STMR_CLKEVT_DEFAULT_RATE;
	}

	evt->dev.mult = div_sc(freq, NSEC_PER_SEC, evt->dev.shift);
	evt->dev.max_delta_ns = clockevent_delta2ns(0xffffffff, &evt->dev);
	evt->dev.min_delta_ns = clockevent_delta2ns(3, &evt->dev);

	clockevents_register_device(&evt->dev);

	if (request_irq(evt->dev.irq, mtk_stmr_handler,
			IRQF_TIMER | IRQF_IRQPOLL |
			IRQF_TRIGGER_HIGH | IRQF_PERCPU,
			"mtk-clkevt", evt)) {
		pr_err("failed to setup irq %d\n", evt->dev.irq);
		goto err_mem;
	}

	pr_info("mtk_stmr: base=0x%lx, irq=%d, freq=%d, hz=%d\n",
		(unsigned long)evt->base, evt->dev.irq, freq, HZ);

	return 0;

err_mem:
	iounmap(evt->base);
	of_address_to_resource(node, 0, &res);
	release_mem_region(res.start, resource_size(&res));
err_kzalloc:
	kfree(evt);

	return -EINVAL;
}

TIMER_OF_DECLARE(mtk_systimer, "mediatek,sys_timer", mtk_stmr_init);
