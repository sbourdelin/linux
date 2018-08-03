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

#define GATE_HW_REG_STAT(reg)	(reg)
#define GATE_HW_REG_EN(reg)	((reg) + 0x4)
#define GATE_HW_REG_DIS(reg)	((reg) + 0x8)

#define to_intel_clk_mux(_hw) container_of(_hw, struct intel_clk_mux, hw)
#define to_intel_clk_divider(_hw) \
		container_of(_hw, struct intel_clk_divider, hw)
#define to_intel_clk_gate(_hw) container_of(_hw, struct intel_clk_gate, hw)

void intel_set_clk_val(struct regmap *map, u32 reg, u8 shift,
		       u8 width, u32 set_val)
{
	u32 mask = GENMASK(width + shift, shift);

	regmap_update_bits(map, reg, mask, set_val << shift);
}

u32 intel_get_clk_val(struct regmap *map, u32 reg, u8 shift,
		      u8 width)
{
	u32 val;

	if (regmap_read(map, reg, &val)) {
		WARN_ONCE(1, "Failed to read clk reg: 0x%x\n", reg);
		return 0;
	}
	val >>= shift;
	val &= BIT(width) - 1;

	return val;
}

void intel_clk_add_lookup(struct intel_clk_provider *ctx,
			  struct clk *clk, unsigned int id)
{
	pr_debug("Add clk: %s, id: %u\n", __clk_get_name(clk), id);
	if (ctx->clk_data.clks && id)
		ctx->clk_data.clks[id] = clk;
}

static struct clk
*intel_clk_register_fixed(struct intel_clk_provider *ctx,
			  struct intel_clk_branch *list)
{
	if (list->div_flags & CLOCK_FLAG_VAL_INIT)
		intel_set_clk_val(ctx->map, list->div_off, list->div_shift,
				  list->div_width, list->div_val);

	return clk_register_fixed_rate(NULL, list->name, list->parent_names[0],
				       list->flags, list->mux_flags);
}

static u8 intel_clk_mux_get_parent(struct clk_hw *hw)
{
	struct intel_clk_mux *mux = to_intel_clk_mux(hw);
	u32 val;

	val = intel_get_clk_val(mux->map, mux->reg, mux->shift, mux->width);
	return clk_mux_val_to_index(hw, NULL, mux->flags, val);
}

static int intel_clk_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct intel_clk_mux *mux = to_intel_clk_mux(hw);
	u32 val;

	val = clk_mux_index_to_val(NULL, mux->flags, index);
	intel_set_clk_val(mux->map, mux->reg, mux->shift, mux->width, val);

	return 0;
}

static int intel_clk_mux_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	struct intel_clk_mux *mux = to_intel_clk_mux(hw);

	return clk_mux_determine_rate_flags(hw, req, mux->flags);
}

const static struct clk_ops intel_clk_mux_ops = {
	.get_parent = intel_clk_mux_get_parent,
	.set_parent = intel_clk_mux_set_parent,
	.determine_rate = intel_clk_mux_determine_rate,
};

static struct clk
*intel_clk_register_mux(struct intel_clk_provider *ctx,
			struct intel_clk_branch *list)
{
	struct clk_init_data init;
	struct clk_hw *hw;
	struct intel_clk_mux *mux;
	u32 reg = list->mux_off;
	u8 shift = list->mux_shift;
	u8 width = list->mux_width;
	unsigned long cflags = list->mux_flags;
	int ret;

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);

	init.name = list->name;
	init.ops = &intel_clk_mux_ops;
	init.flags = list->flags | CLK_IS_BASIC;
	init.parent_names = list->parent_names;
	init.num_parents = list->num_parents;

	mux->map = ctx->map;
	mux->reg = reg;
	mux->shift = shift;
	mux->width = width;
	mux->flags = cflags;
	mux->hw.init = &init;

	hw = &mux->hw;
	ret = clk_hw_register(NULL, hw);
	if (ret) {
		kfree(mux);
		return ERR_PTR(ret);
	}

	if (cflags & CLOCK_FLAG_VAL_INIT)
		intel_set_clk_val(ctx->map, reg, shift, width, list->mux_val);

	return hw->clk;
}

static unsigned long
intel_clk_divider_recalc_rate(struct clk_hw *hw,
			      unsigned long parent_rate)
{
	struct intel_clk_divider *divider = to_intel_clk_divider(hw);
	unsigned int val;

	val = intel_get_clk_val(divider->map, divider->reg,
				divider->shift, divider->width);
	return divider_recalc_rate(hw, parent_rate, val, divider->table,
				   divider->flags, divider->width);
}

