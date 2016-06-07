/*
 * Copyright (c) 2016 Maxime Ripard. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CCU_FIXED_FACTOR_H_
#define _CCU_FIXED_FACTOR_H_

#include <linux/clk-provider.h>

#include "ccu_common.h"

struct ccu_fixed_factor {
	u16			div;
	u16			mult;

	struct ccu_common	common;
};

#define SUNXI_CCU_FIXED_FACTOR(_struct, _name, _parent,			\
			       _div, _mult, _flags)			\
	struct ccu_fixed_factor _struct = {				\
		.div	= _div,						\
		.mult	= _mult,					\
		.common	= {						\
			.hw.init	= SUNXI_HW_INIT(_name,		\
							_parent,	\
							&ccu_fixed_factor_ops, \
							_flags),	\
		},							\
	}

static inline struct ccu_fixed_factor *hw_to_ccu_fixed_factor(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_fixed_factor, common);
}

extern const struct clk_ops ccu_fixed_factor_ops;

#endif /* _CCU_FIXED_FACTOR_H_ */
