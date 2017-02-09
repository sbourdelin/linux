/*
 * Synopsys AXS10X SDP Generic PLL clock driver
 *
 * Copyright (C) 2017 Synopsys
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/of.h>

/* PLL registers addresses */
#define PLL_REG_IDIV	0x0
#define PLL_REG_FBDIV	0x4
#define PLL_REG_ODIV	0x8

/*
 * Bit fields of the PLL IDIV/FBDIV/ODIV registers:
 *  ________________________________________________________________________
 * |31                15|    14    |   13   |  12  |11         6|5         0|
 * |-------RESRVED------|-NOUPDATE-|-BYPASS-|-EDGE-|--HIGHTIME--|--LOWTIME--|
 * |____________________|__________|________|______|____________|___________|
 *
 * Following macros detirmine the way of access to these registers
 * They should be set up only using the macros.
 * reg should be and uint32_t variable.
 */

#define PLL_REG_GET_LOW(reg)			\
	(((reg) & (0x3F << 0)) >> 0)
#define PLL_REG_GET_HIGH(reg)			\
	(((reg) & (0x3F << 6)) >> 6)
#define PLL_REG_GET_EDGE(reg)			\
	(((reg) & (BIT(12))) ? 1 : 0)
#define PLL_REG_GET_BYPASS(reg)			\
	(((reg) & (BIT(13))) ? 1 : 0)
#define PLL_REG_GET_NOUPD(reg)			\
	(((reg) & (BIT(14))) ? 1 : 0)
#define PLL_REG_GET_PAD(reg)			\
	(((reg) & (0x1FFFF << 15)) >> 15)

#define PLL_REG_SET_LOW(reg, value)		\
	{ reg |= (((value) & 0x3F) << 0); }
#define PLL_REG_SET_HIGH(reg, value)	\
	{ reg |= (((value) & 0x3F) << 6); }
#define PLL_REG_SET_EDGE(reg, value)	\
	{ reg |= (((value) & 0x01) << 12); }
#define PLL_REG_SET_BYPASS(reg, value)	\
	{ reg |= (((value) & 0x01) << 13); }
#define PLL_REG_SET_NOUPD(reg, value)	\
	{ reg |= (((value) & 0x01) << 14); }
#define PLL_REG_SET_PAD(reg, value)		\
	{ reg |= (((value) & 0x1FFFF) << 15); }

#define PLL_LOCK	0x1
#define PLL_MAX_LOCK_TIME 100 /* 100 us */

struct pll_cfg {
	u32 rate;
	u32 idiv;
	u32 fbdiv;
	u32 odiv;
};

struct pll_of_table {
	unsigned long prate;
	struct pll_cfg *pll_cfg_table;
};

struct pll_of_data {
	struct pll_of_table *pll_table;
};

static struct pll_of_data pgu_pll_data = {
	.pll_table = (struct pll_of_table []){
		{
			.prate = 27000000,
			.pll_cfg_table = (struct pll_cfg []){
				{ 25200000, 1, 84, 90 },
				{ 50000000, 1, 100, 54 },
				{ 74250000, 1, 44, 16 },
				{ },
			},
		},
		/* Used as list limiter */
		{ },
	},
};

static struct pll_of_data arc_pll_data = {
	.pll_table = (struct pll_of_table []){
		{
			.prate = 33333333,
			.pll_cfg_table = (struct pll_cfg []){
				{ 33333333,  1, 1,  1 },
				{ 50000000,  1, 30, 20 },
				{ 75000000,  2, 45, 10 },
				{ 90000000,  2, 54, 10 },
				{ 100000000, 1, 30, 10 },
				{ 125000000, 2, 45, 6 },
				{ },
			},
		},
		/* Used as list limiter */
		{ },
	},
};

struct pll_clk {
	void __iomem *base;
	void __iomem *lock;
	const struct pll_of_data *pll_data;
	struct clk_hw hw;
	struct device *dev;
};

