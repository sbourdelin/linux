// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic JZ47xx SoC TCU clocks driver
 * Copyright (C) 2018 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/ingenic-tcu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/ingenic,tcu.h>

enum ingenic_version {
	ID_JZ4740,
	ID_JZ4770,
	ID_JZ4780,
};

struct ingenic_tcu {
	struct device_node *np;
	struct regmap *map;

	struct clk_onecell_data clocks;
};

struct ingenic_tcu_clk_info {
	struct clk_init_data init_data;
	u8 gate_bit;
	u8 tcsr_reg;
};

struct ingenic_tcu_clk {
	struct clk_hw hw;

	struct ingenic_tcu *tcu;
	const struct ingenic_tcu_clk_info *info;

	unsigned int idx;
};

#define to_tcu_clk(_hw) container_of(_hw, struct ingenic_tcu_clk, hw)

static int ingenic_tcu_enable(struct clk_hw *hw)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	struct ingenic_tcu *tcu = tcu_clk->tcu;

	regmap_write(tcu->map, REG_TSCR, BIT(info->gate_bit));
	return 0;
}

static void ingenic_tcu_disable(struct clk_hw *hw)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;

	regmap_write(tcu->map, REG_TSSR, BIT(info->gate_bit));
}

static int ingenic_tcu_is_enabled(struct clk_hw *hw)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	unsigned int value;

	regmap_read(tcu->map, REG_TSR, &value);

	return !(value & BIT(info->gate_bit));
}

static u8 ingenic_tcu_get_parent(struct clk_hw *hw)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	unsigned int val = 0;
	int ret;

	ret = regmap_read(tcu->map, info->tcsr_reg, &val);
	WARN_ONCE(ret < 0, "Unable to read TCSR %i", tcu_clk->idx);

	return (u8) ffs(val & TCSR_PARENT_CLOCK_MASK) - 1;
}

static int ingenic_tcu_set_parent(struct clk_hw *hw, u8 idx)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	int ret;

	/*
	 * Our clock provider has the CLK_SET_PARENT_GATE flag set, so we know
	 * that the clk is in unprepared state. To be able to access TCSR
	 * we must ungate the clock supply and we gate it again when done.
	 */

	regmap_write(tcu->map, REG_TSCR, BIT(info->gate_bit));

	ret = regmap_update_bits(tcu->map, info->tcsr_reg,
			TCSR_PARENT_CLOCK_MASK, BIT(idx));
	WARN_ONCE(ret < 0, "Unable to update TCSR %i", tcu_clk->idx);

	regmap_write(tcu->map, REG_TSSR, BIT(info->gate_bit));

	return 0;
}

static unsigned long ingenic_tcu_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	unsigned int prescale;
	int ret;

	ret = regmap_read(tcu->map, info->tcsr_reg, &prescale);
	WARN_ONCE(ret < 0, "Unable to read TCSR %i", tcu_clk->idx);

	prescale = (prescale & TCSR_PRESCALE_MASK) >> TCSR_PRESCALE_LSB;

	return parent_rate >> (prescale * 2);
}

static long ingenic_tcu_round_rate(struct clk_hw *hw, unsigned long req_rate,
		unsigned long *parent_rate)
{
	long rate = (long) *parent_rate;
	unsigned int shift;

	if (req_rate > rate)
		return -EINVAL;

	for (shift = 0; shift < 10; shift += 2)
		if ((rate >> shift) <= req_rate)
			return rate >> shift;

	return rate >> 10;
}

static int ingenic_tcu_set_rate(struct clk_hw *hw, unsigned long req_rate,
		unsigned long parent_rate)
{
	struct ingenic_tcu_clk *tcu_clk = to_tcu_clk(hw);
	const struct ingenic_tcu_clk_info *info = tcu_clk->info;
	struct ingenic_tcu *tcu = tcu_clk->tcu;
	u8 prescale = (ffs(parent_rate / req_rate) / 2) << TCSR_PRESCALE_LSB;
	int ret;

	/*
	 * Our clock provider has the CLK_SET_RATE_GATE flag set, so we know
	 * that the clk is in unprepared state. To be able to access TCSR
	 * we must ungate the clock supply and we gate it again when done.
	 */

	regmap_write(tcu->map, REG_TSCR, BIT(info->gate_bit));

	ret = regmap_update_bits(tcu->map, info->tcsr_reg,
				TCSR_PRESCALE_MASK, prescale);
	WARN_ONCE(ret < 0, "Unable to update TCSR %i", tcu_clk->idx);

	regmap_write(tcu->map, REG_TSSR, BIT(info->gate_bit));

	return 0;
}

static const struct clk_ops ingenic_tcu_clk_ops = {
	.get_parent	= ingenic_tcu_get_parent,
	.set_parent	= ingenic_tcu_set_parent,

	.recalc_rate	= ingenic_tcu_recalc_rate,
	.round_rate	= ingenic_tcu_round_rate,
	.set_rate	= ingenic_tcu_set_rate,

	.enable		= ingenic_tcu_enable,
	.disable	= ingenic_tcu_disable,
	.is_enabled	= ingenic_tcu_is_enabled,
};

static const char * const ingenic_tcu_timer_parents[] = {
	"pclk",
	"rtc",
	"ext",
};

