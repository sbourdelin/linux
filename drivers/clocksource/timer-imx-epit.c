// SPDX-License-Identifier: GPL-2.0
/*
 * i.MX EPIT Timer
 *
 * Copyright (C) 2010 Sascha Hauer <s.hauer@pengutronix.de>
 * Copyright (C) 2018 Colin Didier <colin.didier@devialet.com>
 * Copyright (C) 2018 Clément Péron <clement.peron@devialet.com>
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>

#define EPITCR				0x00
#define EPITSR				0x04
#define EPITLR				0x08
#define EPITCMPR			0x0c
#define EPITCNR				0x10

#define EPITCR_EN			BIT(0)
#define EPITCR_ENMOD			BIT(1)
#define EPITCR_OCIEN			BIT(2)
#define EPITCR_RLD			BIT(3)
#define EPITCR_PRESC(x)			(((x) & 0xfff) << 4)
#define EPITCR_SWR			BIT(16)
#define EPITCR_IOVW			BIT(17)
#define EPITCR_DBGEN			BIT(18)
#define EPITCR_WAITEN			BIT(19)
#define EPITCR_RES			BIT(20)
#define EPITCR_STOPEN			BIT(21)
#define EPITCR_OM_DISCON		(0 << 22)
#define EPITCR_OM_TOGGLE		(1 << 22)
#define EPITCR_OM_CLEAR			(2 << 22)
#define EPITCR_OM_SET			(3 << 22)
#define EPITCR_CLKSRC_OFF		(0 << 24)
#define EPITCR_CLKSRC_PERIPHERAL	(1 << 24)
#define EPITCR_CLKSRC_REF_HIGH		(2 << 24)
#define EPITCR_CLKSRC_REF_LOW		(3 << 24)

#define EPITSR_OCIF			BIT(0)

struct epit_timer {
	void __iomem *base;
	int irq;
	struct clk *clk;
	struct clock_event_device ced;
	struct irqaction act;
};

static void __iomem *sched_clock_reg;

static inline struct epit_timer *to_epit_timer(struct clock_event_device *ced)
{
	return container_of(ced, struct epit_timer, ced);
}

static inline void epit_irq_disable(struct epit_timer *epittm)
{
	u32 val;

	val = readl_relaxed(epittm->base + EPITCR);
	writel_relaxed(val & ~EPITCR_OCIEN, epittm->base + EPITCR);
}

static inline void epit_irq_enable(struct epit_timer *epittm)
{
	u32 val;

	val = readl_relaxed(epittm->base + EPITCR);
	writel_relaxed(val | EPITCR_OCIEN, epittm->base + EPITCR);
}

static void epit_irq_acknowledge(struct epit_timer *epittm)
{
	writel_relaxed(EPITSR_OCIF, epittm->base + EPITSR);
}

static u64 notrace epit_read_sched_clock(void)
{
	return ~readl_relaxed(sched_clock_reg);
}

static int epit_set_next_event(unsigned long cycles,
			       struct clock_event_device *ced)
{
	struct epit_timer *epittm = to_epit_timer(ced);
	unsigned long tcmp;

	tcmp = readl_relaxed(epittm->base + EPITCNR) - cycles;
	writel_relaxed(tcmp, epittm->base + EPITCMPR);

	return 0;
}

/* Left event sources disabled, no more interrupts appear */
static int epit_shutdown(struct clock_event_device *ced)
{
	struct epit_timer *epittm = to_epit_timer(ced);
	unsigned long flags;

	/*
	 * The timer interrupt generation is disabled at least
	 * for enough time to call epit_set_next_event()
	 */
	local_irq_save(flags);

	/* Disable interrupt in EPIT module */
	epit_irq_disable(epittm);

	/* Clear pending interrupt */
	epit_irq_acknowledge(epittm);

	local_irq_restore(flags);

	return 0;
}

