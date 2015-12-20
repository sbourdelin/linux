/*
 *  arch/arm/mach-vt8500/timer.c
 *
 *  Copyright (C) 2012 Tony Prisk <linux@prisktech.co.nz>
 *  Copyright (C) 2010 Alexey Charkov <alchark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * This file is copied and modified from the original timer.c provided by
 * Alexey Charkov. Minor changes have been made for Device Tree Support.
 */

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/delay.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define VT8500_TIMER_OFFSET	0x0100
#define VT8500_TIMER_HZ		3000000
#define TIMER_MATCH0_VAL	0
#define TIMER_MATCH1_VAL	0x04
#define TIMER_MATCH2_VAL	0x08
#define TIMER_MATCH3_VAL	0x0c
#define TIMER_COUNT_VAL		0x0010
#define TIMER_STATUS_VAL	0x0014
#define TIMER_IER_VAL		0x001c		/* interrupt enable */
#define TIMER_CTRL_VAL		0x0020
#define TIMER_AS_VAL		0x0024		/* access status */
/* R/W status flags */
#define TIMER_COUNT_R_ACTIVE	(1 << 5)
#define TIMER_COUNT_W_ACTIVE	(1 << 4)
#define TIMER_MATCH3_W_ACTIVE	(1 << 3)
#define TIMER_MATCH2_W_ACTIVE	(1 << 2)
#define TIMER_MATCH1_W_ACTIVE	(1 << 1)
#define TIMER_MATCH0_W_ACTIVE	(1 << 0)

#define vt8500_timer_sync(bit)	{ while (readl_relaxed \
				    (regbase + TIMER_AS_VAL) & bit) \
					cpu_relax(); }

#define MIN_OSCR_DELTA		16

static void __iomem *regbase;

static void vt8500_timer_write(unsigned long reg, u32 value)
{
	switch (reg) {
	case TIMER_COUNT_VAL:
		vt8500_timer_sync(TIMER_COUNT_W_ACTIVE);
		break;
	case TIMER_MATCH0_VAL:
		vt8500_timer_sync(TIMER_MATCH0_W_ACTIVE);
		break;
	case TIMER_MATCH1_VAL:
		vt8500_timer_sync(TIMER_MATCH1_W_ACTIVE);
		break;
	case TIMER_MATCH2_VAL:
		vt8500_timer_sync(TIMER_MATCH2_W_ACTIVE);
		break;
	case TIMER_MATCH3_VAL:
		vt8500_timer_sync(TIMER_MATCH3_W_ACTIVE);
		break;
	}

	writel_relaxed(value, regbase + reg);
}

static u32 vt8500_timer_read(unsigned long reg)
{
	if (reg == TIMER_COUNT_VAL) {
		vt8500_timer_write(TIMER_CTRL_VAL, 3);
		vt8500_timer_sync(TIMER_COUNT_R_ACTIVE);

		return readl_relaxed(regbase + TIMER_COUNT_VAL);
	}

	return readl_relaxed(regbase + reg);
}

static cycle_t vt8500_oscr0_read(struct clocksource *cs)
{
	return vt8500_timer_read(TIMER_COUNT_VAL);
}

static struct clocksource clocksource = {
	.name           = "vt8500_timer",
	.rating         = 200,
	.read           = vt8500_oscr0_read,
	.mask           = CLOCKSOURCE_MASK(32),
	.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
};

static int vt8500_timer_set_next_event(unsigned long cycles,
				    struct clock_event_device *evt)
{
	unsigned long alarm = vt8500_timer_read(TIMER_COUNT_VAL) + cycles;

	vt8500_timer_write(TIMER_MATCH0_VAL, alarm);
	if ((signed)(alarm - vt8500_timer_read(
				TIMER_COUNT_VAL)) <= MIN_OSCR_DELTA) {
		return -ETIME;
	}

	vt8500_timer_write(TIMER_IER_VAL, 1);

	return 0;
}

static int vt8500_shutdown(struct clock_event_device *evt)
{
	vt8500_timer_write(TIMER_CTRL_VAL,
				vt8500_timer_read(TIMER_CTRL_VAL) | 1);
	vt8500_timer_write(TIMER_IER_VAL, 0);
	return 0;
}

static struct clock_event_device clockevent = {
	.name			= "vt8500_timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 200,
	.set_next_event		= vt8500_timer_set_next_event,
	.set_state_shutdown	= vt8500_shutdown,
	.set_state_oneshot	= vt8500_shutdown,
};

static irqreturn_t vt8500_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	vt8500_timer_write(TIMER_STATUS_VAL, 0xf);
	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction irq = {
	.name    = "vt8500_timer",
	.flags   = IRQF_TIMER | IRQF_IRQPOLL,
	.handler = vt8500_timer_interrupt,
	.dev_id  = &clockevent,
};

static void __init vt8500_timer_init(struct device_node *np)
{
	int timer_irq;

	regbase = of_iomap(np, 0);
	if (!regbase) {
		pr_err("%s: Missing iobase description in Device Tree\n",
								__func__);
		return;
	}
	timer_irq = irq_of_parse_and_map(np, 0);
	if (!timer_irq) {
		pr_err("%s: Missing irq description in Device Tree\n",
								__func__);
		return;
	}

	vt8500_timer_write(TIMER_CTRL_VAL, 1);
	vt8500_timer_write(TIMER_STATUS_VAL, 0xf);
	vt8500_timer_write(TIMER_MATCH0_VAL, ~0);

	if (clocksource_register_hz(&clocksource, VT8500_TIMER_HZ))
		pr_err("%s: vt8500_timer_init: clocksource_register failed for %s\n",
					__func__, clocksource.name);

	clockevent.cpumask = cpumask_of(0);

	if (setup_irq(timer_irq, &irq))
		pr_err("%s: setup_irq failed for %s\n", __func__,
							clockevent.name);
	clockevents_config_and_register(&clockevent, VT8500_TIMER_HZ,
					MIN_OSCR_DELTA * 2, 0xf0000000);
}

CLOCKSOURCE_OF_DECLARE(vt8500, "via,vt8500-timer", vt8500_timer_init);
