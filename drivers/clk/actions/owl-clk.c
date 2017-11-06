/*
 * Copyright (c) 2014 Actions Semi Inc.
 * Author: David Liu <liuwei@actions-semi.com>
 *
 * Copyright (c) 2017 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 *
 * based on
 *
 * samsung/clk.c
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2013 Linaro Ltd.
 * Author: Thomas Abraham <thomas.ab@owl.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/slab.h>
#include "owl-clk.h"

void owl_clk_add_hw_data(struct owl_clk_provider *ctx, struct clk_hw *clk_hw,
				unsigned int id)
{
	if (id)
		ctx->clk_data.hws[id] = clk_hw;
}

/* register a list of fixed factor clocks */
void owl_clk_register_fixed_factor(struct owl_clk_provider *ctx,
			struct owl_fixed_factor_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_fixed_factor(NULL, clks[i].name,
				clks[i].parent_name, clks[i].flags,
				clks[i].mult, clks[i].div);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
			       __func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

/* register a list of pll clocks */
void owl_clk_register_pll(struct owl_clk_provider *ctx,
			struct owl_pll_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = owl_pll_clk_register(clks[i].name, clks[i].parent_name,
				clks[i].flags, ctx->reg_base + clks[i].offset,
				clks[i].bfreq, clks[i].enable_bit,
				clks[i].shift, clks[i].width,
				clks[i].min_mul, clks[i].max_mul,
				clks[i].pll_flags, clks[i].table,
				&ctx->lock);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
				__func__, clks[i].name);
			continue;

		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

/* register a list of divider clocks */
void owl_clk_register_divider(struct owl_clk_provider *ctx,
		struct owl_divider_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_divider_table(NULL, clks[i].name,
				clks[i].parent_name, clks[i].flags,
				ctx->reg_base + clks[i].offset, clks[i].shift,
				clks[i].width, clks[i].div_flags,
				clks[i].table, &ctx->lock);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
				__func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

/* register a list of factor divider clocks */
void owl_clk_register_factor(struct owl_clk_provider *ctx,
		struct owl_factor_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = owl_factor_clk_register(NULL, clks[i].name,
				clks[i].parent_name, clks[i].flags,
				ctx->reg_base + clks[i].offset, clks[i].shift,
				clks[i].width, clks[i].div_flags,
				clks[i].table, &ctx->lock);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
				__func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

/* register a list of mux clocks */
void owl_clk_register_mux(struct owl_clk_provider *ctx,
		struct owl_mux_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_mux(NULL, clks[i].name,
				clks[i].parent_names, clks[i].num_parents,
				clks[i].flags, ctx->reg_base + clks[i].offset,
				clks[i].shift, clks[i].width,
				clks[i].mux_flags, &ctx->lock);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
				__func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

/* register a list of gate clocks */
void owl_clk_register_gate(struct owl_clk_provider *ctx,
		struct owl_gate_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_gate(NULL, clks[i].name,
				clks[i].parent_name, clks[i].flags,
				ctx->reg_base + clks[i].offset,
				clks[i].bit_idx, clks[i].gate_flags,
				&ctx->lock);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
			       __func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}

static struct clk_hw *_register_composite(struct owl_clk_provider *ctx,
			struct owl_composite_clock *cclk)
{
	struct clk_hw *clk_hw;
	struct owl_mux_clock *amux;
	struct owl_gate_clock *agate;
	union rate_clock *arate;
	struct clk_gate *gate = NULL;
	struct clk_mux *mux = NULL;
	struct clk_fixed_factor *fixed_factor = NULL;
	struct clk_divider *div = NULL;
	struct owl_factor *factor = NULL;
	struct clk_hw *mux_hw = NULL;
	struct clk_hw *gate_hw = NULL;
	struct clk_hw *rate_hw = NULL;
	const struct clk_ops *rate_ops = NULL;
	const char *clk_name = cclk->name;
	const char **parent_names;
	int i, num_parents;

	amux = &cclk->mux;
	agate = &cclk->gate;
	arate = &cclk->rate;

	parent_names = NULL;
	num_parents = 0;

	if (amux->id) {
		num_parents = amux->num_parents;
		if (num_parents > 0) {
			parent_names = kzalloc((sizeof(char *) * num_parents),
					GFP_KERNEL);
			if (!parent_names)
				return ERR_PTR(-ENOMEM);

			for (i = 0; i < num_parents; i++)
				parent_names[i] = kstrdup(amux->parent_names[i],
						GFP_KERNEL);
		}

		mux = kzalloc(sizeof(*mux), GFP_KERNEL);
		if (!mux)
			return NULL;

		/* set up gate properties */
		mux->reg = ctx->reg_base + amux->offset;
		mux->shift = amux->shift;
		mux->mask = BIT(amux->width) - 1;
		mux->flags = amux->mux_flags;
		mux->lock = &ctx->lock;
		mux_hw = &mux->hw;
	}

	if (arate->fixed_factor.id) {
		switch (cclk->type) {
		case OWL_COMPOSITE_TYPE_FIXED_FACTOR:
			fixed_factor = kzalloc(sizeof(*fixed_factor),
					GFP_KERNEL);
			if (!fixed_factor)
				return NULL;
			fixed_factor->mult = arate->fixed_factor.mult;
			fixed_factor->div = arate->fixed_factor.div;

			rate_ops = &clk_fixed_factor_ops;
			rate_hw = &fixed_factor->hw;
			break;

		case OWL_COMPOSITE_TYPE_DIVIDER:
			div = kzalloc(sizeof(*div), GFP_KERNEL);
			if (!div)
				return NULL;
			div->reg = ctx->reg_base + arate->div.offset;
			div->shift = arate->div.shift;
			div->width = arate->div.width;
			div->flags = arate->div.div_flags;
			div->table = arate->div.table;
			div->lock = &ctx->lock;

			rate_ops = &clk_divider_ops;
			rate_hw = &div->hw;
			break;

		case OWL_COMPOSITE_TYPE_FACTOR:
			factor = kzalloc(sizeof(*factor), GFP_KERNEL);
			if (!factor)
				return NULL;
			factor->reg = ctx->reg_base + arate->factor.offset;
			factor->shift = arate->factor.shift;
			factor->width = arate->factor.width;
			factor->flags = arate->factor.div_flags;
			factor->table = arate->factor.table;
			factor->lock = &ctx->lock;

			rate_ops = &owl_factor_ops;
			rate_hw = &factor->hw;
			break;

		default:
			break;
		}
	}

	if (agate->id) {
		gate = kzalloc(sizeof(*gate), GFP_KERNEL);
		if (!gate)
			return ERR_PTR(-ENOMEM);

		/* set up gate properties */
		gate->reg = ctx->reg_base + agate->offset;
		gate->bit_idx = agate->bit_idx;
		gate->lock = &ctx->lock;
		gate_hw = &gate->hw;
	}

	clk_hw = clk_hw_register_composite(NULL, clk_name,
			parent_names, num_parents,
			mux_hw, &clk_mux_ops,
			rate_hw, rate_ops,
			gate_hw, &clk_gate_ops, cclk->flags);

	return clk_hw;
}

/* register a list of composite clocks */
void owl_clk_register_composite(struct owl_clk_provider *ctx,
		struct owl_composite_clock *clks, int nums)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = _register_composite(ctx, &clks[i]);
		if (IS_ERR(clk_hw)) {
			pr_err("%s: failed to register clock %s\n",
				__func__, clks[i].name);
			continue;
		}

		owl_clk_add_hw_data(ctx, clk_hw, clks[i].id);
	}
}
