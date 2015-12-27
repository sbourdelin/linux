/*
 * Copyright(c) 2015 EZchip Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/cpu.h>
#include <soc/nps/common.h>

#define NPS_MSU_TICK_LOW	0xC8
#define NPS_CLUSTER_OFFSET	8
#define NPS_CLUSTER_NUM		16

/* Timer related Aux registers */
#define ARC_REG_TIMER0_LIMIT    0x23    /* timer 0 limit */
#define ARC_REG_TIMER0_CTRL     0x22    /* timer 0 control */
#define ARC_REG_TIMER0_CNT      0x21    /* timer 0 count */

#define TIMER_CTRL_IE           (1 << 0) /* Interupt when Count reachs limit */
#define TIMER_CTRL_NH           (1 << 1) /* Count only when CPU NOT halted */

/* This array is per cluster of CPUs (Each NPS400 cluster got 256 CPUs) */
static void *nps_msu_reg_low_addr[NPS_CLUSTER_NUM] __read_mostly;

static unsigned long nps_timer_rate;
static int nps_timer_irq;

static cycle_t nps_clksrc_read(struct clocksource *clksrc)
{
	int cluster = raw_smp_processor_id() >> NPS_CLUSTER_OFFSET;

	return (cycle_t)ioread32be(nps_msu_reg_low_addr[cluster]);
}

static struct clocksource nps_counter = {
	.name	= "EZnps-tick",
	.rating = 301,
	.read   = nps_clksrc_read,
	.mask   = CLOCKSOURCE_MASK(32),
	.flags  = CLOCK_SOURCE_IS_CONTINUOUS,
};

static void nps_timer_event_setup(unsigned int cycles)
{
	write_aux_reg(ARC_REG_TIMER0_LIMIT, cycles);
	write_aux_reg(ARC_REG_TIMER0_CNT, 0);   /* start from 0 */

	write_aux_reg(ARC_REG_TIMER0_CTRL, TIMER_CTRL_IE | TIMER_CTRL_NH);
}

static int nps_clkevent_set_next_event(unsigned long delta,
				       struct clock_event_device *dev)
{
	nps_timer_event_setup(delta);
	return 0;
}

static int nps_clkevent_set_periodic(struct clock_event_device *dev)
{
	/*
	 * At X Hz, 1 sec = 1000ms -> X cycles;
	 *                    10ms -> X / 100 cycles
	 */
	nps_timer_event_setup(nps_timer_rate / HZ);
	return 0;
}

static DEFINE_PER_CPU(struct clock_event_device, nps_clockevent_device) = {
	.name			= "nps_sys_timer",
	.features               = CLOCK_EVT_FEAT_ONESHOT |
				  CLOCK_EVT_FEAT_PERIODIC,
	.rating                 = 300,
	.set_next_event         = nps_clkevent_set_next_event,
	.set_state_periodic     = nps_clkevent_set_periodic,
};

static int nps_timer_cpu_notify(struct notifier_block *self,
				unsigned long action, void *hcpu)
{
	struct clock_event_device *evt = this_cpu_ptr(&nps_clockevent_device);

	evt->irq = nps_timer_irq;
	evt->cpumask = cpumask_of(smp_processor_id());

	/*
	 * Grab cpu pointer in each case to avoid spurious
	 * preemptible warnings
	 */
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_STARTING:
		enable_percpu_irq(nps_timer_irq, 0);
		clockevents_config_and_register(evt, nps_timer_rate,
						0, ULONG_MAX);
		break;
	case CPU_DYING:
		disable_percpu_irq(nps_timer_irq);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block nps_timer_cpu_nb = {
	.notifier_call = nps_timer_cpu_notify,
};

static irqreturn_t nps_timer_irq_handler(int irq, void *dev_id)
{
	struct clock_event_device *evt = this_cpu_ptr(&nps_clockevent_device);
	int irq_reenable = clockevent_state_periodic(evt);

	/*
	 * Any write to CTRL reg ACks the interrupt, we rewrite the
	 * Count when [N]ot [H]alted bit.
	 * And re-arm it if perioid by [I]nterrupt [E]nable bit
	 */
	write_aux_reg(ARC_REG_TIMER0_CTRL, irq_reenable | TIMER_CTRL_NH);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static void __init nps_setup_clocksource(struct device_node *node,
					 struct clk *clk, int irq)
{
	struct clocksource *clksrc = &nps_counter;
	int ret, cluster;

	for (cluster = 0; cluster < NPS_CLUSTER_NUM; cluster++)
		nps_msu_reg_low_addr[cluster] =
			nps_host_reg((cluster << NPS_CLUSTER_OFFSET),
				 NPS_MSU_BLKID, NPS_MSU_TICK_LOW);

	ret = clk_prepare_enable(clk);
	if (ret)
		pr_err("Couldn't enable parent clock\n");

	nps_timer_rate = clk_get_rate(clk);

	ret = clocksource_register_hz(clksrc, nps_timer_rate);
	if (ret)
		pr_err("Couldn't register clock source.\n");
}

static void __init nps_setup_clockevents(struct device_node *node,
					 struct clk *clk, int irq)
{
	struct clock_event_device *evt = this_cpu_ptr(&nps_clockevent_device);
	int ret;

	register_cpu_notifier(&nps_timer_cpu_nb);

	evt->irq = irq;
	evt->cpumask = cpumask_of(smp_processor_id());

	clockevents_config_and_register(evt, nps_timer_rate, 0, ULONG_MAX);

	enable_percpu_irq(irq, 0);

	ret = request_percpu_irq(irq, nps_timer_irq_handler,
				 "timer", evt);
	if (ret)
		pr_err("Unable to register interrupt\n");
}

static void __init nps_timer_init(struct device_node *node)
{
	struct clk *clk;

	nps_timer_irq = irq_of_parse_and_map(node, 0);
	if (nps_timer_irq <= 0)
		panic("Can't parse IRQ");

	clk = of_clk_get(node, 0);
	if (IS_ERR(clk))
		panic("Can't get timer clock");

	nps_setup_clocksource(node, clk, nps_timer_irq);
	nps_setup_clockevents(node, clk, nps_timer_irq);
}

CLOCKSOURCE_OF_DECLARE(ezchip_nps400_clksrc, "ezchip,nps400-timer",
		       nps_timer_init);
