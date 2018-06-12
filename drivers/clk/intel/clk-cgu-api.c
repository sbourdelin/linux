// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2016 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 *
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "clk-cgu-api.h"

#define to_gate_clk(_hw) container_of(_hw, struct gate_clk, hw)
#define to_clk_gate_dummy(_hw) container_of(_hw, struct gate_dummy_clk, hw)
#define to_div_clk(_hw) container_of(_hw, struct div_clk, hw)
#define to_mux_clk(_hw) container_of(_hw, struct mux_clk, hw)

static void set_clk_val(struct regmap *map, u32 reg, u8 shift,
			u8 width, u32 set_val)
{
	u32 mask = GENMASK(width + shift, shift);

	regmap_update_bits(map, reg, mask,
			   (set_val << shift));
}

static u32 get_clk_val(struct regmap *map, u32 reg, u8 shift,
		       u8 width)
{
	u32 val;

	regmap_read(map, reg, &val);
	val >>= shift;
	val &= (BIT(width) - 1);

	return val;
}

static struct regmap *regmap_from_node(struct device_node *np)
{
	struct regmap *map;

	for ( ; np; ) {
		np = of_get_parent(np);
		if (!np)
			return ERR_PTR(-EINVAL);

		map = syscon_node_to_regmap(np);
		if (!IS_ERR(map))
			return map;
	}

	return ERR_PTR(-EINVAL);
}

#define GATE_STAT_REG(reg)	(reg)
#define GATE_EN_REG(reg)	((reg) + 0x4)
#define GATE_DIS_REG(reg)	((reg) + 0x8)

static int get_gate(struct regmap *map, u32 reg, u8 shift)
{
	unsigned int val;

	regmap_read(map, GATE_STAT_REG(reg), &val);

	return !!(val & BIT(shift));
}

static void set_gate(struct regmap *map, u32 reg, u8 shift, int enable)
{
	if (enable)
		regmap_write(map, GATE_EN_REG(reg), BIT(shift));
	else
		regmap_write(map, GATE_DIS_REG(reg), BIT(shift));
}

void
intel_fixed_rate_clk_setup(struct device_node *node,
			   const struct fixed_rate_clk_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;
	unsigned long rate;
	struct regmap *regmap;
	u32 reg;

	if (!data)
		return;

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;

	if (of_property_read_u32(node, "clock-frequency", (u32 *)&rate))
		rate = data->fixed_rate;
	if (!rate) {
		pr_err("clk(%s): Could not get fixed rate\n", clk_name);
		return;
	}

	regmap = regmap_from_node(node);
	if (IS_ERR(regmap))
		return;

	/* Register the fixed rate clock */
	clk = clk_register_fixed_rate(NULL, clk_name, NULL, 0, rate);
	if (IS_ERR(clk))
		return;

	/* Clock init */
	if (of_property_read_u32(node, "reg", &reg)) {
		pr_err("%s no reg definition\n", node->name);
		return;
	}

	set_clk_val(regmap, reg, data->shift, data->width, data->setval);

	/* Register clock provider */
	of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

static int gate_clk_enable(struct clk_hw *hw)
{
	struct gate_clk *clk;

	clk = to_gate_clk(hw);
	set_gate(clk->map, clk->reg, clk->bit_idx, 1);
	return 0;
}

static void gate_clk_disable(struct clk_hw *hw)
{
	struct gate_clk *clk;

	clk = to_gate_clk(hw);
	set_gate(clk->map, clk->reg, clk->bit_idx, 0);
}

static int gate_clk_is_enabled(struct clk_hw *hw)
{
	struct gate_clk *clk;

	clk = to_gate_clk(hw);
	return get_gate(clk->map, clk->reg, clk->bit_idx);
}

static const struct clk_ops gate_clk_ops = {
	.enable = gate_clk_enable,
	.disable = gate_clk_disable,
	.is_enabled = gate_clk_is_enabled,
};

static struct clk *gate_clk_register(struct device *dev, const char *name,
				     const char *parent_name,
				     unsigned long flags,
				     struct regmap *map, unsigned int reg,
				     u8 bit_idx, unsigned int clk_gate_flags)
{
	struct gate_clk *gate;
	struct clk *clk;
	struct clk_init_data init;

	/* Allocate the gate */
	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &gate_clk_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);

	/* Struct gate_clk assignments */
	gate->map = map;
	gate->reg = reg;
	gate->bit_idx = bit_idx;
	gate->flags = clk_gate_flags;
	gate->hw.init = &init;

	clk = clk_register(dev, &gate->hw);
	if (IS_ERR(clk)) {
		pr_err("0x%4x %d %s %d %d %s\n", (u32)reg, init.num_parents,
		       parent_name, bit_idx, clk_gate_flags, name);
		kfree(gate);
	}

	return clk;
}

