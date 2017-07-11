/*
 * Spreadtrum gate clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _SPRD_GATE_H_
#define _SPRD_GATE_H_

#include "common.h"

struct sprd_gate {
	u32			op_bit;
	u16			flags;
	u16			sc_offset;

	struct sprd_clk_common	common;
};

#define SPRD_GATE_CLK(_struct, _name, _parent, _reg, _sc_offset,	\
		      _op_bit, _flags, _gate_flags)			\
	struct sprd_gate _struct = {					\
		.op_bit		= _op_bit,				\
		.sc_offset	= _sc_offset,				\
		.flags		= _gate_flags,				\
		.common	= {						\
			.reg		= _reg,				\
			.lock		= &sprd_gate_lock,		\
			.hw.init	= CLK_HW_INIT(_name,		\
						      _parent,		\
						      &sprd_gate_ops,	\
						      _flags),		\
		}							\
	}

static inline struct sprd_gate *hw_to_sprd_gate(const struct clk_hw *hw)
{
	struct sprd_clk_common *common = hw_to_sprd_clk_common(hw);

	return container_of(common, struct sprd_gate, common);
}

static inline void sprd_clk_writel_offset(u32 val,
	const struct sprd_clk_common *common, u32 offset)
{
	writel(val, common->base + common->reg + offset);
}

void sprd_gate_helper_disable(struct sprd_clk_common *common, u32 gate);
int sprd_gate_helper_enable(struct sprd_clk_common *common, u32 gate);
int sprd_gate_helper_is_enabled(struct sprd_clk_common *common, u32 gate);

extern const struct clk_ops sprd_gate_ops;
extern spinlock_t sprd_gate_lock;

#endif /* _SPRD_GATE_H_ */
