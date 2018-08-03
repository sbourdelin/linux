// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2018 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "clk-cgu-pll.h"
#include "clk-cgu.h"

#define to_intel_clk_pll(_hw)	container_of(_hw, struct intel_clk_pll, hw)

/*
 * Calculate formula:
 * rate = (prate * mult + (prate * frac) / frac_div) / div
 */
static unsigned long
intel_pll_calc_rate(unsigned long prate, unsigned int mult,
		    unsigned int div, unsigned int frac,
		    unsigned int frac_div)
{
	u64 crate, frate, rate64;

	rate64 = prate;
	crate = rate64 * mult;

	if (frac) {
		frate = rate64 * frac;
		do_div(frate, frac_div);
		crate += frate;
	}
	do_div(crate, div);

	return (unsigned long)crate;
}

static void
grx500_pll_get_params(struct intel_clk_pll *pll, unsigned int *mult,
		      unsigned int *frac)
{
	*mult = intel_get_clk_val(pll->map, pll->reg, 2, 7);
	*frac = intel_get_clk_val(pll->map, pll->reg, 9, 21);
}

static int intel_wait_pll_lock(struct intel_clk_pll *pll, int bit_idx)
{
	unsigned int val;

	return regmap_read_poll_timeout(pll->map, pll->reg, val,
					val & BIT(bit_idx), 10, 1000);
}

static unsigned long
intel_grx500_pll_recalc_rate(struct clk_hw *hw, unsigned long prate)
{
	struct intel_clk_pll *pll = to_intel_clk_pll(hw);
	unsigned int mult, frac;

	grx500_pll_get_params(pll, &mult, &frac);

	return intel_pll_calc_rate(prate, mult, 1, frac, BIT(20));
}

static int intel_grx500_pll_is_enabled(struct clk_hw *hw)
{
	struct intel_clk_pll *pll = to_intel_clk_pll(hw);

	if (intel_wait_pll_lock(pll, 1)) {
		pr_err("%s: pll: %s is not locked!\n",
		       __func__, clk_hw_get_name(hw));
		return 0;
	}

	return intel_get_clk_val(pll->map, pll->reg, 1, 1);
}

const static struct clk_ops intel_grx500_pll_ops = {
	.recalc_rate = intel_grx500_pll_recalc_rate,
	.is_enabled = intel_grx500_pll_is_enabled,
};

static struct clk
*intel_clk_register_pll(struct intel_clk_provider *ctx,
			enum intel_pll_type type, const char *cname,
			const char *const *pname, u8 num_parents,
			unsigned long flags, unsigned int reg,
			const struct intel_pll_rate_table *table,
			unsigned int mult, unsigned int div, unsigned int frac)
{
	struct clk_init_data init;
	struct intel_clk_pll *pll;
	struct clk_hw *hw;
	int ret, i;

	if (type != pll_grx500) {
		pr_err("%s: pll type %d not supported!\n",
		       __func__, type);
		return ERR_PTR(-EINVAL);
	}
	init.name = cname;
	init.ops = &intel_grx500_pll_ops;
	init.flags = CLK_IS_BASIC;
	init.parent_names = pname;
	init.num_parents = num_parents;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);
	pll->map = ctx->map;
	pll->reg = reg;
	pll->flags = flags;
	pll->mult = mult;
	pll->div = div;
	pll->frac = frac;
	pll->hw.init = &init;
	if (table) {
		for (i = 0; table[i].rate != 0; i++)
			;
		pll->table_sz = i;
		pll->rate_table = kmemdup(table, i * sizeof(table[0]),
					  GFP_KERNEL);
		if (!pll->rate_table) {
			ret = -ENOMEM;
			goto err_free_pll;
		}
	}
	hw = &pll->hw;
	ret = clk_hw_register(NULL, hw);
	if (ret)
		goto err_free_pll;

	return hw->clk;

err_free_pll:
	kfree(pll);
	return ERR_PTR(ret);
}

void intel_clk_register_plls(struct intel_clk_provider *ctx,
			     struct intel_pll_clk *list, unsigned int nr_clk)
{
	struct clk *clk;
	int i;

	for (i = 0; i < nr_clk; i++, list++) {
		clk = intel_clk_register_pll(ctx, list->type, list->name,
				list->parent_names, list->num_parents,
				list->flags, list->reg, list->rate_table,
				list->mult, list->div, list->frac);
		if (IS_ERR(clk)) {
			pr_err("%s: failed to register pll: %s\n",
			       __func__, list->name);
			continue;
		}

		intel_clk_add_lookup(ctx, clk, list->id);
	}
}