void
intel_gate_clk_setup(struct device_node *node, const struct gate_clk_data *data)
{
	struct clk_onecell_data *clk_data;
	const char *clk_parent;
	const char *clk_name = node->name;
	struct regmap *regmap;
	unsigned int reg;
	int i, j, num;
	unsigned int val;

	if (!data || !data->reg_size) {
		pr_err("%s: register bit size cannot be 0!\n", __func__);
		return;
	}

	regmap = regmap_from_node(node);
	if (IS_ERR(regmap))
		return;

	if (of_property_read_u32(node, "reg", &reg)) {
		pr_err("%s no reg definition\n", node->name);
		return;
	}

	clk_parent = of_clk_get_parent_name(node, 0);

	/* Size probe and memory allocation */
	num = find_last_bit(&data->mask, data->reg_size);
	clk_data = kmalloc(sizeof(*clk_data), GFP_KERNEL);
	if (!clk_data)
		return;

	clk_data->clks = kcalloc(num + 1, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		goto __clkarr_alloc_fail;

	i = 0;
	j = 0;
	for_each_set_bit(i, &data->mask, data->reg_size) {
		of_property_read_string_index(node, "clock-output-names",
					      j, &clk_name);

		clk_data->clks[j] = gate_clk_register(NULL, clk_name,
						      clk_parent, 0, regmap,
						      reg, i, 0);
		WARN_ON(IS_ERR(clk_data->clks[j]));

		j++;
	}

	/* Adjust to the real max */
	clk_data->clk_num = num + 1;

	/* Initial gate default setting */
	if (data->flags & CLK_INIT_DEF_CFG_REQ) {
		val = (unsigned int)data->def_onoff;
		if (val)
			regmap_write(regmap, GATE_EN_REG(reg), val);
		val = (((unsigned int)(~data->def_onoff)) & data->mask);
		if (val)
			regmap_write(regmap, GATE_DIS_REG(reg), val);
	}

	/* Register to clock provider */
	of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
	return;

__clkarr_alloc_fail:
	kfree(clk_data);
}

static unsigned long div_recalc_rate(struct div_clk *div,
				     unsigned long parent_rate,
				     unsigned int val)
{
	return divider_recalc_rate(&div->hw, parent_rate,
			val, div->div_table, div->flags, div->width);
}

static unsigned long div_clk_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct div_clk *div = to_div_clk(hw);
	unsigned int val;

	val = get_clk_val(div->map, div->reg, div->shift, div->width);

	return div_recalc_rate(div, parent_rate, val);
}

static long div_clk_round_rate(struct clk_hw *hw,
			       unsigned long rate, unsigned long *prate)
{
	struct div_clk *div = to_div_clk(hw);

	return divider_round_rate(hw, rate, prate,
				  div->div_table, div->width, div->flags);
}

static int div_set_rate(struct div_clk *div, unsigned long rate,
			unsigned long parent_rate)
{
	unsigned int val;

	val = divider_get_val(rate, parent_rate, div->div_table,
			      div->width, div->flags);
	set_clk_val(div->map, div->reg, div->shift, div->width, val);

	return 0;
}

static int div_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct div_clk *div = to_div_clk(hw);

	return div_set_rate(div, rate, parent_rate);
}

static const struct clk_ops clk_div_ops = {
	.recalc_rate = div_clk_recalc_rate,
	.round_rate = div_clk_round_rate,
	.set_rate = div_clk_set_rate,
};

