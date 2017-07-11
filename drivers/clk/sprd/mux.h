/*
 * Spreadtrum multiplexer clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _SPRD_MUX_H_
#define _SPRD_MUX_H_

#include "common.h"

struct sprd_mux_internal {
	u8		shift;
	u8		width;
	const u8	*table;
};

struct sprd_mux {
	struct sprd_mux_internal mux;
	struct sprd_clk_common	common;
};

#define _SPRD_MUX_CLK(_shift, _width, _table)		\
	{						\
		.shift	= _shift,			\
		.width	= _width,			\
		.table	= _table,			\
	}

#define SPRD_MUX_CLK(_struct, _name, _parents, _table,			\
				     _reg, _shift, _width,		\
				     _flags)				\
	struct sprd_mux _struct = {					\
		.mux	= _SPRD_MUX_CLK(_shift, _width, _table),	\
		.common	= {						\
			.reg		= _reg,				\
			.lock		= &sprd_mux_lock,		\
			.hw.init	= CLK_HW_INIT_PARENTS(_name,	\
							      _parents, \
							      &sprd_mux_ops,\
							      _flags),	\
		}							\
	}

static inline struct sprd_mux *hw_to_sprd_mux(const struct clk_hw *hw)
{
	struct sprd_clk_common *common = hw_to_sprd_clk_common(hw);

	return container_of(common, struct sprd_mux, common);
}

extern const struct clk_ops sprd_mux_ops;
extern spinlock_t sprd_mux_lock;

u8 sprd_mux_helper_get_parent(const struct sprd_clk_common *common,
			      const struct sprd_mux_internal *mux);
int sprd_mux_helper_set_parent(const struct sprd_clk_common *common,
			       const struct sprd_mux_internal *mux,
			       u8 index);

#endif /* _SPRD_MUX_H_ */
