// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 NXP
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>

#include "clk.h"

#define PCG_PREDIV_SHIFT	16
#define PCG_PREDIV_WIDTH	3

#define PCG_DIV_SHIFT		0
#define PCG_DIV_WIDTH		6

#define PCG_PCS_SHIFT		24
#define PCG_PCS_MASK		0x7

#define PCG_CGC_SHIFT		28

#define to_clk_imx_composite(_hw) \
		container_of(_hw, struct imx_clk_composite, hw)

struct imx_clk_composite {
	struct clk_hw	hw;
	struct clk_ops	ops;

	struct clk_hw	*mux_hw;
	struct clk_hw	*prediv_hw;
	struct clk_hw	*div_hw;
	struct clk_hw	*gate_hw;

	const struct clk_ops	*mux_ops;
	const struct clk_ops	*prediv_ops;
	const struct clk_ops	*div_ops;
	const struct clk_ops	*gate_ops;
};

static u8 clk_imx_composite_get_parent(struct clk_hw *hw)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *mux_ops = clk->mux_ops;
	struct clk_hw *mux_hw = clk->mux_hw;

	__clk_hw_set_clk(mux_hw, hw);

	return mux_ops->get_parent(mux_hw);
}

static int clk_imx_composite_set_parent(struct clk_hw *hw, u8 index)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *mux_ops = clk->mux_ops;
	struct clk_hw *mux_hw = clk->mux_hw;

	__clk_hw_set_clk(mux_hw, hw);

	return mux_ops->set_parent(mux_hw, index);
}

static unsigned long clk_imx_composite_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *div_ops = clk->div_ops;
	struct clk_hw *div_hw = clk->div_hw;

	__clk_hw_set_clk(div_hw, hw);

	return div_ops->recalc_rate(div_hw, parent_rate);
}

static int clk_imx_composite_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *div_ops = clk->div_ops;
	const struct clk_ops *mux_ops = clk->mux_ops;
	struct clk_hw *div_hw = clk->div_hw;
	struct clk_hw *mux_hw = clk->mux_hw;
	struct clk_hw *parent;
	unsigned long parent_rate;
	long tmp_rate, best_rate = 0;
	unsigned long rate_diff;
	unsigned long best_rate_diff = ULONG_MAX;
	long rate;
	int i;

	if (div_hw && div_ops && div_ops->determine_rate) {
		__clk_hw_set_clk(div_hw, hw);
		return div_ops->determine_rate(div_hw, req);
	} else if (div_hw && div_ops && div_ops->round_rate &&
		   mux_hw && mux_ops && mux_ops->set_parent) {
		req->best_parent_hw = NULL;

		if (clk_hw_get_flags(hw) & CLK_SET_RATE_NO_REPARENT) {
			parent = clk_hw_get_parent(mux_hw);
			req->best_parent_hw = parent;
			req->best_parent_rate = clk_hw_get_rate(parent);

			rate = div_ops->round_rate(div_hw, req->rate,
						    &req->best_parent_rate);
			if (rate < 0)
				return rate;

			req->rate = rate;
			return 0;
		}

		for (i = 0; i < clk_hw_get_num_parents(mux_hw); i++) {
			parent = clk_hw_get_parent_by_index(mux_hw, i);
			if (!parent)
				continue;

			parent_rate = clk_hw_get_rate(parent);

			tmp_rate = div_ops->round_rate(div_hw, req->rate,
							&parent_rate);
			if (tmp_rate < 0)
				continue;

			rate_diff = abs(req->rate - tmp_rate);

			if (!rate_diff || !req->best_parent_hw
				       || best_rate_diff > rate_diff) {
				req->best_parent_hw = parent;
				req->best_parent_rate = parent_rate;
				best_rate_diff = rate_diff;
				best_rate = tmp_rate;
			}

			if (!rate_diff)
				return 0;
		}

		req->rate = best_rate;
		return 0;
	} else if (mux_hw && mux_ops && mux_ops->determine_rate) {
		__clk_hw_set_clk(mux_hw, hw);
		return mux_ops->determine_rate(mux_hw, req);
	} else {
		pr_err("clk: %s function called, but no mux or rate callback set!\n", __func__);
		return -EINVAL;
	}
}

static long clk_imx_composite_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *prate)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *div_ops = clk->div_ops;
	struct clk_hw *div_hw = clk->div_hw;

	__clk_hw_set_clk(div_hw, hw);

	return div_ops->round_rate(div_hw, rate, prate);
}

static int clk_imx_composite_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *div_ops = clk->div_ops;
	struct clk_hw *div_hw = clk->div_hw;

	__clk_hw_set_clk(div_hw, hw);

	return div_ops->set_rate(div_hw, rate, parent_rate);
}

