/*
 * Spreadtrum divider clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_DIV_H_
#define _CCU_DIV_H_

#include "ccu_common.h"

/**
 * struct ccu_div_internal - Internal divider description
 * @shift: Bit offset of the divider in its register
 * @width: Width of the divider field in its register
 *
 * That structure represents a single divider, and is meant to be
 * embedded in other structures representing the various clock
 * classes.
 */
struct ccu_div_internal {
	u8	shift;
	u8	width;
};

#define _SPRD_CCU_DIV(_shift, _width)	\
	{				\
		.shift	= _shift,	\
		.width	= _width,	\
	}

struct ccu_div {
	struct ccu_div_internal	div;
	struct ccu_common	common;
};

#define SPRD_CCU_DIV(_struct, _name, _parent, _reg,			\
			_shift, _width, _flags)				\
	struct ccu_div _struct = {					\
		.div	= _SPRD_CCU_DIV(_shift, _width),		\
		.common	= {						\
			.reg		= _reg,				\
			.lock		= &div_lock,			\
			.hw.init	= CLK_HW_INIT(_name,		\
						      _parent,		\
						      &ccu_div_ops,	\
						      _flags),		\
		}							\
	}

static inline struct ccu_div *hw_to_ccu_div(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_div, common);
}

long ccu_div_helper_round_rate(struct ccu_common *common,
			       struct ccu_div_internal *div,
			       unsigned long rate,
			       unsigned long *parent_rate);

unsigned long ccu_div_helper_recalc_rate(struct ccu_common *common,
					 struct ccu_div_internal *div,
					 unsigned long parent_rate);

int ccu_div_helper_set_rate(struct ccu_common *common,
			    struct ccu_div_internal *div,
			    unsigned long rate,
			    unsigned long parent_rate);

extern const struct clk_ops ccu_div_ops;
extern spinlock_t div_lock;

#endif /* _CCU_DIV_H_ */
