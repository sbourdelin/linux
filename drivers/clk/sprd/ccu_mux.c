/*
 * Spreadtrum multiplexer clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>

#include "ccu_mux.h"

DEFINE_SPINLOCK(mux_lock);

u8 ccu_mux_helper_get_parent(struct ccu_common *common,
			     struct ccu_mux_internal *mux)
{
	u32 reg;
	u8 parent;
	int num_parents;
	int i;

	reg = ccu_readl(common);
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

static u8 ccu_mux_get_parent(struct clk_hw *hw)
{
	struct ccu_mux *cm = hw_to_ccu_mux(hw);

	return ccu_mux_helper_get_parent(&cm->common, &cm->mux);
}

int ccu_mux_helper_set_parent(struct ccu_common *common,
			      struct ccu_mux_internal *mux,
			      u8 index)
{
	unsigned long flags = 0;
	u32 reg;

	if (mux->table)
		index = mux->table[index];

	spin_lock_irqsave(common->lock, flags);

	reg = ccu_readl(common);
	reg &= ~GENMASK(mux->width + mux->shift - 1, mux->shift);
	ccu_writel(reg | (index << mux->shift), common);

	spin_unlock_irqrestore(common->lock, flags);

	return 0;
}

static int ccu_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct ccu_mux *cm = hw_to_ccu_mux(hw);

	return ccu_mux_helper_set_parent(&cm->common, &cm->mux, index);
}

const struct clk_ops ccu_mux_ops = {
	.get_parent = ccu_mux_get_parent,
	.set_parent = ccu_mux_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};
