/*
 * Spreatrum pll clock driver
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/delay.h>
#include "pll_cfg.h"

struct sprd_pll_config *g_sprd_pll_config;

static void pll_write(void __iomem *reg, u32 val, u32 msk)
{
	writel_relaxed((readl_relaxed(reg) & ~msk) | val, reg);
}

static unsigned long pll_get_refin_rate(struct sprd_pll_hw *pll,
					struct sprd_pll_config *ppll_config)
{
	u32 i = 3;
	u8 index;
	u32 value;
	const unsigned long refin[4] = { 2, 4, 13, 26 };

	value = ppll_config->refin_msk.value;
	index = ppll_config->refin_msk.index;
	if (value) {
		i = (readl_relaxed(pll->reg[index]) & value) >> __ffs(value);
		i = i > 3 ? 3 : i;
	}

	return refin[i];
}

static u8 pll_get_ibias(unsigned long rate, struct pll_ibias_table *table)
{
	if (!table)
		return 0;

	for (; table->rate < SPRD_PLL_MAX_RATE; table++)
		if (rate <= table->rate)
			break;

	return table->ibias;
}

static void *pll_get_config(struct clk_hw *hw,
			    struct sprd_pll_config *pll_config)
{
	struct sprd_pll_config *p;

	for (p = pll_config; p->name != NULL; p++)
		if (!strcmp(p->name, __clk_get_name(hw->clk)))
			break;

	return p->name ? p : NULL;
}

static int pll_clk_prepare(struct clk_hw *hw)
{
	struct sprd_pll_config *pcfg;

	pcfg = pll_get_config(hw, g_sprd_pll_config);
	if (!pcfg)
		return -EPERM;

	udelay(pcfg->udelay);

	return 0;
}

static long pll_round_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long *prate)
{
	return rate;
}

static inline int pll_check(struct sprd_pll_hw *pll,
			    struct sprd_pll_config *ppll_config)
{
	if ((ppll_config->lock_done.index >= pll->reg_num)	||
	    (ppll_config->div_s.index >= pll->reg_num)		||
	    (ppll_config->mod_en.index >= pll->reg_num)		||
	    (ppll_config->sdm_en.index >= pll->reg_num)		||
	    (ppll_config->refin_msk.index >= pll->reg_num)	||
	    (ppll_config->ibias_msk.index >= pll->reg_num)	||
	    (ppll_config->pll_n_msk.index >= pll->reg_num)	||
	    (ppll_config->nint_msk.index >= pll->reg_num)	||
	    (ppll_config->kint_msk.index >= pll->reg_num)	||
	    (ppll_config->prediv_msk.index >= pll->reg_num)) {
		pr_err("%s: pll[%s] exceed max:%d\n", __func__,
		       __clk_get_name(pll->hw.clk), pll->reg_num);

		return -EINVAL;
	}

	return 0;
}

/* get field */
#define gf(array, pll_struct) \
	(array[pll_struct.index] & pll_struct.value)

/* get field value */
#define gfv(array, pll_struct) \
	(gf(array, pll_struct) >> __ffs(pll_struct.value))

