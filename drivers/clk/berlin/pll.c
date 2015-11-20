/*
 * Copyright (c) 2015 Marvell Technology Group Ltd.
 *
 * Author: Jisheng Zhang <jszhang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define PLL_CTRL0	0x0
#define PLL_CTRL1	0x4
#define PLL_CTRL2	0x8
#define PLL_CTRL3	0xC
#define PLL_CTRL4	0x10
#define PLL_STATUS	0x14

#define PLL_SOURCE_MAX	2

struct berlin_pll {
	struct clk_hw	hw;
	void __iomem	*ctrl;
	void __iomem	*bypass;
	u8		bypass_shift;
};

#define to_berlin_pll(hw)       container_of(hw, struct berlin_pll, hw)

static unsigned long berlin_pll_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	u32 val, fbdiv, rfdiv, vcodivsel, bypass;
	struct berlin_pll *pll = to_berlin_pll(hw);

	bypass = readl_relaxed(pll->bypass);
	if (bypass & (1 << pll->bypass_shift))
		return parent_rate;

	val = readl_relaxed(pll->ctrl + PLL_CTRL0);
	fbdiv = (val >> 12) & 0x1FF;
	rfdiv = (val >> 3) & 0x1FF;
	val = readl_relaxed(pll->ctrl + PLL_CTRL1);
	vcodivsel = (val >> 9) & 0x7;
	return parent_rate * fbdiv * 4 / rfdiv /
		(1 << vcodivsel);
}

static u8 berlin_pll_get_parent(struct clk_hw *hw)
{
	struct berlin_pll *pll = to_berlin_pll(hw);
	u32 bypass = readl_relaxed(pll->bypass);

	return !!(bypass & (1 << pll->bypass_shift));
}

static const struct clk_ops berlin_pll_ops = {
	.recalc_rate	= berlin_pll_recalc_rate,
	.get_parent	= berlin_pll_get_parent,
};

static void __init berlin_pll_setup(struct device_node *np)
{
	struct clk_init_data init;
	struct berlin_pll *pll;
	const char *parent_names[PLL_SOURCE_MAX];
	struct clk *clk;
	int ret, num_parents;
	u8 bypass_shift;

	num_parents = of_clk_get_parent_count(np);
	if (num_parents <= 0 || num_parents > PLL_SOURCE_MAX)
		return;

	ret = of_property_read_u8(np, "bypass-shift", &bypass_shift);
	if (ret)
		return;

	of_clk_parent_fill(np, parent_names, num_parents);

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return;

	pll->ctrl = of_iomap(np, 0);
	if (WARN_ON(!pll->ctrl))
		goto err_iomap_ctrl;

	pll->bypass = of_iomap(np, 1);
	if (WARN_ON(!pll->bypass))
		goto err_iomap_bypass;

	init.name = np->name;
	init.flags = 0;
	init.ops = &berlin_pll_ops;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	pll->hw.init = &init;
	pll->bypass_shift = bypass_shift;

	clk = clk_register(NULL, &pll->hw);
	if (WARN_ON(IS_ERR(clk)))
		goto err_clk_register;

	ret = of_clk_add_provider(np, of_clk_src_simple_get, clk);
	if (WARN_ON(ret))
		goto err_clk_add;
	return;

err_clk_add:
	clk_unregister(clk);
err_clk_register:
	iounmap(pll->bypass);
err_iomap_bypass:
	iounmap(pll->ctrl);
err_iomap_ctrl:
	kfree(pll);
}
CLK_OF_DECLARE(berlin_pll, "marvell,berlin-pll", berlin_pll_setup);