static struct clk *div_clk_register(struct device *dev, const char *name,
				    const char *parent_name,
				    unsigned long flags,
				    struct regmap *map, unsigned int reg,
				    const struct div_clk_data *data)
{
	struct div_clk *div;
	struct clk *clk;
	struct clk_init_data init;

	/* Allocate the divider clock*/
	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_div_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);

	/* Struct clk_divider assignments */
	div->map = map;
	div->reg = reg;
	div->shift = data->shift;
	div->width = data->width;
	div->flags = data->flags;
	div->div_table = data->div_table;
	div->hw.init = &init;
	div->tbl_sz = data->tbl_sz;

	/* Register the clock */
	clk = clk_register(dev, &div->hw);
	if (IS_ERR(clk))
		kfree(div);

	return clk;
}

void
intel_div_clk_setup(struct device_node *node, const struct div_clk_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *clk_parent;
	struct regmap *map;
	unsigned int reg;

	if (!data)
		return;

	map = regmap_from_node(node);
	if (IS_ERR(map))
		return;

	if (of_property_read_u32(node, "reg", &reg)) {
		pr_err("%s no reg definition\n", node->name);
		return;
	}

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;
	clk_parent = of_clk_get_parent_name(node, 0);

	clk = div_clk_register(NULL, clk_name, clk_parent, 0, map, reg, data);
	if (IS_ERR(clk))
		return;

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

void
intel_cluster_div_clk_setup(struct device_node *node,
			    const struct div_clk_data *data, u32 num)
{
	struct clk_onecell_data *clk_data;
	const char *clk_name;
	const char *clk_parent;
	struct regmap *regmap;
	unsigned int reg;
	int i;

	if (!data || !num) {
		pr_err("%s: invalid array or array size!\n", __func__);
		return;
	}

	regmap = regmap_from_node(node);
	if (IS_ERR(regmap))
		return;

	if (of_property_read_u32(node, "reg", &reg)) {
		pr_err("%s no reg definition\n", node->name);
		return;
	}

	clk_data = kmalloc(sizeof(*clk_data), GFP_KERNEL);
	if (!clk_data)
		return;
	clk_data->clks = kcalloc(num, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		goto __clkarr_alloc_fail;

	clk_parent = of_clk_get_parent_name(node, 0);

	for (i = 0; i < num; i++) {
		of_property_read_string_index(node, "clock-output-names",
					      i, &clk_name);
		clk_data->clks[i] = div_clk_register(NULL, clk_name, clk_parent,
						     0, regmap, reg, &data[i]);
		WARN_ON(IS_ERR(clk_data->clks[i]));
	}
	clk_data->clk_num = num + 1;

	of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
	return;

__clkarr_alloc_fail:
	kfree(clk_data);
}

static unsigned int mux_parent_from_table(const u32 *table,
					  unsigned int val, unsigned int num)
{
	unsigned int i;

	for (i = 0; i < num; i++)
		if (table[i] == val)
			return i;

	return -EINVAL;
}

static u8 mux_clk_get_parent(struct clk_hw *hw)
{
	struct mux_clk *mux = to_mux_clk(hw);
	int num_parents = clk_hw_get_num_parents(hw);
	unsigned int val;

	val = get_clk_val(mux->map, mux->reg, mux->shift, mux->width);
	if (mux->table)
		return mux_parent_from_table(mux->table, val, num_parents);

	if (val && (mux->flags & CLK_MUX_INDEX_BIT))
		val = ffs(val) - 1;
	if (val && (mux->flags & CLK_MUX_INDEX_ONE))
		val -= 1;
	if (val >= num_parents)
		return -EINVAL;

	return val;
}

static int mux_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct mux_clk *mux = to_mux_clk(hw);

	if (mux->table) {
		index = mux->table[index];
	} else {
		if (mux->flags & CLK_MUX_INDEX_BIT)
			index = BIT(index);
		if (mux->flags & CLK_MUX_INDEX_ONE)
			index += 1;
	}

	set_clk_val(mux->map, mux->reg, mux->shift, mux->width, index);

	return 0;
}

static const struct clk_ops mux_clk_ops = {
	.get_parent = mux_clk_get_parent,
	.set_parent = mux_clk_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};

static struct clk *mux_clk_register(struct device *dev, const char *name,
				    const char * const *parent_names,
				    u8 num_parents, unsigned long flags,
				    struct regmap *map, unsigned int reg,
				    const struct mux_clk_data *data)
{
	struct mux_clk *mux;
	struct clk_init_data init;
	struct clk *clk;

	/* allocate mux clk */
	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);

	/* struct init assignments */
	init.name = name;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = parent_names;
	init.num_parents = num_parents;
	init.ops = &mux_clk_ops;

	/* struct mux_clk assignments */
	mux->map = map;
	mux->reg = reg;
	mux->shift = data->shift;
	mux->width = data->width;
	mux->flags = data->clk_flags;
	mux->table = data->table;
	mux->hw.init = &init;

	clk = clk_register(dev, &mux->hw);
	if (IS_ERR(clk))
		kfree(mux);

	return clk;
}

