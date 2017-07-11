/*
 * Spreadtrum gate clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>

#include "gate.h"

DEFINE_SPINLOCK(sprd_gate_lock);
EXPORT_SYMBOL_GPL(sprd_gate_lock);

static void sprd_gate_endisable(const struct sprd_gate *sg, u32 en)
{
	const struct sprd_clk_common *common = &sg->common;
	unsigned long flags = 0;
	u32 reg;
	int set = sg->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;

	set ^= en;

	spin_lock_irqsave(common->lock, flags);

	reg = sprd_clk_readl(common);

	if (set)
		reg |= sg->op_bit;
	else
		reg &= ~sg->op_bit;

	sprd_clk_writel(reg, common);

	spin_unlock_irqrestore(common->lock, flags);
}

static void clk_sc_gate_endisable(const struct sprd_gate *sg, u32 en)
{
	const struct sprd_clk_common *common = &sg->common;
	unsigned long flags = 0;
	int set = sg->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	u32 offset;

	set ^= en;

	/*
	 * Each set/clear gate clock has three registers:
	 * common->reg			- base register
	 * common->reg + offset		- set register
	 * common->reg + 2 * offset	- clear register
	 */
	offset = set ? sg->sc_offset : sg->sc_offset * 2;

	spin_lock_irqsave(common->lock, flags);
	sprd_clk_writel_offset(sg->op_bit, common, offset);
	spin_unlock_irqrestore(common->lock, flags);
}

static void sprd_gate_disable(struct clk_hw *hw)
{
	struct sprd_gate *sg = hw_to_sprd_gate(hw);

	if (sg->sc_offset)
		clk_sc_gate_endisable(sg, 0);
	else
		sprd_gate_endisable(sg, 0);
}

static int sprd_gate_enable(struct clk_hw *hw)
{
	struct sprd_gate *sg = hw_to_sprd_gate(hw);

	if (sg->sc_offset)
		clk_sc_gate_endisable(sg, 1);
	else
		sprd_gate_endisable(sg, 1);

	return 0;
}

static int sprd_gate_is_enabled(struct clk_hw *hw)
{
	struct sprd_gate *sg = hw_to_sprd_gate(hw);
	struct sprd_clk_common *common = &sg->common;
	u32 reg;

	reg = sprd_clk_readl(common);

	if (sg->flags & CLK_GATE_SET_TO_DISABLE)
		reg ^= sg->op_bit;

	reg &= sg->op_bit;

	return reg ? 1 : 0;
}

const struct clk_ops sprd_gate_ops = {
	.disable	= sprd_gate_disable,
	.enable		= sprd_gate_enable,
	.is_enabled	= sprd_gate_is_enabled,
};
EXPORT_SYMBOL_GPL(sprd_gate_ops);
