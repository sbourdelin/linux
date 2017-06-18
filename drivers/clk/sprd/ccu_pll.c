/*
 * Spreadtrum pll clock driver
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "ccu_pll.h"

#define CCU_PLL_1M	1000000
#define CCU_PLL_10M	(CCU_PLL_1M * 10)

#define pindex(pll, member)		\
	(pll->factors[member].shift / (8 * sizeof(pll->regs[0])))

#define pshift(pll, member)		\
	(pll->factors[member].shift % (8 * sizeof(pll->regs[0])))

#define pwidth(pll, member)		\
	pll->factors[member].width

#define pmask(pll, member)					\
	((pwidth(pll, member)) ?				\
	GENMASK(pwidth(pll, member) + pshift(pll, member) - 1,	\
	pshift(pll, member)) : 0)

#define pinternal(pll, cfg, member)	\
	(cfg[pindex(pll, member)] & pmask(pll, member))

#define pinternal_val(pll, cfg, member)	\
	(pinternal(pll, cfg, member) >> pshift(pll, member))

static unsigned long pll_get_refin_rate(struct ccu_pll *pll)
{
	u8 shift, index, refin_id = 3;
	u32 mask;
	const unsigned long refin[4] = { 2, 4, 13, 26 };

	if (pwidth(pll, PLL_REFIN)) {
		index = pindex(pll, PLL_REFIN);
		shift = pshift(pll, PLL_REFIN);
		mask = pmask(pll, PLL_REFIN);
		refin_id = (ccu_pll_readl(pll, index) & mask) >> shift;
		if (refin_id > 3)
			refin_id = 3;
	}

	return refin[refin_id];
}

static u8 pll_get_ibias(unsigned long rate, const u64 *table)
{
	u64 i;
	u8 num = table[0];

	for (i = 0; i < num; i++)
		if (rate <= table[i + 1])
			break;

	return i == num ? num - 1 : i;
}

static unsigned long ccu_pll_helper_recalc_rate(struct ccu_pll *pll,
						unsigned long parent_rate)
{
	unsigned long rate, refin, k1, k2;
	unsigned long kint = 0, nint;
	u32 reg_num = pll->regs[0];
	u32 *cfg;
	u32 i;
	u32 mask;

	cfg = kcalloc(reg_num, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	for (i = 0; i < reg_num; i++)
		cfg[i] = ccu_pll_readl(pll, i);

	refin = pll_get_refin_rate(pll);

	if (pinternal(pll, cfg, PLL_PREDIV))
		refin = refin * 2;

	if (pwidth(pll, PLL_POSTDIV) &&
	    ((pll->fflag == 1 && pinternal(pll, cfg, PLL_POSTDIV)) ||
	     (!pll->fflag && !pinternal(pll, cfg, PLL_POSTDIV))))
		refin = refin / 2;

	if (!pinternal(pll, cfg, PLL_DIV_S))
		rate = refin * pinternal_val(pll, cfg, PLL_N) * CCU_PLL_10M;
	else {
		nint = pinternal_val(pll, cfg, PLL_NINT);
		if (pinternal(pll, cfg, PLL_SDM_EN))
			kint = pinternal_val(pll, cfg, PLL_KINT);

		mask = pmask(pll, PLL_KINT);
#ifdef CONFIG_64BIT
		k1 = 1000;
		k2 = 1000;
		rate = DIV_ROUND_CLOSEST(refin * kint * k1,
					 ((mask >> __ffs(mask)) + 1)) *
					 k2 + refin * nint * CCU_PLL_1M;
#else
		k1 = 100;
		k2 = 10000;
		i = pwidth(pll, PLL_KINT);
		i = i < 21 ? 0 : i - 21;
		rate = DIV_ROUND_CLOSEST(refin * (kint >> i) * k1,
					 ((mask >> (__ffs(mask) + i)) + 1)) *
					 k2 + refin * nint * CCU_PLL_1M;
#endif
	}

	return rate;
}

static int ccu_pll_helper_set_rate(struct ccu_pll *pll,
				   unsigned long rate,
				   unsigned long parent_rate)
{
	u32 mask, shift, width, ibias_val, index, kint, nint;
	u32 reg_num = pll->regs[0], i = 0;
	unsigned long refin, fvco = rate;
	struct reg_cfg *cfg;

	cfg = kcalloc(reg_num, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	refin = pll_get_refin_rate(pll);

	mask = pmask(pll, PLL_PREDIV);
	index = pindex(pll, PLL_PREDIV);
	width = pwidth(pll, PLL_PREDIV);
	if (width && (ccu_pll_readl(pll, index) & mask))
		refin = refin * 2;

	mask = pmask(pll, PLL_POSTDIV);
	index = pindex(pll, PLL_POSTDIV);
	width = pwidth(pll, PLL_POSTDIV);
	cfg[index].msk = mask;
	if (width && ((pll->fflag == 1 && fvco <= pll->fvco) ||
		      (pll->fflag == 0 && fvco > pll->fvco)))
		cfg[index].val |= mask;

	if (width && fvco <= pll->fvco)
		fvco = fvco * 2;

	mask = pmask(pll, PLL_DIV_S);
	index = pindex(pll, PLL_DIV_S);
	cfg[index].val |= mask;
	cfg[index].msk |= mask;

	mask = pmask(pll, PLL_SDM_EN);
	index = pindex(pll, PLL_SDM_EN);
	cfg[index].val |= mask;
	cfg[index].msk |= mask;

	nint  = fvco/(refin * CCU_PLL_1M);

	mask = pmask(pll, PLL_NINT);
	index = pindex(pll, PLL_NINT);
	shift = pshift(pll, PLL_NINT);
	cfg[index].val |= (nint << shift) & mask;
	cfg[index].msk |= mask;

	mask = pmask(pll, PLL_KINT);
	index = pindex(pll, PLL_KINT);
	width = pwidth(pll, PLL_KINT);
	shift = pshift(pll, PLL_KINT);
#ifndef CONFIG_64BIT
	i = width < 21 ? 0 : i - 21;
#endif
	kint = DIV_ROUND_CLOSEST(((fvco - refin * nint * CCU_PLL_1M)/10000) *
	((mask >> (shift + i)) + 1), refin * 100) << i;
	cfg[index].val |= (kint << shift) & mask;
	cfg[index].msk |= mask;

	ibias_val = pll_get_ibias(fvco, pll->itable);

	mask = pmask(pll, PLL_IBIAS);
	index = pindex(pll, PLL_IBIAS);
	shift = pshift(pll, PLL_IBIAS);
	cfg[index].val |= ibias_val << shift & mask;
	cfg[index].msk |= mask;

	for (i = 0; i < reg_num; i++) {
		if (cfg[i].msk)
			ccu_pll_writel(pll, i, cfg[i].val, cfg[i].msk);
	}

	udelay(pll->udelay);

	return 0;
}

static unsigned long ccu_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	return ccu_pll_helper_recalc_rate(pll, parent_rate);
}

static int ccu_pll_set_rate(struct clk_hw *hw,
			    unsigned long rate,
			    unsigned long parent_rate)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	return ccu_pll_helper_set_rate(pll, rate, parent_rate);
}

static int ccu_pll_clk_prepare(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	udelay(pll->udelay);

	return 0;
}

static long ccu_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *prate)
{
	return rate;
}

const struct clk_ops ccu_pll_ops = {
	.prepare = ccu_pll_clk_prepare,
	.recalc_rate = ccu_pll_recalc_rate,
	.round_rate = ccu_pll_round_rate,
	.set_rate = ccu_pll_set_rate,
};