static inline void pll_write(struct pll_clk *clk, unsigned int reg,
		unsigned int val)
{
	iowrite32(val, clk->base + reg);
}

static inline u32 pll_read(struct pll_clk *clk,
		unsigned int reg)
{
	return ioread32(clk->base + reg);
}

static inline struct pll_clk *to_pll_clk(struct clk_hw *hw)
{
	return container_of(hw, struct pll_clk, hw);
}

static inline u32 div_get_value(unsigned int reg)
{
	if (PLL_REG_GET_BYPASS(reg))
		return 1;

	return (PLL_REG_GET_HIGH(reg) + PLL_REG_GET_LOW(reg));
}

static inline u32 encode_div(unsigned int id, int upd)
{
	uint32_t div = 0;

	PLL_REG_SET_LOW(div, (id%2 == 0) ? id >> 1 : (id >> 1) + 1);
	PLL_REG_SET_HIGH(div, id >> 1);
	PLL_REG_SET_EDGE(div, id%2);
	PLL_REG_SET_BYPASS(div, id == 1 ? 1 : 0);
	PLL_REG_SET_NOUPD(div, !upd);

	return div;
}

static const struct pll_cfg *pll_get_cfg(unsigned long prate,
		const struct pll_of_table *pll_table)
{
	int i;

	for (i = 0; pll_table[i].prate != 0; i++)
		if (pll_table[i].prate == prate)
			return pll_table[i].pll_cfg_table;

	return NULL;
}

static unsigned long pll_recalc_rate(struct clk_hw *hw,
			unsigned long parent_rate)
{
	u64 rate;
	u32 idiv, fbdiv, odiv;
	struct pll_clk *clk = to_pll_clk(hw);

	idiv = div_get_value(pll_read(clk, PLL_REG_IDIV));
	fbdiv = div_get_value(pll_read(clk, PLL_REG_FBDIV));
	odiv = div_get_value(pll_read(clk, PLL_REG_ODIV));

	rate = (u64)parent_rate * fbdiv;
	do_div(rate, idiv * odiv);

	return (unsigned long)rate;
}

static long pll_round_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long *prate)
{
	int i;
	long best_rate;
	struct pll_clk *clk = to_pll_clk(hw);
	const struct pll_cfg *pll_cfg = pll_get_cfg(*prate,
			clk->pll_data->pll_table);

	if (!pll_cfg) {
		dev_err(clk->dev, "invalid parent rate=%ld\n", *prate);
		return -EINVAL;
	}

	if (pll_cfg[0].rate == 0)
		return -EINVAL;

	best_rate = pll_cfg[0].rate;

	for (i = 1; pll_cfg[i].rate != 0; i++) {
		if (abs(rate - pll_cfg[i].rate) < abs(rate - best_rate))
			best_rate = pll_cfg[i].rate;
	}

	return best_rate;
}

static int pll_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	int i;
	struct pll_clk *clk = to_pll_clk(hw);
	const struct pll_cfg *pll_cfg = pll_get_cfg(parent_rate,
			clk->pll_data->pll_table);

	if (!pll_cfg) {
		dev_err(clk->dev, "invalid parent rate=%ld\n", parent_rate);
		return -EINVAL;
	}

	for (i = 0; pll_cfg[i].rate != 0; i++) {
		if (pll_cfg[i].rate == rate) {
			pll_write(clk, PLL_REG_IDIV,
					encode_div(pll_cfg[i].idiv, 0));
			pll_write(clk, PLL_REG_FBDIV,
					encode_div(pll_cfg[i].fbdiv, 0));
			pll_write(clk, PLL_REG_ODIV,
					encode_div(pll_cfg[i].odiv, 1));

			/*
			 * Wait until CGU relocks.
			 * If after timeout CGU is unlocked yet return error
			 */
			udelay(PLL_MAX_LOCK_TIME);
			if (ioread32(clk->lock) & PLL_LOCK)
				return 0;
			else
				return -ETIMEDOUT;
		}
	}

	dev_err(clk->dev, "invalid rate=%ld, parent_rate=%ld\n", rate,
			parent_rate);
	return -EINVAL;
}