static const struct ingenic_tcu_clk_info ingenic_tcu_clk_info[] = {
#define DEF_TIMER(_name, _gate_bit, _tcsr)				\
	{								\
		.init_data = {						\
			.name = _name,					\
			.parent_names = ingenic_tcu_timer_parents,	\
			.num_parents = ARRAY_SIZE(ingenic_tcu_timer_parents),\
			.ops = &ingenic_tcu_clk_ops,			\
			.flags = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE,\
		},							\
		.gate_bit = _gate_bit,					\
		.tcsr_reg = _tcsr,					\
	}
	[JZ4740_CLK_TIMER0] = DEF_TIMER("timer0", 0, REG_TCSRc(0)),
	[JZ4740_CLK_TIMER1] = DEF_TIMER("timer1", 1, REG_TCSRc(1)),
	[JZ4740_CLK_TIMER2] = DEF_TIMER("timer2", 2, REG_TCSRc(2)),
	[JZ4740_CLK_TIMER3] = DEF_TIMER("timer3", 3, REG_TCSRc(3)),
	[JZ4740_CLK_TIMER4] = DEF_TIMER("timer4", 4, REG_TCSRc(4)),
	[JZ4740_CLK_TIMER5] = DEF_TIMER("timer5", 5, REG_TCSRc(5)),
	[JZ4740_CLK_TIMER6] = DEF_TIMER("timer6", 6, REG_TCSRc(6)),
	[JZ4740_CLK_TIMER7] = DEF_TIMER("timer7", 7, REG_TCSRc(7)),
	[JZ4740_CLK_WDT]    = DEF_TIMER("wdt",   16, REG_WDT_TCSR),
	[JZ4770_CLK_OST]    = DEF_TIMER("ost",   15, REG_OST_TCSR),
#undef DEF_TIMER
};

static int ingenic_tcu_register_clock(struct ingenic_tcu *tcu, unsigned int idx,
		const struct ingenic_tcu_clk_info *info)
{
	struct ingenic_tcu_clk *tcu_clk;
	struct clk *clk;
	int err;

	tcu_clk = kzalloc(sizeof(*tcu_clk), GFP_KERNEL);
	if (!tcu_clk)
		return -ENOMEM;

	tcu_clk->hw.init = &info->init_data;
	tcu_clk->idx = idx;
	tcu_clk->info = info;
	tcu_clk->tcu = tcu;

	/* Set EXT as the default parent clock */
	ingenic_tcu_set_parent(&tcu_clk->hw, 2);

	ingenic_tcu_disable(&tcu_clk->hw);

	clk = clk_register(NULL, &tcu_clk->hw);
	if (IS_ERR(clk)) {
		err = PTR_ERR(clk);
		goto err_free_tcu_clk;
	}

	err = clk_register_clkdev(clk, info->init_data.name, NULL);
	if (err)
		goto err_clk_unregister;

	tcu->clocks.clks[idx] = clk;
	return 0;

err_clk_unregister:
	clk_unregister(clk);
err_free_tcu_clk:
	kfree(tcu_clk);
	return err;
}

static void __init ingenic_tcu_init(struct device_node *np,
		enum ingenic_version id)
{
	struct ingenic_tcu *tcu;
	size_t i, nb_clks;
	int ret = -ENOMEM;

	if (id >= ID_JZ4770)
		nb_clks = (JZ4770_CLK_LAST - JZ4740_CLK_TIMER0) + 1;
	else
		nb_clks = (JZ4740_CLK_LAST - JZ4740_CLK_TIMER0) + 1;

	tcu = kzalloc(sizeof(*tcu), GFP_KERNEL);
	if (!tcu) {
		pr_err("%s: cannot allocate memory\n", __func__);
		return;
	}

	tcu->map = syscon_node_to_regmap(np->parent);
	if (IS_ERR(tcu->map)) {
		pr_err("%s: failed to map TCU registers\n", __func__);
		goto err_free_tcu;
	}

	tcu->clocks.clk_num = nb_clks;
	tcu->clocks.clks = kcalloc(nb_clks, sizeof(struct clk *), GFP_KERNEL);
	if (!tcu->clocks.clks) {
		pr_err("%s: cannot allocate memory\n", __func__);
		goto err_free_tcu;
	}

	for (i = 0; i < nb_clks; i++) {
		ret = ingenic_tcu_register_clock(tcu, i,
				&ingenic_tcu_clk_info[JZ4740_CLK_TIMER0 + i]);
		if (ret) {
			pr_err("%s: cannot register clocks\n", __func__);
			goto err_unregister;
		}
	}

	ret = of_clk_add_provider(np, of_clk_src_onecell_get, &tcu->clocks);
	if (ret) {
		pr_err("%s: cannot add OF clock provider\n", __func__);
		goto err_unregister;
	}

	return;

err_unregister:
	for (i = 0; i < tcu->clocks.clk_num; i++)
		if (tcu->clocks.clks[i])
			clk_unregister(tcu->clocks.clks[i]);
	kfree(tcu->clocks.clks);
err_free_tcu:
	kfree(tcu);
}

static void __init jz4740_tcu_init(struct device_node *np)
{
	ingenic_tcu_init(np, ID_JZ4740);
}
CLK_OF_DECLARE(ingenic_tcu, "ingenic,jz4740-tcu-clocks", jz4740_tcu_init);

static void __init jz4770_tcu_init(struct device_node *np)
{
	ingenic_tcu_init(np, ID_JZ4770);
}
CLK_OF_DECLARE(jz4770_tcu, "ingenic,jz4770-tcu-clocks", jz4770_tcu_init);

static void __init jz4780_tcu_init(struct device_node *np)
{
	ingenic_tcu_init(np, ID_JZ4780);
}
CLK_OF_DECLARE(jz4780_tcu, "ingenic,jz4780-tcu-clocks", jz4780_tcu_init);