static long
intel_clk_divider_round_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long *prate)
{
	struct intel_clk_divider *divider = to_intel_clk_divider(hw);

	return divider_round_rate(hw, rate, prate, divider->table,
				  divider->width, divider->flags);
}

static int
intel_clk_divider_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long prate)
{
	struct intel_clk_divider *divider = to_intel_clk_divider(hw);
	int value;

	value = divider_get_val(rate, prate, divider->table,
				divider->width, divider->flags);
	if (value < 0)
		return value;

	intel_set_clk_val(divider->map, divider->reg,
			  divider->shift, divider->width, value);

	return 0;
}

const static struct clk_ops intel_clk_divider_ops = {
	.recalc_rate = intel_clk_divider_recalc_rate,
	.round_rate = intel_clk_divider_round_rate,
	.set_rate = intel_clk_divider_set_rate,
};

static struct clk
*intel_clk_register_divider(struct intel_clk_provider *ctx,
			    struct intel_clk_branch *list)
{
	struct clk_init_data init;
	struct clk_hw *hw;
	struct intel_clk_divider *div;
	u32 reg = list->div_off;
	u8 shift = list->div_shift;
	u8 width = list->div_width;
	unsigned long cflags = list->div_flags;
	int ret;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	init.name = list->name;
	init.ops = &intel_clk_divider_ops;
	init.flags = list->flags | CLK_IS_BASIC;
	init.parent_names = &list->parent_names[0];
	init.num_parents = 1;

	div->map = ctx->map;
	div->reg = reg;
	div->shift = shift;
	div->width = width;
	div->flags = cflags;
	div->table = list->div_table;
	div->hw.init = &init;

	hw = &div->hw;
	ret = clk_hw_register(NULL, hw);
	if (ret) {
		pr_err("%s: register clk: %s failed!\n",
		       __func__, list->name);
		kfree(div);
		return ERR_PTR(ret);
	}

	if (cflags & CLOCK_FLAG_VAL_INIT)
		intel_set_clk_val(ctx->map, reg, shift, width, list->div_val);

	return hw->clk;
}

static struct clk
*intel_clk_register_fixed_factor(struct intel_clk_provider *ctx,
				 struct intel_clk_branch *list)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fixed_factor(NULL, list->name,
					  list->parent_names[0], list->flags,
					  list->mult, list->div);
	if (IS_ERR(hw))
		return ERR_CAST(hw);

	if (list->div_flags & CLOCK_FLAG_VAL_INIT)
		intel_set_clk_val(ctx->map, list->div_off, list->div_shift,
				  list->div_width, list->div_val);

	return hw->clk;
}

static int
intel_clk_gate_enable(struct clk_hw *hw)
{
	struct intel_clk_gate *gate = to_intel_clk_gate(hw);
	unsigned int reg;

	if (gate->flags & GATE_CLK_VT) {
		gate->reg = 1;
		return 0;
	}

	if (gate->flags & GATE_CLK_HW) {
		reg = GATE_HW_REG_EN(gate->reg);
	} else if (gate->flags & GATE_CLK_SW) {
		reg = gate->reg;
	} else {
		pr_err("%s: gate clk: %s: flag 0x%lx not supported!\n",
		       __func__, clk_hw_get_name(hw), gate->flags);
		return 0;
	}

	intel_set_clk_val(gate->map, reg, gate->shift, 1, 1);

	return 0;
}

static void
intel_clk_gate_disable(struct clk_hw *hw)
{
	struct intel_clk_gate *gate = to_intel_clk_gate(hw);
	unsigned int reg;
	unsigned int set;

	if (gate->flags & GATE_CLK_VT) {
		gate->reg = 0;
		return;
	}

	if (gate->flags & GATE_CLK_HW) {
		reg = GATE_HW_REG_DIS(gate->reg);
		set = 1;
	} else if (gate->flags & GATE_CLK_SW) {
		reg = gate->reg;
		set = 0;
	} else {
		pr_err("%s: gate clk: %s: flag 0x%lx not supported!\n",
		       __func__, clk_hw_get_name(hw), gate->flags);
		return;
	}

	intel_set_clk_val(gate->map, reg, gate->shift, 1, set);
}

