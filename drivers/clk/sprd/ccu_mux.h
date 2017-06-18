/*
 * Spreadtrum multiplexer clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_MUX_H_
#define _CCU_MUX_H_

#include "ccu_common.h"

struct ccu_mux_internal {
	u8		shift;
	u8		width;
	const u8	*table;
};

struct ccu_mux {
	struct ccu_mux_internal mux;
	struct ccu_common	common;
};

#define _SPRD_CCU_MUX(_shift, _width, _table)		\
	{						\
		.shift	= _shift,			\
		.width	= _width,			\
		.table	= _table,			\
	}

#define SPRD_CCU_MUX(_struct, _name, _parents, _table,			\
				     _reg, _shift, _width,		\
				     _flags)				\
	struct ccu_mux _struct = {					\
		.mux	= _SPRD_CCU_MUX(_shift, _width, _table),	\
		.common	= {						\
			.reg		= _reg,				\
			.lock		= &mux_lock,			\
			.hw.init	= CLK_HW_INIT_PARENTS(_name,	\
							      _parents, \
							      &ccu_mux_ops,\
							      _flags),	\
		}							\
	}

static inline struct ccu_mux *hw_to_ccu_mux(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_mux, common);
}

extern const struct clk_ops ccu_mux_ops;
extern spinlock_t mux_lock;

u8 ccu_mux_helper_get_parent(struct ccu_common *common,
			     struct ccu_mux_internal *mux);
int ccu_mux_helper_set_parent(struct ccu_common *common,
			      struct ccu_mux_internal *mux,
			      u8 index);

#endif /* _CCU_MUX_H_ */
