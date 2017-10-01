/*
 * Copyright (c) 2017 Nuvoton Technology corporation.
 * Copyright 2017 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/clockchips.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>

struct npcm7xx_clockevent_data {
	struct clock_event_device cvd;
	void __iomem *timer_base;
	unsigned int rate;
};

/* Timers registers */
#define REG_TCSR0	0x0 /* Timer 0 Control and Status Register */
#define REG_TICR0	0x8 /* Timer 0 Initial Count Register */
#define REG_TCSR1	0x4 /* Timer 1 Control and Status Register */
#define REG_TICR1	0xc /* Timer 1 Initial Count Register */
#define REG_TDR1	0x14 /* Timer 1 Data Register */
#define REG_TISR	0x18 /* Timer Interrupt Status Register */

#define RESETINT	0x1f
#define PERIOD		BIT(27)
#define INTEN		BIT(29)
#define COUNTEN		BIT(30)
#define ONESHOT		0x0
#define TIMER_OPER	GENMASK(3, 27)
#define MIN_PRESCALE	0x1 /* minimal timer prescale */
#define	TDR_MASK_BITS	24
#define MAX_TIMER_CNT	0xFFFFFF
#define CLR_TIMER0_INT	0x1
#define CLR_TIMER_CSR	0x0


static int npcm7xx_timer_oneshot(struct clock_event_device *evt)
{
	u32 val;
	struct npcm7xx_clockevent_data *cevtd =
	 container_of(evt, struct npcm7xx_clockevent_data, cvd);

	val = readl(cevtd->timer_base + REG_TCSR0);
	val &= ~TIMER_OPER;

	val = readl(cevtd->timer_base + REG_TCSR0);
	val |= (ONESHOT | COUNTEN | INTEN | MIN_PRESCALE);
	writel(val, cevtd->timer_base + REG_TCSR0);

	return 0;
}

static int npcm7xx_timer_periodic(struct clock_event_device *evt)
{
	struct npcm7xx_clockevent_data *cevtd =
		container_of(evt, struct npcm7xx_clockevent_data, cvd);
	u32 val;

	val = readl(cevtd->timer_base + REG_TCSR0);
	val &= ~TIMER_OPER;

	writel((cevtd->rate / HZ), cevtd->timer_base + REG_TICR0);
	val |= (PERIOD | COUNTEN | INTEN | MIN_PRESCALE);

	writel(val, cevtd->timer_base + REG_TCSR0);

	return 0;
}

static int npcm7xx_clockevent_setnextevent(unsigned long evt,
		struct clock_event_device *clk)
{
	struct npcm7xx_clockevent_data *cevtd =
		container_of(clk, struct npcm7xx_clockevent_data, cvd);
	u32 val;

	writel(evt, cevtd->timer_base + REG_TICR0);
	val = readl(cevtd->timer_base + REG_TCSR0);
	val |= (COUNTEN | INTEN | MIN_PRESCALE);
	writel(val, cevtd->timer_base + REG_TCSR0);

	return 0;
}

static struct npcm7xx_clockevent_data npcm7xx_clockevent_data = {
	.cvd = {
		.name		    = "npcm7xx-timer0",
		.features	    = CLOCK_EVT_FEAT_PERIODIC |
				      CLOCK_EVT_FEAT_ONESHOT,
		.set_next_event	    = npcm7xx_clockevent_setnextevent,
		.set_state_shutdown = npcm7xx_timer_oneshot,
		.set_state_periodic = npcm7xx_timer_periodic,
		.set_state_oneshot  = npcm7xx_timer_oneshot,
		.tick_resume	    = npcm7xx_timer_oneshot,
		.rating		    = 300
	},
};

static irqreturn_t npcm7xx_timer0_interrupt(int irq, void *dev_id)
{
	struct npcm7xx_clockevent_data *cevtd = dev_id;
	struct clock_event_device *evt = &cevtd->cvd;

	writel(CLR_TIMER0_INT, cevtd->timer_base + REG_TISR);

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction npcm7xx_timer0_irq = {
	.name		= "npcm7xx-timer0",
	.flags		= IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= npcm7xx_timer0_interrupt,
	.dev_id		= &npcm7xx_clockevent_data,
};

static void __init npcm7xx_clockevents_init(int irq, u32 rate)
{
	writel(CLR_TIMER_CSR, npcm7xx_clockevent_data.timer_base + REG_TCSR0);

	writel(RESETINT, npcm7xx_clockevent_data.timer_base + REG_TISR);
	setup_irq(irq, &npcm7xx_timer0_irq);
	npcm7xx_clockevent_data.cvd.cpumask = cpumask_of(0);

	clockevents_config_and_register(&npcm7xx_clockevent_data.cvd, rate,
					0x1, MAX_TIMER_CNT);
}

static void __init npcm7xx_clocksource_init(u32 rate)
{
	u32 val;

	writel(CLR_TIMER_CSR, npcm7xx_clockevent_data.timer_base + REG_TCSR1);
	writel(MAX_TIMER_CNT, npcm7xx_clockevent_data.timer_base + REG_TICR1);

	val = readl(npcm7xx_clockevent_data.timer_base + REG_TCSR1);
	val |= (COUNTEN | PERIOD | MIN_PRESCALE);
	writel(val, npcm7xx_clockevent_data.timer_base + REG_TCSR1);

	clocksource_mmio_init(npcm7xx_clockevent_data.timer_base + REG_TDR1,
				"npcm7xx-timer1", rate,
				300, (unsigned int)TDR_MASK_BITS,
				clocksource_mmio_readl_down);
}

static int __init npcm7xx_timer_init(struct device_node *np)
{
	struct clk *clk;
	int irq, ret;
	u32 rate;

	irq = irq_of_parse_and_map(np, 0);
	if (!irq)
		return -EINVAL;

	npcm7xx_clockevent_data.timer_base = of_iomap(np, 0);
	if (!npcm7xx_clockevent_data.timer_base)
		return -ENXIO;

	clk = of_clk_get(np, 0);

	if (IS_ERR(clk)) {
		ret = of_property_read_u32(np, "clock-frequency", &rate);
		if (ret)
			goto err_iounmap;
	} else {
		clk_prepare_enable(clk);
		rate = clk_get_rate(clk);
	}

	/* Clock input is divided by PRESCALE + 1 before it is fed */
	/* to the counter */
	rate = rate / (MIN_PRESCALE + 1);

	npcm7xx_clockevent_data.rate = rate;

	npcm7xx_clocksource_init(rate);
	npcm7xx_clockevents_init(irq, rate);

	pr_info("Enabling NPCM7xx clocksource timer base: %p, IRQ: %u\n",
		npcm7xx_clockevent_data.timer_base, irq);

	return 0;

err_iounmap:
	iounmap(npcm7xx_clockevent_data.timer_base);
	return ret;

}

TIMER_OF_DECLARE(npcm7xx, "nuvoton,npcm7xx-timer", npcm7xx_timer_init);