static int
intel_clk_gate_is_enabled(struct clk_hw *hw)
{
	struct intel_clk_gate *gate = to_intel_clk_gate(hw);
	unsigned int reg;

	if (gate->flags & GATE_CLK_VT)
		return gate->reg;

	if (gate->flags & GATE_CLK_HW) {
		reg = GATE_HW_REG_STAT(gate->reg);
	} else if (gate->flags & GATE_CLK_SW) {
		reg = gate->reg;
	} else {
		pr_err("%s: gate clk: %s: flag 0x%lx not supported!\n",
		       __func__, clk_hw_get_name(hw), gate->flags);
		return 0;
	}

	return intel_get_clk_val(gate->map, reg, gate->shift, 1);
}

const static struct clk_ops intel_clk_gate_ops = {
	.enable = intel_clk_gate_enable,
	.disable = intel_clk_gate_disable,
	.is_enabled = intel_clk_gate_is_enabled,
};

static struct clk
*intel_clk_register_gate(struct intel_clk_provider *ctx,
			 struct intel_clk_branch *list)
{
	struct clk_init_data init;
	struct clk_hw *hw;
	struct intel_clk_gate *gate;
	u32 reg = list->gate_off;
	u8 shift = list->gate_shift;
	unsigned long cflags = list->gate_flags;
	const char *pname = list->parent_names[0];
	int ret;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = list->name;
	init.ops = &intel_clk_gate_ops;
	init.flags = list->flags | CLK_IS_BASIC;
	init.parent_names = pname ? &pname : NULL;
	init.num_parents = pname ? 1 : 0;

	gate->map	= ctx->map;
	gate->reg	= reg;
	gate->shift	= shift;
	gate->flags	= cflags;
	gate->hw.init	= &init;

	hw = &gate->hw;
	ret = clk_hw_register(NULL, hw);
	if (ret) {
		kfree(gate);
		return ERR_PTR(ret);
	}

	if (cflags & CLOCK_FLAG_VAL_INIT)
		intel_set_clk_val(ctx->map, reg, shift, 1, list->gate_val);

	return hw->clk;
}

void intel_clk_register_branches(struct intel_clk_provider *ctx,
				 struct intel_clk_branch *list,
				 unsigned int nr_clk)
{
	struct clk *clk;
	unsigned int idx;

	for (idx = 0; idx < nr_clk; idx++, list++) {
		switch (list->type) {
		case intel_clk_fixed:
			clk = intel_clk_register_fixed(ctx, list);
			break;
		case intel_clk_mux:
			clk = intel_clk_register_mux(ctx, list);
			break;
		case intel_clk_divider:
			clk = intel_clk_register_divider(ctx, list);
			break;
		case intel_clk_fixed_factor:
			clk = intel_clk_register_fixed_factor(ctx, list);
			break;
		case intel_clk_gate:
			clk = intel_clk_register_gate(ctx, list);
			break;
		default:
			pr_err("%s: type: %u not supported!\n",
			       __func__, list->type);
			return;
		}

		if (IS_ERR(clk)) {
			pr_err("%s: register clk: %s, type: %u failed!\n",
			       __func__, list->name, list->type);
			return;
		}

		intel_clk_add_lookup(ctx, clk, list->id);
	}
}

struct intel_clk_provider * __init
intel_clk_init(struct device_node *np, struct regmap *map, unsigned int nr_clks)
{
	struct intel_clk_provider *ctx;
	struct clk **clks;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	clks = kcalloc(nr_clks, sizeof(*clks), GFP_KERNEL);
	if (!clks) {
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	memset_p((void **)clks, ERR_PTR(-ENOENT), nr_clks);
	ctx->map = map;
	ctx->clk_data.clks = clks;
	ctx->clk_data.clk_num = nr_clks;
	ctx->np = np;

	return ctx;
}

void __init intel_clk_register_osc(struct intel_clk_provider *ctx,
				   struct intel_osc_clk *osc,
				   unsigned int nr_clks)
{
	u32 freq;
	struct clk *clk;
	int idx;

	for (idx = 0; idx < nr_clks; idx++, osc++) {
		if (!osc->dt_freq ||
		    of_property_read_u32(ctx->np, osc->dt_freq, &freq))
			freq = osc->def_rate;

		clk = clk_register_fixed_rate(NULL, osc->name, NULL, 0, freq);
		if (IS_ERR(clk)) {
			pr_err("%s: Failed to register clock: %s\n",
			       __func__, osc->name);
			return;
		}

		intel_clk_add_lookup(ctx, clk, osc->id);
	}
}
