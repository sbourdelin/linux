/*
 * Spreadtrum multiplexer clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>

#include "mux.h"

DEFINE_SPINLOCK(sprd_mux_lock);
EXPORT_SYMBOL_GPL(sprd_mux_lock);

u8 sprd_mux_helper_get_parent(const struct sprd_clk_common *common,
			      const struct sprd_mux_internal *mux)
{
	u32 reg;
	u8 parent;
	int num_parents;
	int i;

	reg = sprd_clk_readl(common);
	parent = reg >> mux->shift;
	parent &= (1 << mux->width) - 1;

	if (mux->table) {
		num_parents = clk_hw_get_num_parents(&common->hw);

		for (i = 0; i < num_parents; i++)
			if (parent == mux->table[i] ||
			    (i < (num_parents - 1) && parent > mux->table[i] &&
			     parent < mux->table[i + 1]))
				return i;
		if (i == num_parents)
			return i - 1;
	}

	return parent;
}
EXPORT_SYMBOL_GPL(sprd_mux_helper_get_parent);

static u8 sprd_mux_get_parent(struct clk_hw *hw)
{
	struct sprd_mux *cm = hw_to_sprd_mux(hw);

	return sprd_mux_helper_get_parent(&cm->common, &cm->mux);
}

int sprd_mux_helper_set_parent(const struct sprd_clk_common *common,
			       const struct sprd_mux_internal *mux,
			       u8 index)
{
	unsigned long flags = 0;
	u32 reg;

	if (mux->table)
		index = mux->table[index];

	spin_lock_irqsave(common->lock, flags);

	reg = sprd_clk_readl(common);
	reg &= ~GENMASK(mux->width + mux->shift - 1, mux->shift);
	sprd_clk_writel(reg | (index << mux->shift), common);

	spin_unlock_irqrestore(common->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(sprd_mux_helper_set_parent);

static int sprd_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct sprd_mux *cm = hw_to_sprd_mux(hw);

	return sprd_mux_helper_set_parent(&cm->common, &cm->mux, index);
}

const struct clk_ops sprd_mux_ops = {
	.get_parent = sprd_mux_get_parent,
	.set_parent = sprd_mux_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};
EXPORT_SYMBOL_GPL(sprd_mux_ops);