static int clk_imx_composite_set_rate_and_parent(struct clk_hw *hw,
					     unsigned long rate,
					     unsigned long parent_rate,
					     u8 index)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *prediv_ops = clk->prediv_ops;
	const struct clk_ops *div_ops = clk->div_ops;
	const struct clk_ops *mux_ops = clk->mux_ops;
	struct clk_hw *prediv_hw = clk->prediv_hw;
	struct clk_hw *div_hw = clk->div_hw;
	struct clk_hw *mux_hw = clk->mux_hw;
	unsigned long temp_rate;

	__clk_hw_set_clk(prediv_hw, hw);
	__clk_hw_set_clk(mux_hw, hw);


	temp_rate = prediv_ops->recalc_rate(div_hw, parent_rate);
	if (temp_rate > rate) {
		prediv_ops->set_rate(div_hw, rate, parent_rate);
	} else {
		mux_ops->set_parent(mux_hw, index);
		prediv_ops->set_rate(div_hw, rate, parent_rate);
	}

	temp_rate = div_ops->recalc_rate(div_hw, parent_rate);
	if (temp_rate > rate) {
		div_ops->set_rate(div_hw, rate, parent_rate);
		mux_ops->set_parent(mux_hw, index);
	} else {
		div_ops->set_rate(div_hw, rate, parent_rate);
	}

	return 0;
}

static int clk_imx_composite_is_enabled(struct clk_hw *hw)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *gate_ops = clk->gate_ops;
	struct clk_hw *gate_hw = clk->gate_hw;

	__clk_hw_set_clk(gate_hw, hw);

	return gate_ops->is_enabled(gate_hw);
}

static int clk_imx_composite_enable(struct clk_hw *hw)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *gate_ops = clk->gate_ops;
	struct clk_hw *gate_hw = clk->gate_hw;

	__clk_hw_set_clk(gate_hw, hw);

	return gate_ops->enable(gate_hw);
}

static void clk_imx_composite_disable(struct clk_hw *hw)
{
	struct imx_clk_composite *clk = to_clk_imx_composite(hw);
	const struct clk_ops *gate_ops = clk->gate_ops;
	struct clk_hw *gate_hw = clk->gate_hw;

	__clk_hw_set_clk(gate_hw, hw);

	gate_ops->disable(gate_hw);
}

struct clk_hw *clk_hw_register_imx_composite(struct device *dev, const char *name,
			const char * const *parent_names, int num_parents,
			struct clk_hw *mux_hw, const struct clk_ops *mux_ops,
			struct clk_hw *prediv_hw, const struct clk_ops *prediv_ops,
			struct clk_hw *div_hw, const struct clk_ops *div_ops,
			struct clk_hw *gate_hw, const struct clk_ops *gate_ops,
			unsigned long flags)
{
	struct clk_hw *hw;
	struct clk_init_data init;
	struct imx_clk_composite *clk_imx8;
	struct clk_ops *composite_ops;
	int ret;

	clk_imx8 = kzalloc(sizeof(*clk_imx8), GFP_KERNEL);
	if (!clk_imx8)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = parent_names;
	init.num_parents = num_parents;
	hw = &clk_imx8->hw;

	composite_ops = &clk_imx8->ops;

	if (mux_hw && mux_ops) {
		if (!mux_ops->get_parent) {
			hw = ERR_PTR(-EINVAL);
			goto err;
		}

		clk_imx8->mux_hw = mux_hw;
		clk_imx8->mux_ops = mux_ops;
		composite_ops->get_parent = clk_imx_composite_get_parent;
		if (mux_ops->set_parent)
			composite_ops->set_parent =
					clk_imx_composite_set_parent;
		if (mux_ops->determine_rate)
			composite_ops->determine_rate =
					clk_imx_composite_determine_rate;
	}

	if (prediv_hw && prediv_ops && div_hw && div_ops) {
		if (!div_ops->recalc_rate) {
			hw = ERR_PTR(-EINVAL);
			goto err;
		}
		composite_ops->recalc_rate = clk_imx_composite_recalc_rate;

		if (prediv_ops->determine_rate)
			composite_ops->determine_rate =
				clk_imx_composite_determine_rate;
		else if (prediv_ops->round_rate)
			composite_ops->round_rate =
				clk_imx_composite_round_rate;

		/* .set_rate requires either .round_rate or .determine_rate */
		if (prediv_ops->set_rate) {
			if (prediv_ops->determine_rate || prediv_ops->round_rate)
				composite_ops->set_rate =
						clk_imx_composite_set_rate;
			else
				WARN(1, "%s: missing round_rate op is required\n",
						__func__);
		}

		clk_imx8->prediv_hw = prediv_hw;
		clk_imx8->prediv_ops = prediv_ops;
		clk_imx8->div_hw = div_hw;
		clk_imx8->div_ops = div_ops;
	}

	if (mux_hw && mux_ops) {
		if ((prediv_hw && prediv_ops) || (div_hw && div_ops)) {
			if (mux_ops->set_parent &&
				(prediv_ops->set_rate && div_ops->set_rate))
				composite_ops->set_rate_and_parent =
				clk_imx_composite_set_rate_and_parent;
		}
	}

	if (gate_hw && gate_ops) {
		if (!gate_ops->is_enabled || !gate_ops->enable ||
		    !gate_ops->disable) {
			hw = ERR_PTR(-EINVAL);
			goto err;
		}

		clk_imx8->gate_hw = gate_hw;
		clk_imx8->gate_ops = gate_ops;
		composite_ops->is_enabled = clk_imx_composite_is_enabled;
		composite_ops->enable = clk_imx_composite_enable;
		composite_ops->disable = clk_imx_composite_disable;
	}

	init.ops = composite_ops;
	clk_imx8->hw.init = &init;

	ret = clk_hw_register(dev, hw);
	if (ret) {
		hw = ERR_PTR(ret);
		goto err;
	}

	if (clk_imx8->mux_hw)
		clk_imx8->mux_hw->clk = hw->clk;

	if (clk_imx8->prediv_hw)
		clk_imx8->prediv_hw->clk = hw->clk;

	if (clk_imx8->div_hw)
		clk_imx8->div_hw->clk = hw->clk;

	if (clk_imx8->gate_hw)
		clk_imx8->gate_hw->clk = hw->clk;

	return hw;

err:
	kfree(clk_imx8);
	return hw;
}

