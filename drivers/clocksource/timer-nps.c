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
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <soc/nps/common.h>

#define NPS_MSU_TICK_LOW	0xC8
#define NPS_CLUSTER_OFFSET	8
#define NPS_CLUSTER_NUM		16

/* This array is per cluster of CPUs (Each NPS400 cluster got 256 CPUs) */
static void *nps_msu_reg_low_addr[NPS_CLUSTER_NUM] __read_mostly;

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

static void __init nps_setup_clocksource(struct device_node *node)
{
	struct clocksource *clksrc = &nps_counter;
	struct clk *clk;
	unsigned long rate;
	int ret, cluster;

	for (cluster = 0; cluster < NPS_CLUSTER_NUM; cluster++)
		nps_msu_reg_low_addr[cluster] =
			nps_host_reg((cluster << NPS_CLUSTER_OFFSET),
				 NPS_MSU_BLKID, NPS_MSU_TICK_LOW);

	clk = of_clk_get(node, 0);
	BUG_ON(IS_ERR(clk));
	clk_prepare_enable(clk);
	rate = clk_get_rate(clk);

	ret = clocksource_register_hz(clksrc, rate);
	if (ret)
		pr_err("Couldn't register clock source.\n");
}

CLOCKSOURCE_OF_DECLARE(ezchip_nps400_clksrc, "ezchip,nps400-timer",
		       nps_setup_clocksource);
