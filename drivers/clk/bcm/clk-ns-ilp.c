/*
 * Copyright (C) 2016 Rafał Miłecki <rafal@milecki.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define PMU_XTAL_FREQ_RATIO			0x66c
#define  XTAL_ALP_PER_4ILP			0x00001fff
#define  XTAL_CTL_EN				0x80000000
#define PMU_SLOW_CLK_PERIOD			0x6dc

struct ns_ilp {
	struct clk *clk;
	struct clk_hw hw;
	struct clk *alp_clk;
	void __iomem *pmu;
};

static int ns_ilp_enable(struct clk_hw *hw)
{
	struct ns_ilp *ilp = container_of(hw, struct ns_ilp, hw);

	writel(0x10199, ilp->pmu + PMU_SLOW_CLK_PERIOD);
	writel(0x10000, ilp->pmu + 0x674);

	return 0;
}

static unsigned long ns_ilp_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct ns_ilp *ilp = container_of(hw, struct ns_ilp, hw);
	void __iomem *pmu = ilp->pmu;
	u32 last_val, cur_val;
	u32 sum = 0, num = 0, loop_num = 0;
	u32 avg;
	int err;

	err = clk_prepare_enable(ilp->alp_clk);
	if (err)
		return 0;

	/* Enable */
	writel(XTAL_CTL_EN, pmu + PMU_XTAL_FREQ_RATIO);

	/* Read initial value */
	last_val = readl(pmu + PMU_XTAL_FREQ_RATIO) & XTAL_ALP_PER_4ILP;

	/* Try getting 20 different values for calculating average */
	while (num < 20) {
		cur_val = readl(pmu + PMU_XTAL_FREQ_RATIO) & XTAL_ALP_PER_4ILP;

		if (cur_val != last_val) {
			/* Got different value, use it */
			sum += cur_val;
			num++;
			loop_num = 0;
			last_val = cur_val;
		} else if (++loop_num > 5000) {
			/* Same value over and over, give up */
			sum += cur_val;
			num++;
			break;
		}
	}

	/* Disable */
	writel(0x0, pmu + PMU_XTAL_FREQ_RATIO);

	avg = sum / num;

	return clk_get_rate(ilp->alp_clk) * 4 / avg;
}

const struct clk_ops ns_ilp_clk_ops = {
	.enable = ns_ilp_enable,
	.recalc_rate = ns_ilp_recalc_rate,
};

static void ns_ilp_init(struct device_node *np)
{
	struct ns_ilp *ilp;
	struct resource res;
	struct clk_init_data init = { 0 };
	int index;
	int err;

	ilp = kzalloc(sizeof(*ilp), GFP_KERNEL);
	if (!ilp)
		return;

	index = of_property_match_string(np, "reg-names", "pmu");
	if (index < 0) {
		err = index;
		goto err_free_ilp;
	}
	err = of_address_to_resource(np, index, &res);
	if (err) {
		err = index;
		goto err_free_ilp;
	}
	ilp->pmu = ioremap_nocache(res.start, resource_size(&res));
	if (IS_ERR(ilp->pmu)) {
		err = PTR_ERR(ilp->pmu);
		goto err_free_ilp;
	}

	ilp->alp_clk = of_clk_get_by_name(np, "alp-clk");
	if (IS_ERR(ilp->alp_clk)) {
		err = PTR_ERR(ilp->alp_clk);
		goto err_unmap_pmu;
	}

	init.name = np->name;
	init.ops = &ns_ilp_clk_ops;
	init.flags = CLK_IS_ROOT;

	ilp->hw.init = &init;
	ilp->clk = clk_register(NULL, &ilp->hw);
	if (WARN_ON(IS_ERR(ilp->clk)))
		goto err_put_alp_clk;

	of_clk_add_provider(np, of_clk_src_simple_get, ilp->clk);

	return;

err_put_alp_clk:
	clk_put(ilp->alp_clk);
err_unmap_pmu:
	iounmap(ilp->pmu);
err_free_ilp:
	kfree(ilp);
	pr_err("Failed to init ILP clock: %d\n", err);
}
CLK_OF_DECLARE(ns_ilp_clk, "brcm,ns-ilp", ns_ilp_init);