struct clk *clk_register_imx_composite(struct device *dev, const char *name,
			const char * const *parent_names, int num_parents,
			struct clk_hw *mux_hw, const struct clk_ops *mux_ops,
			struct clk_hw *prediv_hw, const struct clk_ops *prediv_ops,
			struct clk_hw *div_hw, const struct clk_ops *div_ops,
			struct clk_hw *gate_hw, const struct clk_ops *gate_ops,
			unsigned long flags)
{
	struct clk_hw *hw;

	hw = clk_hw_register_imx_composite(dev, name, parent_names, num_parents,
			mux_hw, mux_ops, prediv_hw, prediv_ops,
			div_hw, div_ops, gate_hw, gate_ops,
			flags);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}

void clk_unregister_imx_composite(struct clk *clk)
{
	struct imx_clk_composite *clk_imx8;
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	clk_imx8 = to_clk_imx_composite(hw);

	clk_unregister(clk);
	kfree(clk_imx8);
}

struct clk *imx_clk_composite_flags(const char *name, const char **parent_names,
			int num_parents, void __iomem *reg, unsigned long flags)
{
	struct clk_hw *mux_hw = NULL, *prediv_hw = NULL;
	struct clk_hw *div_hw = NULL, *gate_hw = NULL;
	struct clk_divider *prediv = NULL;
	struct clk_divider *div = NULL;
	struct clk_gate *gate = NULL;
	struct clk_mux *mux = NULL;
	struct clk *clk;

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);
	mux_hw = &mux->hw;
	mux->reg = reg;
	mux->shift = PCG_PCS_SHIFT;
	mux->mask = PCG_PCS_MASK;

	prediv = kzalloc(sizeof(*prediv), GFP_KERNEL);
	if (!prediv) {
		kfree(mux);
		return ERR_PTR(-ENOMEM);
	}
	prediv_hw = &prediv->hw;
	prediv->reg = reg;
	prediv->shift = PCG_PREDIV_SHIFT;
	prediv->width = PCG_PREDIV_WIDTH;
	prediv->lock = &imx_ccm_lock;
	prediv->flags = CLK_DIVIDER_ROUND_CLOSEST;

	div = kzalloc(sizeof(*prediv), GFP_KERNEL);
	if (!div) {
		kfree(mux);
		return ERR_PTR(-ENOMEM);
	}
	div_hw = &div->hw;
	div->reg = reg;
	div->shift = PCG_DIV_SHIFT;
	div->width = PCG_DIV_WIDTH;
	div->lock = &imx_ccm_lock;
	div->flags = CLK_DIVIDER_ROUND_CLOSEST;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate) {
		kfree(mux);
		kfree(prediv);
		kfree(div);
		return ERR_PTR(-ENOMEM);
	}
	gate_hw = &gate->hw;
	gate->reg = reg;
	gate->bit_idx = PCG_CGC_SHIFT;

	flags |= CLK_SET_RATE_NO_REPARENT | CLK_OPS_PARENT_ENABLE;

	clk = clk_register_imx_composite(NULL, name, parent_names, num_parents,
					mux_hw, &clk_mux_ops, prediv_hw,
					&clk_divider_ops, div_hw,
					&clk_divider_ops, gate_hw,
					&clk_gate_ops, flags);
	if (IS_ERR(clk)) {
		kfree(mux);
		kfree(prediv);
		kfree(div);
		kfree(gate);
	}

	return clk;
}