static int epit_set_oneshot(struct clock_event_device *ced)
{
	struct epit_timer *epittm = to_epit_timer(ced);
	unsigned long flags;

	/*
	 * The timer interrupt generation is disabled at least
	 * for enough time to call epit_set_next_event()
	 */
	local_irq_save(flags);

	/* Disable interrupt in EPIT module */
	epit_irq_disable(epittm);

	/* Clear pending interrupt, only while switching mode */
	if (!clockevent_state_oneshot(ced))
		epit_irq_acknowledge(epittm);

	/*
	 * Do not put overhead of interrupt enable/disable into
	 * epit_set_next_event(), the core has about 4 minutes
	 * to call epit_set_next_event() or shutdown clock after
	 * mode switching
	 */
	epit_irq_enable(epittm);
	local_irq_restore(flags);

	return 0;
}

static irqreturn_t epit_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ced = dev_id;
	struct epit_timer *epittm = to_epit_timer(ced);

	epit_irq_acknowledge(epittm);

	ced->event_handler(ced);

	return IRQ_HANDLED;
}

static int __init epit_clocksource_init(struct epit_timer *epittm)
{
	unsigned int c = clk_get_rate(epittm->clk);

	sched_clock_reg = epittm->base + EPITCNR;
	sched_clock_register(epit_read_sched_clock, 32, c);

	return clocksource_mmio_init(epittm->base + EPITCNR, "epit", c, 200, 32,
				     clocksource_mmio_readl_down);
}

static int __init epit_clockevent_init(struct epit_timer *epittm)
{
	struct clock_event_device *ced = &epittm->ced;
	struct irqaction *act = &epittm->act;

	ced->name = "epit";
	ced->features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_DYNIRQ;
	ced->set_state_shutdown = epit_shutdown;
	ced->tick_resume = epit_shutdown;
	ced->set_state_oneshot = epit_set_oneshot;
	ced->set_next_event = epit_set_next_event;
	ced->rating = 200;
	ced->cpumask = cpumask_of(0);
	ced->irq = epittm->irq;
	clockevents_config_and_register(ced, clk_get_rate(epittm->clk),
					0xff, 0xfffffffe);

	act->name = "i.MX EPIT Timer Tick",
	act->flags = IRQF_TIMER | IRQF_IRQPOLL;
	act->handler = epit_timer_interrupt;
	act->dev_id = ced;

	/* Make irqs happen */
	return setup_irq(epittm->irq, act);
}

static int __init epit_timer_init(struct device_node *np)
{
	struct epit_timer *epittm;
	int ret;

	epittm = kzalloc(sizeof(*epittm), GFP_KERNEL);
	if (!epittm)
		return -ENOMEM;

	epittm->base = of_iomap(np, 0);
	if (!epittm->base) {
		ret = -ENXIO;
		goto out_kfree;
	}

	epittm->irq = irq_of_parse_and_map(np, 0);
	if (!epittm->irq) {
		ret = -EINVAL;
		goto out_iounmap;
	}

        /* Get EPIT clock */
        epittm->clk = of_clk_get(np, 0);
        if (IS_ERR(epittm->clk)) {
		pr_err("i.MX EPIT: unable to get clk\n");
		ret = PTR_ERR(epittm->clk);
		goto out_iounmap;
        }

	ret = clk_prepare_enable(epittm->clk);
	if (ret) {
		pr_err("i.MX EPIT: unable to prepare+enable clk\n");
		goto out_iounmap;
	}

	/* Initialise to a known state (all timers off, and timing reset) */
	writel_relaxed(0x0, epittm->base + EPITCR);
	writel_relaxed(0xffffffff, epittm->base + EPITLR);
	writel_relaxed(EPITCR_EN | EPITCR_CLKSRC_REF_HIGH | EPITCR_WAITEN,
		       epittm->base + EPITCR);

	ret = epit_clocksource_init(epittm);
	if (ret) {
		pr_err("i.MX EPIT: failed to init clocksource\n");
		goto out_clk_disable;
	}

	ret = epit_clockevent_init(epittm);
	if (ret) {
		pr_err("i.MX EPIT: failed to init clockevent\n");
		goto out_clk_disable;
	}

	return 0;

out_clk_disable:
	clk_disable_unprepare(epittm->clk);
out_iounmap:
	iounmap(epittm->base);
out_kfree:
	kfree(epittm);

	return ret;
}
TIMER_OF_DECLARE(epit_timer, "fsl,imx31-epit", epit_timer_init);