static unsigned long pll_recalc_rate(struct sprd_pll_hw *pll,
				     struct sprd_pll_config *ppll_config,
				     unsigned long parent_rate)
{
	unsigned long rate, refin, k1, k2;
	unsigned long kint = 0, nint, cfg[SPRD_PLL_MAX_REGNUM], n;
	int i;
	u32 value;

	if (!ppll_config) {
		pr_err("%s:%d Cannot get pll %s\n", __func__,
			__LINE__, __clk_get_name(pll->hw.clk));
		return parent_rate;
	}

	if (pll_check(pll, ppll_config))
		return parent_rate;

	for (i = 0; i < pll->reg_num; i++)
		cfg[i] = readl_relaxed(pll->reg[i]);

	refin = pll_get_refin_rate(pll, ppll_config);

	if (gf(cfg, ppll_config->prediv_msk))
		refin = refin * 2;

	if (ppll_config->postdiv_msk.value &&
	    (((ppll_config->postdiv_msk.fvco_threshold->flag == 1) &&
	      gf(cfg, ppll_config->postdiv_msk)) ||
	     ((ppll_config->postdiv_msk.fvco_threshold->flag == 0) &&
	      !gf(cfg, ppll_config->postdiv_msk))))
		refin = refin / 2;

	if (!gf(cfg, ppll_config->div_s)) {
		n = gfv(cfg, ppll_config->pll_n_msk);
		rate = refin * n * 10000000;
	} else {
		nint = gfv(cfg, ppll_config->nint_msk);
		if (gf(cfg, ppll_config->sdm_en))
			kint = gfv(cfg, ppll_config->kint_msk);

		value = ppll_config->kint_msk.value;
#ifdef CONFIG_64BIT
		k1 = 1000;
		k2 = 1000;
		i = 0;
#else
		k1 = 100;
		k2 = 10000;
		i = fls(value >> __ffs(value));
		i = i < 20 ? 0 : i - 20;
#endif
		rate = DIV_ROUND_CLOSEST(refin * (kint >> i) * k1,
					 ((value >> (__ffs(value) + i)) + 1)) *
					 k2 + refin * nint * 1000000;
	}

	return rate;
}

static int pll_adjustable_set_rate(struct sprd_pll_hw *pll,
				   struct sprd_pll_config *ppll_config,
				   unsigned long rate,
				   unsigned long parent_rate)
{
	u8 ibias, index;
	u32 value;
	unsigned long kint, nint;
	unsigned long refin, val, fvco = rate;
	struct reg_cfg cfg[SPRD_PLL_MAX_REGNUM] = {{},};
	struct fvco_threshold *ft;
	int i = 0;

	if (ppll_config == NULL) {
		pr_err("%s:%d Cannot get pll clk[%s]\n", __func__,
			__LINE__, __clk_get_name(pll->hw.clk));
		return -EINVAL;
	}

	if (pll_check(pll, ppll_config))
		return -EINVAL;

	/* calc the pll refin */
	refin = pll_get_refin_rate(pll, ppll_config);

	value = ppll_config->prediv_msk.value;
	index = ppll_config->prediv_msk.index;
	if (value) {
		val = readl_relaxed(pll->reg[index]);
		if (val & value)
			refin = refin * 2;
	}

	value = ppll_config->postdiv_msk.value;
	index = ppll_config->postdiv_msk.index;
	ft = ppll_config->postdiv_msk.fvco_threshold;
	cfg[index].msk = value;
	if (value && ((ft->flag == 1 && fvco <= ft->rate) ||
		      (ft->flag == 0 && fvco > ft->rate)))
		cfg[index].val |= value;

	if (fvco <= ft->rate)
		fvco = fvco * 2;

	value = ppll_config->div_s.value;
	index = ppll_config->div_s.index;
	cfg[index].val |= value;
	cfg[index].msk |= value;

	value = ppll_config->sdm_en.value;
	index = ppll_config->sdm_en.index;
	cfg[index].val |= value;
	cfg[index].msk |= value;

	nint  = fvco/(refin * 1000000);

	value = ppll_config->nint_msk.value;
	index = ppll_config->nint_msk.index;
	cfg[index].val |= (nint << __ffs(value)) & value;
	cfg[index].msk |= value;

	value = ppll_config->kint_msk.value;
	index = ppll_config->kint_msk.index;
#ifndef CONFIG_64BIT
	i = fls(value >> __ffs(value));
	i = i < 20 ? 0 : i - 20;
#endif
	kint = DIV_ROUND_CLOSEST(((fvco - refin * nint * 1000000)/10000) *
	((value >> (__ffs(value) + i)) + 1), refin * 100) << i;
	cfg[index].val |= (kint << __ffs(value)) & value;
	cfg[index].msk |= value;

	ibias = pll_get_ibias(fvco, ppll_config->itable);
	value = ppll_config->ibias_msk.value;
	index = ppll_config->ibias_msk.index;
	cfg[index].val |= ibias << __ffs(value) & value;
	cfg[index].msk |= value;

	for (i = 0; i < pll->reg_num; i++)
		if (cfg[i].msk)
			pll_write(pll->reg[i], cfg[i].val, cfg[i].msk);

	udelay(ppll_config->udelay);

	return 0;
}

