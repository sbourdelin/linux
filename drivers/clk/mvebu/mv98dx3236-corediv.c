/*
 * MV98DX3236 Core divider clock
 *
 * Copyright (C) 2015 Allied Telesis Labs
 *
 * Based on armada-xp-corediv.c
 * Copyright (C) 2015 Marvell
 *
 * John Thompson <john.thompson@alliedtelesis.co.nz>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "common.h"

#define CORE_CLK_DIV_RATIO_MASK		0xff

#define CLK_DIV_RATIO_NAND_MASK 0x0f
#define CLK_DIV_RATIO_NAND_OFFSET 6
#define CLK_DIV_RATIO_NAND_FORCE_RELOAD_BIT 26

#define RATIO_RELOAD_BIT BIT(10)
#define RATIO_REG_OFFSET 0x08

/*
 * This structure represents one core divider clock for the clock
 * framework, and is dynamically allocated for each core divider clock
 * existing in the current SoC.
 */
struct clk_corediv {
	struct clk_hw hw;
	void __iomem *reg;
	spinlock_t lock;
};

static struct clk_onecell_data clk_data;


#define to_corediv_clk(p) container_of(p, struct clk_corediv, hw)

static int mv98dx3236_corediv_is_enabled(struct clk_hw *hwclk)
{
	/* Core divider is always active */
	return 1;
}

static int mv98dx3236_corediv_enable(struct clk_hw *hwclk)
{
	/* always succeeds */
	return 0;
}

static void mv98dx3236_corediv_disable(struct clk_hw *hwclk)
{
	/* can't be disabled so is left alone */
}

static unsigned long mv98dx3236_corediv_recalc_rate(struct clk_hw *hwclk,
					 unsigned long parent_rate)
{
	struct clk_corediv *corediv = to_corediv_clk(hwclk);
	u32 reg, div;

	reg = readl(corediv->reg + RATIO_REG_OFFSET);
	div = (reg >> CLK_DIV_RATIO_NAND_OFFSET) & CLK_DIV_RATIO_NAND_MASK;
	return parent_rate / div;
}

static long mv98dx3236_corediv_round_rate(struct clk_hw *hwclk,
			       unsigned long rate, unsigned long *parent_rate)
{
	/* Valid ratio are 1:4, 1:5, 1:6 and 1:8 */
	u32 div;

	div = *parent_rate / rate;
	if (div < 4)
		div = 4;
	else if (div > 6)
		div = 8;

	return *parent_rate / div;
}

static int mv98dx3236_corediv_set_rate(struct clk_hw *hwclk, unsigned long rate,
			    unsigned long parent_rate)
{
	struct clk_corediv *corediv = to_corediv_clk(hwclk);
	unsigned long flags = 0;
	u32 reg, div;

	div = parent_rate / rate;

	spin_lock_irqsave(&corediv->lock, flags);

	/* Write new divider to the divider ratio register */
	reg = readl(corediv->reg + RATIO_REG_OFFSET);
	reg &= ~(CLK_DIV_RATIO_NAND_MASK << CLK_DIV_RATIO_NAND_OFFSET);
	reg |= (div & CLK_DIV_RATIO_NAND_MASK) << CLK_DIV_RATIO_NAND_OFFSET;
	writel(reg, corediv->reg + RATIO_REG_OFFSET);

	/* Set reload-force for this clock */
	reg = readl(corediv->reg) | BIT(CLK_DIV_RATIO_NAND_FORCE_RELOAD_BIT);
	writel(reg, corediv->reg);

	/* Now trigger the clock update */
	reg = readl(corediv->reg + RATIO_REG_OFFSET) | RATIO_RELOAD_BIT;
	writel(reg, corediv->reg + RATIO_REG_OFFSET);

	/*
	 * Wait for clocks to settle down, and then clear all the
	 * ratios request and the reload request.
	 */
	udelay(1000);
	reg &= ~(CORE_CLK_DIV_RATIO_MASK | RATIO_RELOAD_BIT);
	writel(reg, corediv->reg + RATIO_REG_OFFSET);
	udelay(1000);

	spin_unlock_irqrestore(&corediv->lock, flags);

	return 0;
}

static const struct clk_ops ops = {
	.enable = mv98dx3236_corediv_enable,
	.disable = mv98dx3236_corediv_disable,
	.is_enabled = mv98dx3236_corediv_is_enabled,
	.recalc_rate = mv98dx3236_corediv_recalc_rate,
	.round_rate = mv98dx3236_corediv_round_rate,
	.set_rate = mv98dx3236_corediv_set_rate,
};

static void __init mv98dx3236_corediv_clk_init(struct device_node *node)
{
	struct clk_init_data init;
	struct clk_corediv *corediv;
	struct clk **clks;
	void __iomem *base;
	const __be32 *off;
	const char *parent_name;
	const char *clk_name;
	int len;
	struct device_node *dfx_node;

	dfx_node = of_parse_phandle(node, "base", 0);
	if (WARN_ON(!dfx_node))
		return;

	off = of_get_property(node, "reg", &len);
	if (WARN_ON(!off))
		return;

	base = of_iomap(dfx_node, 0);
	if (WARN_ON(!base))
		return;

	of_node_put(dfx_node);

	parent_name = of_clk_get_parent_name(node, 0);

	clk_data.clk_num = 1;

	/* clks holds the clock array */
	clks = kcalloc(clk_data.clk_num, sizeof(struct clk *),
				GFP_KERNEL);
	if (WARN_ON(!clks))
		goto err_unmap;
	/* corediv holds the clock specific array */
	corediv = kcalloc(clk_data.clk_num, sizeof(struct clk_corediv),
				GFP_KERNEL);
	if (WARN_ON(!corediv))
		goto err_free_clks;

	spin_lock_init(&corediv->lock);

	of_property_read_string_index(node, "clock-output-names",
					  0, &clk_name);

	init.num_parents = 1;
	init.parent_names = &parent_name;
	init.name = clk_name;
	init.ops = &ops;
	init.flags = 0;

	corediv[0].reg = (void *)((int)base + be32_to_cpu(*off));
	corediv[0].hw.init = &init;

	clks[0] = clk_register(NULL, &corediv[0].hw);
	WARN_ON(IS_ERR(clks[0]));

	clk_data.clks = clks;
	of_clk_add_provider(node, of_clk_src_onecell_get, &clk_data);
	return;

err_free_clks:
	kfree(clks);
err_unmap:
	iounmap(base);
}

CLK_OF_DECLARE(mv98dx3236_corediv_clk, "marvell,mv98dx3236-corediv-clock",
	       mv98dx3236_corediv_clk_init);
