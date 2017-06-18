/*
 * Spreadtrum gate clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_GATE_H_
#define _CCU_GATE_H_

#include "ccu_common.h"

struct ccu_gate {
	u32			op_bit;
	u16			flags;
	u16			sc_offset;

	struct ccu_common	common;
};

#define SPRD_CCU_GATE(_struct, _name, _parent, _reg, _sc_offset,	\
		      _op_bit, _flags, _gate_flags)			\
	struct ccu_gate _struct = {					\
		.op_bit		= _op_bit,				\
		.sc_offset	= _sc_offset,				\
		.flags		= _gate_flags,				\
		.common	= {						\
			.reg		= _reg,				\
			.lock		= &gate_lock,			\
			.hw.init	= CLK_HW_INIT(_name,		\
						      _parent,		\
						      &ccu_gate_ops,	\
						      _flags),		\
		}							\
	}

#define SPRD_CCU_GATE_NO_PARENT(_struct, _name, _reg, _sc_offset,	\
				_op_bit, _flags, _gate_flags)		\
	struct ccu_gate _struct = {					\
		.op_bit		= _op_bit,				\
		.sc_offset	= _sc_offset,				\
		.flags		= _gate_flags,				\
		.common	= {						\
			.reg	= _reg,					\
			.lock	= &gate_lock,				\
			.hw.init = CLK_HW_INIT_NO_PARENT(_name,		\
							 &ccu_gate_ops,	\
							 _flags),	\
		}							\
	}

static inline struct ccu_gate *hw_to_ccu_gate(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_gate, common);
}

static inline void ccu_writel_offset(u32 val,
	struct ccu_common *common, u32 offset)
{
	writel(val, common->base + common->reg + offset);
}

void ccu_gate_helper_disable(struct ccu_common *common, u32 gate);
int ccu_gate_helper_enable(struct ccu_common *common, u32 gate);
int ccu_gate_helper_is_enabled(struct ccu_common *common, u32 gate);

extern const struct clk_ops ccu_gate_ops;
extern spinlock_t gate_lock;

#endif /* _CCU_GATE_H_ */
