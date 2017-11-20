/*
 * drivers/clk/clk_common.h
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CLK_COMMON_H_
#define _CLK_COMMON_H_

#include <linux/clk-provider.h>

#define CLK_HW_INIT(_name, _parent, _ops, _flags)		\
	(&(struct clk_init_data) {				\
		.flags		= _flags,			\
		.name		= _name,			\
		.parent_names	= (const char *[]) { _parent },	\
		.num_parents	= 1,				\
		.ops		= _ops,				\
	})

#define CLK_HW_INIT_PARENTS(_name, _parents, _ops, _flags)	\
	(&(struct clk_init_data) {				\
		.flags		= _flags,			\
		.name		= _name,			\
		.parent_names	= _parents,			\
		.num_parents	= ARRAY_SIZE(_parents),		\
		.ops		= _ops,				\
	})

#define CLK_HW_INIT_NO_PARENT(_name, _ops, _flags)     \
	(&(struct clk_init_data) {                      \
		.flags          = _flags,               \
		.name           = _name,                \
		.parent_names   = NULL,                 \
		.num_parents    = 0,                    \
		.ops            = _ops,                 \
	})

#define CLK_FIXED_FACTOR(_struct, _name, _parent,			\
			_div, _mult, _flags)				\
	struct clk_fixed_factor _struct = {				\
		.div		= _div,					\
		.mult		= _mult,				\
		.hw.init	= CLK_HW_INIT(_name,			\
					      _parent,			\
					      &clk_fixed_factor_ops,	\
					      _flags),			\
	}

#define CLK_FIXED_RATE(_struct, _name, _flags,				\
		       _fixed_rate, _fixed_accuracy)			\
	struct clk_fixed_rate _struct = {				\
		.fixed_rate	= _fixed_rate,				\
		.fixed_accuracy	= _fixed_accuracy,			\
		.hw.init	= CLK_HW_INIT_NO_PARENT(_name,		\
					  &clk_fixed_rate_ops,		\
							_flags),	\
	}

#endif /* _CLK_COMMON_H_ */