static void pll_clk_setup(struct device_node *node,
			  const struct clk_ops *clk_ops)
{
	struct clk *clk = NULL;
	const char *parent_names;
	struct sprd_pll_hw *pll;
	int reg_num, index;
	struct clk_init_data init = {
		.ops = clk_ops,
		.flags = CLK_IGNORE_UNUSED,
		.num_parents = 1,
	};

	parent_names = of_clk_get_parent_name(node, 0);
	if (!parent_names) {
		pr_err("%s: Failed to get parent_names in node[%s]\n",
			__func__, node->name);
		return;
	}
	init.parent_names = &parent_names;

	if (of_property_read_string(node, "clock-output-names", &init.name))
		return;

	pll = kzalloc(sizeof(struct sprd_pll_hw), GFP_KERNEL);
	if (!pll)
		return;

	reg_num = of_property_count_u32_elems(node, "reg");
	reg_num = reg_num / (of_n_addr_cells(node) + of_n_size_cells(node));
	if (reg_num > SPRD_PLL_MAX_REGNUM) {
		pr_err("%s: reg_num:%d exceed max number\n",
		       __func__, reg_num);
		goto kfree_pll;
	}
	pll->reg_num = reg_num;

	for (index = 0; index < reg_num; index++) {
		pll->reg[index] = of_iomap(node, index);
		if (!pll->reg[index])
			goto kfree_pll;
	}

	pll->hw.init = &init;

	clk = clk_register(NULL, &pll->hw);
	if (!IS_ERR(clk)) {
		clk_register_clkdev(clk, init.name, 0);
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		return;
	}

kfree_pll:
	kfree(pll);
}

static unsigned long sprd_adjustable_pll_recalc_rate(struct clk_hw *hw,
						     unsigned long parent_rate)
{
	struct sprd_pll_config *ppll_config;
	struct sprd_pll_hw *pll = to_sprd_pll_hw(hw);

	ppll_config = pll_get_config(hw, g_sprd_pll_config);

	return pll_recalc_rate(pll, ppll_config, parent_rate);
}

static int sprd_adjustable_pll_set_rate(struct clk_hw *hw,
					unsigned long rate,
					unsigned long parent_rate)
{
	struct sprd_pll_config *ppll_config;
	struct sprd_pll_hw *pll = to_sprd_pll_hw(hw);

	ppll_config = pll_get_config(hw, g_sprd_pll_config);

	return pll_adjustable_set_rate(pll, ppll_config, rate,
					    parent_rate);
}

const struct clk_ops sprd_adjustable_pll_ops = {
	.prepare = pll_clk_prepare,
	.round_rate = pll_round_rate,
	.set_rate = sprd_adjustable_pll_set_rate,
	.recalc_rate = sprd_adjustable_pll_recalc_rate,
};

static void __init sc9836_adjustable_pll_setup(struct device_node *node)
{
	g_sprd_pll_config = sc9836_pll_config;
	pll_clk_setup(node, &sprd_adjustable_pll_ops);
}

static void __init sc9860_adjustable_pll_setup(struct device_node *node)
{
	g_sprd_pll_config = sc9860_pll_config;
	pll_clk_setup(node, &sprd_adjustable_pll_ops);
}

CLK_OF_DECLARE(sc9836_adjustable_pll_clock, "sprd,sc9836-adjustable-pll-clock",
	       sc9836_adjustable_pll_setup);
CLK_OF_DECLARE(sc9860_adjustable_pll_clock, "sprd,sc9860-adjustable-pll-clock",
	       sc9860_adjustable_pll_setup);
