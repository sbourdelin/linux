/*
 * Spreadtrum gate clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>

#include "ccu_gate.h"

DEFINE_SPINLOCK(gate_lock);

static void ccu_gate_endisable(struct ccu_gate *cg, u32 en)
{
	struct ccu_common *common = &cg->common;
	unsigned long flags = 0;
	u32 reg;
	int set = cg->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;

	set ^= en;

	spin_lock_irqsave(common->lock, flags);

	reg = ccu_readl(common);

	if (set)
		reg |= cg->op_bit;
	else
		reg &= ~cg->op_bit;

	ccu_writel(reg, common);

	spin_unlock_irqrestore(common->lock, flags);
}

static void ccu_sc_gate_endisable(struct ccu_gate *cg, u32 en)
{
	struct ccu_common *common = &cg->common;
	unsigned long flags = 0;
	int set = cg->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	u32 offset;

	set ^= en;

	/*
	 * Each set/clear gate clock has three registers:
	 * common->reg			- base register
	 * common->reg + offset		- set register
	 * common->reg + 2 * offset	- clear register
	 */
	offset = set ? cg->sc_offset : cg->sc_offset * 2;

	spin_lock_irqsave(common->lock, flags);
	ccu_writel_offset(cg->op_bit, common, offset);
	spin_unlock_irqrestore(common->lock, flags);
}

static void ccu_gate_disable(struct clk_hw *hw)
{
	struct ccu_gate *cg = hw_to_ccu_gate(hw);

	if (cg->sc_offset)
		ccu_sc_gate_endisable(cg, 0);
	else
		ccu_gate_endisable(cg, 0);
}

static int ccu_gate_enable(struct clk_hw *hw)
{
	struct ccu_gate *cg = hw_to_ccu_gate(hw);

	if (cg->sc_offset)
		ccu_sc_gate_endisable(cg, 1);
	else
		ccu_gate_endisable(cg, 1);

	return 0;
}

static int ccu_gate_is_enabled(struct clk_hw *hw)
{
	struct ccu_gate *cg = hw_to_ccu_gate(hw);
	struct ccu_common *common = &cg->common;
	u32 reg;

	reg = ccu_readl(common);

	if (cg->flags & CLK_GATE_SET_TO_DISABLE)
		reg ^= cg->op_bit;

	reg &= cg->op_bit;

	return reg ? 1 : 0;
}

const struct clk_ops ccu_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,
};
