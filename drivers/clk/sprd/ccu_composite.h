/*
 * Spreadtrum composite clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_COMPOSITE_H_
#define _CCU_COMPOSITE_H_

#include "ccu_common.h"
#include "ccu_mux.h"
#include "ccu_div.h"

struct ccu_comp {
	struct ccu_mux_internal	mux;
	struct ccu_div_internal	div;
	struct ccu_common	common;
};

#define SPRD_CCU_COMP(_struct, _name, _parent, _reg, _table,		\
			_mshift, _mwidth, _dshift, _dwidth, _flags)	\
	struct ccu_comp _struct = {					\
		.mux	= _SPRD_CCU_MUX(_mshift, _mwidth, _table),	\
		.div	= _SPRD_CCU_DIV(_dshift, _dwidth),		\
		.common = {						\
			 .reg		= _reg,				\
			 .lock		= &comp_lock,			\
			 .hw.init	= CLK_HW_INIT_PARENTS(_name,	\
							      _parent,	\
							&ccu_comp_ops,	\
							      _flags),	\
			 }						\
	}

static inline struct ccu_comp *hw_to_ccu_comp(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_comp, common);
}

extern const struct clk_ops ccu_comp_ops;
extern spinlock_t comp_lock;

#endif /* _CCU_COMPOSITE_H_ */
