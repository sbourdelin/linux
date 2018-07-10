// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Amlogic Meson MMC Sub Clock Controller Driver
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Yixun Lan <yixun.lan@amlogic.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/amlogic,meson-mmc-clkc.h>

#include "clkc.h"

/* clock ID used by internal driver */
#define CLKID_MMC_MUX				0
#define CLKID_MMC_PHASE_CORE			2

#define SD_EMMC_CLOCK		0
#define   CLK_DIV_MASK GENMASK(5, 0)
#define   CLK_SRC_MASK GENMASK(7, 6)
#define   CLK_CORE_PHASE_MASK GENMASK(9, 8)
#define   CLK_TX_PHASE_MASK GENMASK(11, 10)
#define   CLK_RX_PHASE_MASK GENMASK(13, 12)
#define   CLK_V2_TX_DELAY_MASK GENMASK(19, 16)
#define   CLK_V2_RX_DELAY_MASK GENMASK(23, 20)
#define   CLK_V2_ALWAYS_ON BIT(24)

#define   CLK_V3_TX_DELAY_MASK GENMASK(21, 16)
#define   CLK_V3_RX_DELAY_MASK GENMASK(27, 22)
#define   CLK_V3_ALWAYS_ON BIT(28)

#define   CLK_DELAY_STEP_PS 200
#define   CLK_PHASE_STEP 30
#define   CLK_PHASE_POINT_NUM (360 / CLK_PHASE_STEP)

#define MUX_CLK_NUM_PARENTS	2
#define MMC_MAX_CLKS		5

struct clk_regmap_phase_data {
	unsigned long			phase_mask;
	unsigned long			delay_mask;
	unsigned int			delay_step_ps;
};

struct mmc_clkc_data {
	struct clk_regmap_phase_data	tx;
	struct clk_regmap_phase_data	rx;
};

struct mmc_clkc_info {
	struct device			*dev;
	struct regmap			*map;
	struct mmc_clkc_data		*data;
};

static inline struct clk_regmap_phase_data *
clk_get_regmap_phase_data(struct clk_regmap *clk)
{
	return (struct clk_regmap_phase_data *)clk->data;
}

static struct clk_regmap_mux_data mmc_clkc_mux_data = {
	.offset = SD_EMMC_CLOCK,
	.mask = 0x3,
	.shift = 6,
	.flags = CLK_DIVIDER_ROUND_CLOSEST,
};

static struct clk_regmap_div_data mmc_clkc_div_data = {
	.offset = SD_EMMC_CLOCK,
	.shift = 0,
	.width = 6,
	.flags = CLK_DIVIDER_ROUND_CLOSEST | CLK_DIVIDER_ONE_BASED,
};

static struct clk_regmap_phase_data mmc_clkc_core_phase = {
	.phase_mask	= CLK_CORE_PHASE_MASK,
};

