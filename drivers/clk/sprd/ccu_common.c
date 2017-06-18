/*
 * Spreadtrum clock infrastructure
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "ccu_common.h"

static inline void __iomem *ccu_find_base(struct ccu_addr_map *maps,
					  unsigned int num, unsigned int reg)
{
	int i;

	for (i = 0; i < num; i++)
		if ((reg & 0xffff0000) == maps[i].phy)
			return maps[i].virt;

	return 0;
}

int sprd_ccu_probe(struct device_node *node, struct ccu_addr_map *maps,
		   unsigned int count, const struct sprd_ccu_desc *desc)
{
	int i, ret = 0;
	struct ccu_common *cclk;
	struct clk_hw *hw;

	for (i = 0; i < desc->num_ccu_clks; i++) {
		cclk = desc->ccu_clks[i];
		if (!cclk)
			continue;

		cclk->base = ccu_find_base(maps, count, cclk->reg);
		if (!cclk->base) {
			pr_err("%s: No mapped address found for clock(0x%x)\n",
				__func__, cclk->reg);
			return -EINVAL;
		}
		cclk->reg = cclk->reg & 0xffff;
	}

	for (i = 0; i < desc->hw_clks->num; i++) {

		hw = desc->hw_clks->hws[i];

		if (!hw)
			continue;

		ret = clk_hw_register(NULL, hw);
		if (ret) {
			pr_err("Couldn't register clock %d - %s\n",
			       i, hw->init->name);
			goto err_clk_unreg;
		}
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				     desc->hw_clks);
	if (ret) {
		pr_err("Failed to add clock provider.\n");
		goto err_clk_unreg;
	}

	return 0;

err_clk_unreg:
	while (--i >= 0) {
		hw = desc->hw_clks->hws[i];
		if (!hw)
			continue;

		clk_hw_unregister(hw);
	}

	return ret;
}