static const struct clk_ops pll_ops = {
	.recalc_rate = pll_recalc_rate,
	.round_rate = pll_round_rate,
	.set_rate = pll_set_rate,
};

static int pll_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *parent_name;
	struct clk *clk;
	struct pll_clk *pll_clk;
	struct resource *mem;
	struct clk_init_data init = { };

	pll_clk = devm_kzalloc(dev, sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pll_clk->base = devm_ioremap_resource(dev, mem);
	if (IS_ERR(pll_clk->base))
		return PTR_ERR(pll_clk->base);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	pll_clk->lock = devm_ioremap_resource(dev, mem);
	if (IS_ERR(pll_clk->lock))
		return PTR_ERR(pll_clk->base);

	init.name = dev->of_node->name;
	init.ops = &pll_ops;
	parent_name = of_clk_get_parent_name(dev->of_node, 0);
	init.parent_names = &parent_name;
	init.num_parents = 1;
	pll_clk->hw.init = &init;
	pll_clk->dev = dev;
	pll_clk->pll_data = of_device_get_match_data(dev);

	if (!pll_clk->pll_data) {
		dev_err(dev, "No OF match data provided\n");
			return -EINVAL;
	}

	clk = devm_clk_register(dev, &pll_clk->hw);
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to register %s clock (%ld)\n",
				init.name, PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	return of_clk_add_provider(dev->of_node, of_clk_src_simple_get, clk);
}

static int pll_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	return 0;
}

static void __init of_pll_clk_setup(struct device_node *node)
{
	const char *parent_name;
	struct clk *clk;
	struct pll_clk *pll_clk;
	struct clk_init_data init = { };

	pll_clk = kzalloc(sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return;

	pll_clk->base = of_iomap(node, 0);
	if (!pll_clk->base) {
		pr_err("failed to map pll div registers\n");
		iounmap(pll_clk->base);
		return;
	}

	pll_clk->lock = of_iomap(node, 1);
	if (!pll_clk->lock) {
		pr_err("failed to map pll lock register\n");
		iounmap(pll_clk->lock);
		return;
	}

	init.name = node->name;
	init.ops = &pll_ops;
	parent_name = of_clk_get_parent_name(node, 0);
	init.parent_names = &parent_name;
	init.num_parents = parent_name ? 1 : 0;
	pll_clk->hw.init = &init;
	pll_clk->pll_data = &arc_pll_data;

	clk = clk_register(NULL, &pll_clk->hw);
	if (IS_ERR(clk)) {
		pr_err("failed to register %s clock (%ld)\n",
				node->name, PTR_ERR(clk));
		kfree(pll_clk);
		return;
	}

	of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

CLK_OF_DECLARE(axs10x_pll_clock, "snps,axs10x-arc-pll-clock", of_pll_clk_setup);

static const struct of_device_id pll_clk_id[] = {
	{ .compatible = "snps,axs10x-arc-pll-clock", .data = &arc_pll_data},
	{ .compatible = "snps,axs10x-pgu-pll-clock", .data = &pgu_pll_data},
	{ },
};
MODULE_DEVICE_TABLE(of, pll_clk_id);

static struct platform_driver pll_clk_driver = {
	.driver = {
		.name = "axs10x-pll-clock",
		.of_match_table = pll_clk_id,
	},
	.probe = pll_clk_probe,
	.remove = pll_clk_remove,
};
builtin_platform_driver(pll_clk_driver);

MODULE_AUTHOR("Vlad Zakharov <vzakhar@synopsys.com>");
MODULE_DESCRIPTION("Synopsys AXS10X SDP Generic PLL Clock Driver");
MODULE_LICENSE("GPL v2");