void
intel_mux_clk_setup(struct device_node *node, const struct mux_clk_data *data)
{
	struct clk *clk;
	const char *clk_name;
	const char **parents;
	unsigned int num_parents;
	struct regmap *map;
	unsigned int reg;

	if (!data)
		return;

	map = regmap_from_node(node);
	if (IS_ERR(map))
		return;

	if (of_property_read_string(node, "clock-output-names", &clk_name)) {
		pr_err("%s: no output clock name!\n", node->name);
		return;
	}

	if (of_property_read_u32(node, "reg", &reg)) {
		pr_err("%s no reg definition\n", node->name);
		return;
	}

	num_parents = of_clk_get_parent_count(node);
	if (num_parents) {
		parents = kmalloc_array(num_parents,
					sizeof(char *), GFP_KERNEL);
		if (!parents)
			return;
		of_clk_parent_fill(node, parents, num_parents);
	} else {
		pr_err("%s: mux clk no parent!\n", __func__);
		return;
	}

	clk = mux_clk_register(NULL, clk_name, parents, num_parents,
			       data->flags, map, reg, data);
	if (IS_ERR(clk))
		goto __mux_clk_fail;

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
	return;

__mux_clk_fail:
	kfree(parents);
}

static int gate_clk_dummy_enable(struct clk_hw *hw)
{
	struct gate_dummy_clk *clk;

	clk = to_clk_gate_dummy(hw);
	clk->clk_status = 1;

	return 0;
}

static void gate_clk_dummy_disable(struct clk_hw *hw)
{
	struct gate_dummy_clk *clk;

	clk = to_clk_gate_dummy(hw);
	clk->clk_status = 0;
}

static int gate_clk_dummy_is_enabled(struct clk_hw *hw)
{
	struct gate_dummy_clk *clk;

	clk = to_clk_gate_dummy(hw);
	return clk->clk_status;
}

static const struct clk_ops clk_gate_dummy_ops = {
	.enable = gate_clk_dummy_enable,
	.disable = gate_clk_dummy_disable,
	.is_enabled = gate_clk_dummy_is_enabled,
};

static struct clk
*clk_register_gate_dummy(struct device *dev,
			 const char *name,
			 const char *parent_name,
			 unsigned long flags,
			 const struct gate_dummy_clk_data *data)
{
	struct gate_dummy_clk *gate_clk;
	struct clk *clk;
	struct clk_init_data init;

	/* Allocate the gate_dummy clock*/
	gate_clk = kzalloc(sizeof(*gate_clk), GFP_KERNEL);
	if (!gate_clk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_gate_dummy_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);
	gate_clk->hw.init = &init;

	/* Struct gate_clk assignments */
	if (data->flags & CLK_INIT_DEF_CFG_REQ)
		gate_clk->clk_status = data->def_val & 0x1;

	/* Register the clock */
	clk = clk_register(dev, &gate_clk->hw);
	if (IS_ERR(clk))
		kfree(gate_clk);

	return clk;
}

void
intel_gate_dummy_clk_setup(struct device_node *node,
			   const struct gate_dummy_clk_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;

	if (!data)
		return;

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;

	clk = clk_register_gate_dummy(NULL, clk_name, NULL, 0, data);
	if (IS_ERR(clk)) {
		pr_err("%s: dummy gate clock register fail!\n", __func__);
		return;
	}

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
}