static const struct mmc_clkc_data mmc_clkc_gx_data = {
	{
		.phase_mask	= CLK_TX_PHASE_MASK,
		.delay_mask	= CLK_V2_TX_DELAY_MASK,
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
	{
		.phase_mask	= CLK_RX_PHASE_MASK,
		.delay_mask	= CLK_V2_RX_DELAY_MASK,
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
};

static const struct mmc_clkc_data mmc_clkc_axg_data = {
	{
		.phase_mask	= CLK_TX_PHASE_MASK,
		.delay_mask	= CLK_V3_TX_DELAY_MASK,
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
	{
		.phase_mask	= CLK_RX_PHASE_MASK,
		.delay_mask	= CLK_V3_RX_DELAY_MASK,
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
};

static const struct of_device_id mmc_clkc_match_table[] = {
	{
		.compatible = "amlogic,meson-gx-mmc-clkc",
		.data = &mmc_clkc_gx_data
	},
	{
		.compatible = "amlogic,meson-axg-mmc-clkc",
		.data = &mmc_clkc_axg_data
	},
	{}
};
MODULE_DEVICE_TABLE(of, mmc_clkc_match_table);

static struct clk_regmap *mmc_clkc_register_mux(struct mmc_clkc_info *clkc)
{
	const char *parent_names[MUX_CLK_NUM_PARENTS];
	struct device *dev = clkc->dev;
	struct clk_init_data init;
	struct clk_regmap *mux;
	struct clk *clk;
	char *mux_name;
	int i, ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < MUX_CLK_NUM_PARENTS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "clkin%d", i);
		clk = devm_clk_get(dev, name);
		if (IS_ERR(clk)) {
			if (clk != ERR_PTR(-EPROBE_DEFER))
				dev_err(dev, "Missing clock %s\n", name);
			return ERR_PTR((long)clk);
		}

		parent_names[i] = __clk_get_name(clk);
	}

	mux_name = kasprintf(GFP_KERNEL, "%s#mux", dev_name(dev));
	if (!mux_name)
		return ERR_PTR(-ENOMEM);

	mux->map = clkc->map;
	mux->data = &mmc_clkc_mux_data;

	init.name = mux_name;
	init.ops = &clk_regmap_mux_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = parent_names;
	init.num_parents = MUX_CLK_NUM_PARENTS;

	mux->hw.init = &init;
	ret = devm_clk_hw_register(dev, &mux->hw);
	if (ret) {
		dev_err(dev, "Mux clock registration failed\n");
		mux = ERR_PTR(ret);
	}

	kfree(mux_name);
	return mux;
}

static struct clk_regmap *mmc_clkc_register_div(struct mmc_clkc_info *clkc)
{
	const char *parent_names[MUX_CLK_NUM_PARENTS];
	struct device *dev = clkc->dev;
	struct clk_init_data init;
	struct clk_regmap *div;
	char *mux_name, *div_name;
	int ret;

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	mux_name = kasprintf(GFP_KERNEL, "%s#mux", dev_name(dev));
	div_name = kasprintf(GFP_KERNEL, "%s#div", dev_name(dev));
	if (!mux_name || !div_name) {
		div = ERR_PTR(-ENOMEM);
		goto err;
	}

	parent_names[0] = mux_name;
	div->map = clkc->map;
	div->data = &mmc_clkc_div_data;

	init.name = div_name;
	init.ops = &clk_regmap_divider_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = parent_names;
	init.num_parents = 1;

	div->hw.init = &init;
	ret = devm_clk_hw_register(dev, &div->hw);
	if (ret) {
		dev_err(dev, "Divider clock registration failed\n");
		div = ERR_PTR(ret);
	}

err:
	kfree(mux_name);
	kfree(div_name);
	return div;
}

static int clk_regmap_get_phase(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_phase_data *ph = clk_get_regmap_phase_data(clk);
	unsigned int phase_num = 1 <<  hweight_long(ph->phase_mask);
	unsigned long period_ps, p, d;
	int degrees;
	u32 val;

	regmap_read(clk->map, SD_EMMC_CLOCK, &val);
	p = (val & ph->phase_mask) >> __ffs(ph->phase_mask);
	degrees = p * 360 / phase_num;

	if (ph->delay_mask) {
		period_ps = DIV_ROUND_UP((unsigned long)NSEC_PER_SEC * 1000,
					 clk_get_rate(hw->clk));
		d = (val & ph->delay_mask) >> __ffs(ph->delay_mask);
		degrees += d * ph->delay_step_ps * 360 / period_ps;
		degrees %= 360;
	}

	return degrees;
}

static void clk_regmap_apply_phase_delay(struct clk_regmap *clk,
					 unsigned int phase,
					 unsigned int delay)
{
	struct clk_regmap_phase_data *ph = clk->data;
	u32 val;

	regmap_read(clk->map, SD_EMMC_CLOCK, &val);

	val &= ~ph->phase_mask;
	val |= phase << __ffs(ph->phase_mask);

	if (ph->delay_mask) {
		val &= ~ph->delay_mask;
		val |= delay << __ffs(ph->delay_mask);
	}

	regmap_write(clk->map, SD_EMMC_CLOCK, val);
}

static int clk_regmap_set_phase(struct clk_hw *hw, int degrees)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_phase_data *ph = clk_get_regmap_phase_data(clk);
	unsigned int phase_num = 1 <<  hweight_long(ph->phase_mask);
	unsigned long period_ps, d = 0, r;
	u64 p;

	p = degrees % 360;

	if (!ph->delay_mask) {
		p = DIV_ROUND_CLOSEST_ULL(p, 360 / phase_num);
	} else {
		period_ps = DIV_ROUND_UP((unsigned long)NSEC_PER_SEC * 1000,
					 clk_get_rate(hw->clk));

		/* First compute the phase index (p), the remainder (r) is the
		 * part we'll try to acheive using the delays (d).
		 */
		r = do_div(p, 360 / phase_num);
		d = DIV_ROUND_CLOSEST(r * period_ps,
				      360 * ph->delay_step_ps);
		d = min(d, ph->delay_mask >> __ffs(ph->delay_mask));
	}

	clk_regmap_apply_phase_delay(clk, p, d);
	return 0;
}

static const struct clk_ops clk_regmap_phase_ops = {
	.get_phase = clk_regmap_get_phase,
	.set_phase = clk_regmap_set_phase,
};

static struct clk_regmap *
mmc_clkc_register_phase_clk(struct mmc_clkc_info *clkc,
			    char *name, char *parent_name, unsigned long flags,
			    struct clk_regmap_phase_data *phase_data)

{
	const char *parent_names[MUX_CLK_NUM_PARENTS];
	struct device *dev = clkc->dev;
	struct clk_init_data init;
	struct clk_regmap *x;
	char *clk_name, *core_name;
	int ret;

	/* create the mmc rx clock */
	x = devm_kzalloc(dev, sizeof(*x), GFP_KERNEL);
	if (!x)
		return ERR_PTR(-ENOMEM);

	clk_name = kasprintf(GFP_KERNEL, "%s#%s", dev_name(dev), name);
	core_name = kasprintf(GFP_KERNEL, "%s#%s", dev_name(dev), parent_name);

	if (!clk_name || !core_name) {
		x = ERR_PTR(-ENOMEM);
		goto err;
	}
	parent_names[0] = core_name;

	init.name = clk_name;
	init.ops = &clk_regmap_phase_ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = 1;

	x->map = clkc->map;
	x->data = phase_data;
	x->hw.init = &init;

	ret = devm_clk_hw_register(dev, &x->hw);
	if (ret) {
		dev_err(dev, "Core %s clock registration failed\n", name);
		x = ERR_PTR(ret);
	}
err:
	kfree(clk_name);
	kfree(core_name);
	return x;
}

static int mmc_clkc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mmc_clkc_info *clkc;
	struct clk_regmap *mux, *div, *core, *rx, *tx;
	struct clk_hw_onecell_data *onecell_data;

	clkc = devm_kzalloc(dev, sizeof(*clkc), GFP_KERNEL);
	if (!clkc)
		return -ENOMEM;

	clkc->data = (struct mmc_clkc_data *)of_device_get_match_data(dev);
	if (!clkc->data)
		return -EINVAL;

	clkc->dev = dev;
	clkc->map = syscon_node_to_regmap(dev->of_node);

	if (IS_ERR(clkc->map)) {
		dev_err(dev, "could not find mmc clock controller\n");
		return PTR_ERR(clkc->map);
	}

	onecell_data = devm_kzalloc(dev, sizeof(*onecell_data) +
				    sizeof(*onecell_data->hws) * MMC_MAX_CLKS,
				    GFP_KERNEL);
	if (!onecell_data)
		return -ENOMEM;

	mux = mmc_clkc_register_mux(clkc);
	if (IS_ERR(mux))
		return PTR_ERR(mux);

	div = mmc_clkc_register_div(clkc);
	if (IS_ERR(div))
		return PTR_ERR(div);

	core = mmc_clkc_register_phase_clk(clkc, "core", "div",
					   CLK_SET_RATE_PARENT,
					   &mmc_clkc_core_phase);
	if (IS_ERR(core))
		return PTR_ERR(core);

	rx = mmc_clkc_register_phase_clk(clkc, "rx", "core", 0,
					 &clkc->data->rx);
	if (IS_ERR(rx))
		return PTR_ERR(rx);

	tx = mmc_clkc_register_phase_clk(clkc, "tx", "core", 0,
					 &clkc->data->tx);
	if (IS_ERR(tx))
		return PTR_ERR(tx);

	onecell_data->hws[CLKID_MMC_MUX]		= &mux->hw,
	onecell_data->hws[CLKID_MMC_DIV]		= &div->hw,
	onecell_data->hws[CLKID_MMC_PHASE_CORE]		= &core->hw,
	onecell_data->hws[CLKID_MMC_PHASE_RX]		= &rx->hw,
	onecell_data->hws[CLKID_MMC_PHASE_TX]		= &tx->hw,
	onecell_data->num				= MMC_MAX_CLKS;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   onecell_data);
}

static struct platform_driver mmc_clkc_driver = {
	.probe		= mmc_clkc_probe,
	.driver		= {
		.name	= "meson-mmc-clkc",
		.of_match_table = of_match_ptr(mmc_clkc_match_table),
	},
};

module_platform_driver(mmc_clkc_driver);
