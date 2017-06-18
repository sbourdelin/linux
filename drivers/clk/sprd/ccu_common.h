/*
 * Spreadtrum clock infrastructure
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _CCU_COMMON_H_
#define _CCU_COMMON_H_

#include <linux/clk-provider.h>

struct device_node;

#define CLK_HW_INIT_NO_PARENT(_name, _ops, _flags)	\
	(&(struct clk_init_data) {			\
		.flags		= _flags,		\
		.name		= _name,		\
		.parent_names	= NULL,			\
		.num_parents	= 0,			\
		.ops		= _ops,			\
	})

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

struct ccu_common {
	void __iomem	*base;
	u32		reg;
	spinlock_t	*lock;
	struct clk_hw	hw;
};

struct ccu_addr_map {
	phys_addr_t phy;
	void __iomem *virt;
};

static inline u32 ccu_readl(struct ccu_common *common)
{
	return readl(common->base + common->reg);
}

static inline void ccu_writel(u32 val, struct ccu_common *common)
{
	writel(val, common->base + common->reg);
}

static inline struct ccu_common *hw_to_ccu_common(struct clk_hw *hw)
{
	return container_of(hw, struct ccu_common, hw);
}

struct sprd_ccu_desc {
	struct ccu_common		**ccu_clks;
	unsigned long			num_ccu_clks;
	struct clk_hw_onecell_data	*hw_clks;
};

int sprd_ccu_probe(struct device_node *node, struct ccu_addr_map *maps,
		   unsigned int count, const struct sprd_ccu_desc *desc);

#endif /* _CCU_COMMON_H_ */
