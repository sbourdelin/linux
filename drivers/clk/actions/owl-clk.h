/*
 * Copyright (c) 2014 Actions Semi Inc.
 * Author: David Liu <liuwei@actions-semi.com>
 *
 * Copyright (c) 2017 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 *
 * based on
 *
 * samsung/clk.h
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2013 Linaro Ltd.
 * Author: Thomas Abraham <thomas.ab@owl.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __OWL_CLK_H
#define __OWL_CLK_H

#include <linux/clk-provider.h>

struct owl_clk_provider {
	void __iomem		*reg_base;
	struct clk_hw_onecell_data clk_data;
	spinlock_t		lock;
};

struct owl_fixed_factor_clock {
	unsigned int		id;
	char			*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned int		mult;
	unsigned int		div;
};

/* last entry should have rate = 0 */
struct clk_pll_table {
	unsigned int		val;
	unsigned long		rate;
};

struct owl_pll_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	unsigned long		bfreq;
	u8			enable_bit;
	u8			shift;
	u8			width;
	u8			min_mul;
	u8			max_mul;
	u8			pll_flags;
	const struct clk_pll_table *table;
};

#define CLK_OWL_PLL_FIXED_FREQ	BIT(0)

struct owl_divider_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			div_flags;
	struct clk_div_table	*table;
	const char		*alias;
};

struct clk_factor_table {
	unsigned int		val;
	unsigned int		mul;
	unsigned int		div;
};

struct owl_factor_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			div_flags;
	struct clk_factor_table	*table;
	const char		*alias;
};

struct owl_factor {
	struct clk_hw		hw;
	void __iomem		*reg;
	u8			shift;
	u8			width;
	u8			flags;
	const struct clk_factor_table *table;
	spinlock_t		*lock;
};

extern const struct clk_ops owl_factor_ops;

struct owl_mux_clock {
	unsigned int		id;
	const char		*name;
	const char		**parent_names;
	u8			num_parents;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			mux_flags;
	const char		*alias;
};

struct owl_gate_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			bit_idx;
	u8			gate_flags;
	const char		*alias;
};

union rate_clock {
	struct owl_fixed_factor_clock	fixed_factor;
	struct owl_divider_clock	div;
	struct owl_factor_clock		factor;
};

struct owl_composite_clock {
	unsigned int		id;
	const char		*name;
	unsigned int		type;
	unsigned long		flags;

	struct owl_mux_clock	mux;
	struct owl_gate_clock	gate;
	union rate_clock	rate;
};

#define OWL_COMPOSITE_TYPE_DIVIDER         1
#define OWL_COMPOSITE_TYPE_FACTOR          2
#define OWL_COMPOSITE_TYPE_FIXED_FACTOR    3
#define OWL_COMPOSITE_TYPE_PASS            10

#define COMP_FIXED_FACTOR_CLK(_id, _name, _flags, _mux, _gate, _fixed_factor) \
	{								\
		.id		= _id,					\
		.name		= _name,				\
		.type		= OWL_COMPOSITE_TYPE_FIXED_FACTOR,	\
		.flags		= _flags,				\
		.mux		= _mux,					\
		.gate		= _gate,				\
		.rate.fixed_factor = _fixed_factor,			\
	}

#define COMP_DIV_CLK(_id, _name, _flags, _mux, _gate, _div)		\
	{								\
		.id		= _id,					\
		.name		= _name,				\
		.type		= OWL_COMPOSITE_TYPE_DIVIDER,		\
		.flags		= _flags,				\
		.mux		= _mux,					\
		.gate		= _gate,				\
		.rate.div	= _div,					\
	}

#define COMP_FACTOR_CLK(_id, _name, _flags, _mux, _gate, _factor)	\
	{								\
		.id		= _id,					\
		.name		= _name,				\
		.type		= OWL_COMPOSITE_TYPE_FACTOR,		\
		.flags		= _flags,				\
		.mux		= _mux,					\
		.gate		= _gate,				\
		.rate.factor	= _factor,				\
	}

#define COMP_PASS_CLK(_id, _name, _flags, _mux, _gate)			\
	{								\
		.id		= _id,					\
		.name		= _name,				\
		.type		= OWL_COMPOSITE_TYPE_PASS,		\
		.flags		= _flags,				\
		.mux		= _mux,					\
		.gate		= _gate,				\
	}

#define C_MUX(p, o, s, w, mf)						\
	{								\
		.id		= -1,					\
		.parent_names	= p,					\
		.num_parents	= ARRAY_SIZE(p),			\
		.offset		= o,					\
		.shift		= s,					\
		.width		= w,					\
		.mux_flags	= mf,					\
	}

/* fixed mux, only one parent */
#define C_MUX_F(p, mf)							\
	{								\
		.id		= -1,					\
		.parent_names	= p,					\
		.num_parents	= 1,					\
		.mux_flags = mf,					\
	}

#define C_GATE(o, b, gf)						\
	{								\
		.id		= -1,					\
		.offset		= o,					\
		.bit_idx	= b,					\
		.gate_flags	= gf,					\
	}

#define C_NULL								\
	{								\
		.id		= 0,					\
	}

#define C_FIXED_FACTOR(m, d)						\
	{								\
		.id		= -1,					\
		.mult		= m,					\
		.div		= d,					\
	}

#define C_DIVIDER(o, s, w, t, df)					\
	{								\
		.id		= -1,					\
		.offset		= o,					\
		.shift		= s,					\
		.width		= w,					\
		.table		= t,					\
		.div_flags	= df,					\
	}

#define C_FACTOR(o, s, w, t, df)					\
	{								\
		.id		= -1,					\
		.offset		= o,					\
		.shift		= s,					\
		.width		= w,					\
		.table		= t,					\
		.div_flags	= df,					\
	}

extern void owl_clk_register_pll(struct owl_clk_provider *ctx,
		struct owl_pll_clock *clks, int nums);

extern void owl_clk_register_fixed_factor(
		struct owl_clk_provider *ctx,
		struct owl_fixed_factor_clock *clks,
		int nums);

extern void owl_clk_register_divider(struct owl_clk_provider *ctx,
		struct owl_divider_clock *clks, int nums);

extern void owl_clk_register_factor(struct owl_clk_provider *ctx,
		struct owl_factor_clock *clks, int nums);

extern void owl_clk_register_mux(struct owl_clk_provider *ctx,
		struct owl_mux_clock *clks, int nums);

extern void owl_clk_register_gate(struct owl_clk_provider *ctx,
		struct owl_gate_clock *clks, int nums);

extern void owl_clk_register_composite(struct owl_clk_provider *ctx,
		struct owl_composite_clock *clks, int nums);

extern struct clk_hw *owl_pll_clk_register(const char *name,
		const char *parent_name, unsigned long flags,
		void __iomem *reg, unsigned long bfreq, u8 enable_bit,
		u8 shift, u8 width, u8 min_mul, u8 max_mul, u8 pll_flags,
		const struct clk_pll_table *table, spinlock_t *lock);

extern struct clk_hw *owl_factor_clk_register(struct device *dev,
		const char *name, const char *parent_name,
		unsigned long flags, void __iomem *reg, u8 shift,
		u8 width, u8 clk_factor_flags,
		const struct clk_factor_table *table, spinlock_t *lock);

#endif /* __OWL_CLK_H */
