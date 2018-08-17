// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 NXP.
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/slab.h>

#include "clk.h"

#define PLL_CFG0		0x0
#define PLL_CFG1		0x4

#define PLL_LOCK_STATUS		BIT(31)
#define PLL_PD			19
#define PLL_PD_MASK		BIT(PLL_PD)
#define PLL_BYPASS		14
#define PLL_BYPASS_MASK		BIT(PLL_BYPASS)
#define PLL_NEWDIV_VAL		BIT(12)
#define PLL_NEWDIV_ACK		BIT(11)
#define PLL_FRAC_DIV_MASK	0xffffff
#define PLL_INT_DIV_MASK	0x7f
#define PLL_OUTPUT_DIV_MASK	0x1f
#define PLL_FRAC_DENOM		0x1000000

struct clk_frac_pll {
	struct clk_hw	hw;
	void __iomem	*base;
};

#define to_clk_frac_pll(_hw) container_of(_hw, struct clk_frac_pll, hw)

static int clk_wait_lock(struct clk_frac_pll *pll)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(10);
	u32 val;

	/* Wait for PLL to lock */
	do {
		if (readl_relaxed(pll->base) & PLL_LOCK_STATUS)
			break;
		if (time_after(jiffies, timeout))
			break;
	} while (1);

	return readl_poll_timeout(pll->base, val,
					val & PLL_LOCK_STATUS, 0, 1000);
}

static int clk_wait_ack(struct clk_frac_pll *pll)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(50);
	u32 val;

	/* return directly if the pll is in powerdown or in bypass */
	if (readl_relaxed(pll->base) & (PLL_PD_MASK | PLL_BYPASS_MASK))
		return 0;

	/* Wait for the pll's divfi and divff to be reloaded */
	do {
		if (readl_relaxed(pll->base) & PLL_NEWDIV_ACK)
			break;
		if (time_after(jiffies, timeout))
			break;
	} while (1);

	return readl_poll_timeout(pll->base, val,
					val & PLL_NEWDIV_ACK, 0, 1000);
}

static int clk_pll_prepare(struct clk_hw *hw)
{
	struct clk_frac_pll *pll = to_clk_frac_pll(hw);
	u32 val;

	val = readl_relaxed(pll->base + PLL_CFG0);
	val &= ~PLL_PD_MASK;
	writel_relaxed(val, pll->base + PLL_CFG0);

	return clk_wait_lock(pll);
}

static void clk_pll_unprepare(struct clk_hw *hw)
{
	struct clk_frac_pll *pll = to_clk_frac_pll(hw);
	u32 val;

	val = readl_relaxed(pll->base + PLL_CFG0);
	val |= PLL_PD_MASK;
	writel_relaxed(val, pll->base + PLL_CFG0);
}

static int clk_pll_is_prepared(struct clk_hw *hw)
{
	struct clk_frac_pll *pll = to_clk_frac_pll(hw);
	u32 val;

	val = readl_relaxed(pll->base + PLL_CFG0);
	return (val & PLL_PD_MASK) ? 0 : 1;
}

static unsigned long clk_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct clk_frac_pll *pll = to_clk_frac_pll(hw);
	u32 val, divff, divfi, divq;
	u64 temp64;

	val = readl_relaxed(pll->base + PLL_CFG0);
	divq = ((val & PLL_OUTPUT_DIV_MASK) + 1) * 2;
	val = readl_relaxed(pll->base + PLL_CFG1);
	divff = (val >> 7) & PLL_FRAC_DIV_MASK;
	divfi = (val & PLL_INT_DIV_MASK);

	temp64 = (u64)parent_rate * 8;
	temp64 *= divff;
	do_div(temp64, PLL_FRAC_DENOM);
	temp64 /= divq;

	return parent_rate * 8 * (divfi + 1) / divq + (unsigned long)temp64;
}

static long clk_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *prate)
{
	unsigned long parent_rate = *prate;
	u32 divff, divfi;
	u64 temp64;

	parent_rate *= 8;
	rate *= 2;
	divfi = rate / parent_rate;
	temp64 = (u64)(rate - divfi * parent_rate);
	temp64 *= PLL_FRAC_DENOM;
	do_div(temp64, parent_rate);
	divff = temp64;

	temp64 = (u64)parent_rate;
	temp64 *= divff;
	do_div(temp64, PLL_FRAC_DENOM);

	return (parent_rate * divfi + (unsigned long)temp64) / 2;
}

/*
 * To simplify the clock calculation, we can keep the 'PLL_OUTPUT_VAL' at zero
 * (means the PLL output will be divided by 2). So the PLL output can use
 * the below formula:
 * pllout = parent_rate * 8 / 2 * DIVF_VAL;
 * where DIVF_VAL = 1 + DIVFI + DIVFF / 2^24.
 */
static int clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct clk_frac_pll *pll = to_clk_frac_pll(hw);
	u32 val, divfi, divff;
	u64 temp64;
	int ret;

	parent_rate *= 8;
	rate *= 2;
	divfi = rate / parent_rate;
	temp64 = (u64) (rate - divfi * parent_rate);
	temp64 *= PLL_FRAC_DENOM;
	do_div(temp64, parent_rate);
	divff = temp64;

	val = readl_relaxed(pll->base + PLL_CFG1);
	val &= ~((PLL_FRAC_DIV_MASK << 7) | (PLL_INT_DIV_MASK));
	val |= ((divff << 7) | (divfi - 1));
	writel_relaxed(val, pll->base + PLL_CFG1);

	val = readl_relaxed(pll->base + PLL_CFG0);
	val &= ~0x1f;
	writel_relaxed(val, pll->base + PLL_CFG0);

	/* Set the NEV_DIV_VAL to reload the DIVFI and DIVFF */
	val = readl_relaxed(pll->base + PLL_CFG0);
	val |= PLL_NEWDIV_VAL;
	writel_relaxed(val, pll->base + PLL_CFG0);

	ret = clk_wait_ack(pll);

	/* clear the NEV_DIV_VAL */
	val = readl_relaxed(pll->base + PLL_CFG0);
	val &= ~PLL_NEWDIV_VAL;
	writel_relaxed(val, pll->base + PLL_CFG0);

	return ret;
}

static const struct clk_ops clk_frac_pll_ops = {
	.prepare	= clk_pll_prepare,
	.unprepare	= clk_pll_unprepare,
	.is_prepared	= clk_pll_is_prepared,
	.recalc_rate	= clk_pll_recalc_rate,
	.round_rate	= clk_pll_round_rate,
	.set_rate	= clk_pll_set_rate,
};

struct clk *imx_clk_frac_pll(const char *name, const char *parent_name,
			     void __iomem *base)
{
	struct clk_init_data init;
	struct clk_frac_pll *pll;
	struct clk *clk;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	pll->base = base;
	init.name = name;
	init.ops = &clk_frac_pll_ops;
	init.flags = 0;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	pll->hw.init = &init;

	clk = clk_register(NULL, &pll->hw);
	if (IS_ERR(clk))
		kfree(pll);

	return clk;
}
