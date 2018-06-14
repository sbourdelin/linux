// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,lpass-sdm845.h>

#include "clk-regmap.h"
#include "clk-branch.h"
#include "common.h"

static struct clk_branch gcc_lpass_q6_axi_clk = {
	.halt_reg = 0x0,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_lpass_q6_axi_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

struct clk_branch gcc_lpass_sway_clk = {
	.halt_reg = 0x8,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_lpass_sway_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_audio_wrapper_aon_clk = {
	.halt_reg = 0x098,
	.halt_check = BRANCH_VOTED,
	.clkr = {
		.enable_reg = 0x098,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_audio_wrapper_aon_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_q6ss_ahbm_aon_clk = {
	.halt_reg = 0x12000,
	.halt_check = BRANCH_VOTED,
	.clkr = {
		.enable_reg = 0x12000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_q6ss_ahbm_aon_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_q6ss_ahbs_aon_clk = {
	.halt_reg = 0x1f000,
	.halt_check = BRANCH_VOTED,
	.clkr = {
		.enable_reg = 0x1f000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_q6ss_ahbs_aon_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_qdsp6ss_xo_clk = {
	.halt_reg = 0x18,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x18,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_qdsp6ss_xo_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_qdsp6ss_sleep_clk = {
	.halt_reg = 0x1c,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x1c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_qdsp6ss_sleep_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch lpass_qdsp6ss_core_clk = {
	.halt_reg = 0x0,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "lpass_qdsp6ss_core_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct regmap_config lpass_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.fast_io	= true,
};

static struct clk_regmap *lpass_gcc_sdm845_clocks[] = {
	[GCC_LPASS_Q6_AXI_CLK] = &gcc_lpass_q6_axi_clk.clkr,
	[GCC_LPASS_SWAY_CLK] = &gcc_lpass_sway_clk.clkr,
};

static const struct qcom_cc_desc lpass_gcc_sdm845_desc = {
	.config = &lpass_regmap_config,
	.clks = lpass_gcc_sdm845_clocks,
	.num_clks = ARRAY_SIZE(lpass_gcc_sdm845_clocks),
};

static const struct of_device_id lpass_gcc_sdm845_match_table[] = {
	{ .compatible = "qcom,sdm845-lpass-gcc" },
	{ }
};

static struct clk_regmap *lpass_cc_sdm845_clocks[] = {
	[LPASS_AUDIO_WRAPPER_AON_CLK] = &lpass_audio_wrapper_aon_clk.clkr,
	[LPASS_Q6SS_AHBM_AON_CLK] = &lpass_q6ss_ahbm_aon_clk.clkr,
	[LPASS_Q6SS_AHBS_AON_CLK] = &lpass_q6ss_ahbs_aon_clk.clkr,
};

static const struct qcom_cc_desc lpass_cc_sdm845_desc = {
	.config = &lpass_regmap_config,
	.clks = lpass_cc_sdm845_clocks,
	.num_clks = ARRAY_SIZE(lpass_cc_sdm845_clocks),
};

static const struct of_device_id lpasscc_sdm845_match_table[] = {
	{ .compatible = "qcom,sdm845-lpass-cc" },
	{ }
};

static struct clk_regmap *lpass_qdsp6ss_sdm845_clocks[] = {
	[LPASS_QDSP6SS_XO_CLK] = &lpass_qdsp6ss_xo_clk.clkr,
	[LPASS_QDSP6SS_SLEEP_CLK] = &lpass_qdsp6ss_sleep_clk.clkr,
	[LPASS_QDSP6SS_CORE_CLK] = &lpass_qdsp6ss_core_clk.clkr,
};

static const struct qcom_cc_desc lpass_qdsp6ss_sdm845_desc = {
	.config = &lpass_regmap_config,
	.clks = lpass_qdsp6ss_sdm845_clocks,
	.num_clks = ARRAY_SIZE(lpass_qdsp6ss_sdm845_clocks),
};

static const struct of_device_id lpass_qdsp6_sdm845_match_table[] = {
	{ .compatible = "qcom,sdm845-lpass-qdsp6ss" },
	{ }
};

static int lpass_clocks_sdm845_probe(struct platform_device *pdev,
				     struct device_node *np,
				     const struct qcom_cc_desc *desc)
{
	struct regmap *regmap;
	struct resource res;
	void __iomem *base;

	if (of_address_to_resource(np, 0, &res))
		return -ENOMEM;

	base = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
	if (IS_ERR(base))
		return -ENOMEM;

	regmap = devm_regmap_init_mmio(&pdev->dev, base, desc->config);
	if (!regmap)
		return PTR_ERR(regmap);

	return qcom_cc_really_probe(pdev, desc, regmap);
}

/* LPASS CC clock controller */
static const struct of_device_id lpass_cc_sdm845_match_table[] = {
	{ .compatible = "qcom,sdm845-lpasscc" },
	{ }
};
MODULE_DEVICE_TABLE(of, lpass_cc_sdm845_match_table);

static int lpass_cc_sdm845_probe(struct platform_device *pdev)
{
	struct device_node *cp;
	const struct qcom_cc_desc *desc;
	int ret;

	for_each_available_child_of_node(pdev->dev.of_node, cp) {
		if (of_match_node(lpass_gcc_sdm845_match_table, cp)) {
			lpass_regmap_config.name = "lpass_gcc";
			desc = &lpass_gcc_sdm845_desc;
		} else if (of_match_node(lpasscc_sdm845_match_table, cp)) {
			lpass_regmap_config.name = "lpass_cc";
			desc = &lpass_cc_sdm845_desc;
		} else if (of_match_node(lpass_qdsp6_sdm845_match_table, cp)) {
			lpass_regmap_config.name = "lpass_qdsp6ss";
			desc = &lpass_qdsp6ss_sdm845_desc;
		} else {
			dev_err(&pdev->dev, "LPASS child node not defined\n");
			return -EINVAL;
		}

		ret = lpass_clocks_sdm845_probe(pdev, cp, desc);
		if (ret)
			return ret;
	}

	return 0;
}

static struct platform_driver lpass_cc_sdm845_driver = {
	.probe		= lpass_cc_sdm845_probe,
	.driver		= {
		.name	= "sdm845-lpasscc",
		.of_match_table = lpass_cc_sdm845_match_table,
	},
};

static int __init lpass_cc_sdm845_init(void)
{
	return platform_driver_register(&lpass_cc_sdm845_driver);
}
subsys_initcall(lpass_cc_sdm845_init);

MODULE_LICENSE("GPL v2");
