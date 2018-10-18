// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Amlogic Meson MMC Sub Clock Controller Driver
 *
 * Copyright (c) 2017 Baylibre SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Yixun Lan <yixun.lan@amlogic.com>
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/amlogic,mmc-clkc.h>

#include "clkc.h"

/* clock ID used by internal driver */
#define CLKID_MMC_MUX			0

#define   SD_EMMC_CLOCK		0
#define   CLK_DELAY_STEP_PS		200
#define   CLK_PHASE_STEP		30
#define   CLK_PHASE_POINT_NUM		(360 / CLK_PHASE_STEP)

#define MUX_CLK_NUM_PARENTS		2
#define MMC_MAX_CLKS			5

struct mmc_clkc_data {
	struct meson_clk_phase_delay_data	tx;
	struct meson_clk_phase_delay_data	rx;
};

static struct clk_regmap_mux_data mmc_clkc_mux_data = {
	.offset		= SD_EMMC_CLOCK,
	.mask		= 0x3,
	.shift		= 6,
	.flags		= CLK_DIVIDER_ROUND_CLOSEST,
};

static struct clk_regmap_div_data mmc_clkc_div_data = {
	.offset		= SD_EMMC_CLOCK,
	.shift		= 0,
	.width		= 6,
	.flags		= CLK_DIVIDER_ROUND_CLOSEST | CLK_DIVIDER_ONE_BASED,
};

static struct meson_clk_phase_data mmc_clkc_core_phase = {
	.ph = {
		.reg_off	= SD_EMMC_CLOCK,
		.shift	= 8,
		.width	= 2,
	}
};

static const struct mmc_clkc_data mmc_clkc_gx_data = {
	.tx = {
		.phase = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 10,
			.width	= 2,
		},
		.delay = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 16,
			.width	= 4,
		},
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
	.rx = {
		.phase = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 12,
			.width	= 2,
		},
		.delay = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 20,
			.width	= 4,
		},
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
};

static const struct mmc_clkc_data mmc_clkc_axg_data = {
	.tx = {
		.phase = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 10,
			.width	= 2,
		},
		.delay = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 16,
			.width	= 6,
		},
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
	.rx = {
		.phase = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 12,
			.width	= 2,
		},
		.delay = {
			.reg_off	= SD_EMMC_CLOCK,
			.shift	= 22,
			.width	= 6,
		},
		.delay_step_ps	= CLK_DELAY_STEP_PS,
	},
};

static const struct of_device_id mmc_clkc_match_table[] = {
	{
		.compatible	= "amlogic,gx-mmc-clkc",
		.data		= &mmc_clkc_gx_data
	},
	{
		.compatible	= "amlogic,axg-mmc-clkc",
		.data		= &mmc_clkc_axg_data
	},
	{}
};
MODULE_DEVICE_TABLE(of, mmc_clkc_match_table);

static struct clk_regmap *
mmc_clkc_register_clk(struct device *dev, struct regmap *map,
		      struct clk_init_data *init,
		      const char *suffix, void *data)
{
	struct clk_regmap *clk;
	char *name;
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	name = kasprintf(GFP_KERNEL, "%s#%s", dev_name(dev), suffix);
	if (!name)
		return ERR_PTR(-ENOMEM);

	init->name = name;

	clk->map = map;
	clk->data = data;
	clk->hw.init = init;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		clk = ERR_PTR(ret);

	kfree(name);
	return clk;
}

static struct clk_regmap *mmc_clkc_register_mux(struct device *dev,
						struct regmap *map)
{
	const char *parent_names[MUX_CLK_NUM_PARENTS];
	struct clk_init_data init;
	struct clk_regmap *mux;
	struct clk *clk;
	int i;

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

	init.ops = &clk_regmap_mux_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = parent_names;
	init.num_parents = MUX_CLK_NUM_PARENTS;

	mux = mmc_clkc_register_clk(dev, map, &init, "mux", &mmc_clkc_mux_data);
	if (IS_ERR(mux))
		dev_err(dev, "Mux clock registration failed\n");

	return mux;
}

static struct clk_regmap *
mmc_clkc_register_clk_with_parent(struct device *dev, struct regmap *map,
				  char *suffix, const char *parent,
				  unsigned long flags,
				  const struct clk_ops *ops, void *data)
{
	struct clk_init_data init;
	struct clk_regmap *clk;

	init.ops = ops;
	init.flags = flags;
	init.parent_names = (const char* const []){ parent, };
	init.num_parents = 1;

	clk = mmc_clkc_register_clk(dev, map, &init, suffix, data);
	if (IS_ERR(clk))
		dev_err(dev, "Core %s clock registration failed\n", suffix);

	return clk;
}

static int mmc_clkc_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *onecell_data;
	struct device *dev = &pdev->dev;
	struct mmc_clkc_data *data;
	struct regmap *map;
	struct clk_regmap *mux, *div, *core, *rx, *tx;

	data = (struct mmc_clkc_data *)of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	map = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(map)) {
		dev_err(dev, "could not find mmc clock controller\n");
		return PTR_ERR(map);
	}

	onecell_data = devm_kzalloc(dev, sizeof(*onecell_data) +
				    sizeof(*onecell_data->hws) * MMC_MAX_CLKS,
				    GFP_KERNEL);
	if (!onecell_data)
		return -ENOMEM;

	mux = mmc_clkc_register_mux(dev, map);
	if (IS_ERR(mux))
		return PTR_ERR(mux);

	div = mmc_clkc_register_clk_with_parent(dev, map, "div",
						clk_hw_get_name(&mux->hw),
						CLK_SET_RATE_PARENT,
						&clk_regmap_divider_with_init_ops,
						&mmc_clkc_div_data);
	if (IS_ERR(div))
		return PTR_ERR(div);

	core = mmc_clkc_register_clk_with_parent(dev, map, "core",
						 clk_hw_get_name(&div->hw),
						 CLK_SET_RATE_PARENT,
						 &meson_clk_phase_ops,
						 &mmc_clkc_core_phase);
	if (IS_ERR(core))
		return PTR_ERR(core);

	rx = mmc_clkc_register_clk_with_parent(dev, map, "rx",
					       clk_hw_get_name(&core->hw),  0,
					       &meson_clk_phase_delay_ops,
					       &data->rx);
	if (IS_ERR(rx))
		return PTR_ERR(rx);

	tx = mmc_clkc_register_clk_with_parent(dev, map, "tx",
					       clk_hw_get_name(&core->hw),  0,
					       &meson_clk_phase_delay_ops,
					       &data->tx);
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
