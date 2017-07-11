/*
 * Spreadtrum clock infrastructure
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _SPRD_CLK_COMMON_H_
#define _SPRD_CLK_COMMON_H_

#include <linux/clk-provider.h>

#include "../clk_common.h"

struct device_node;

struct sprd_clk_common {
	void __iomem	*base;
	u32		reg;
	spinlock_t	*lock;
	struct clk_hw	hw;
};

struct clk_addr_map {
	phys_addr_t phy;
	void __iomem *virt;
};

static inline u32 sprd_clk_readl(const struct sprd_clk_common *common)
{
	return readl(common->base + common->reg);
}

static inline void sprd_clk_writel(u32 val,
				   const struct sprd_clk_common *common)
{
	writel(val, common->base + common->reg);
}

static inline struct sprd_clk_common *
	hw_to_sprd_clk_common(const struct clk_hw *hw)
{
	return container_of(hw, struct sprd_clk_common, hw);
}

struct sprd_clk_desc {
	struct sprd_clk_common		**clk_clks;
	unsigned long			num_clk_clks;
	struct clk_hw_onecell_data	*hw_clks;
};

int sprd_clk_probe(struct device_node *node, const struct clk_addr_map *maps,
		   unsigned int count, const struct sprd_clk_desc *desc);

#endif /* _SPRD_CLK_COMMON_H_ */
