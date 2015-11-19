/*
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <soc/rockchip/rk3399-dmc-clk.h>

#define to_rk3399_dmcclk(obj)	container_of(obj, struct rk3399_dmcclk, hw)

/* CRU_CLKSEL6_CON*/
#define CRU_CLKSEL6_CON		0x118
#define CLK_DDRC_PLL_SEL_SHIFT	0x4
#define CLK_DDRC_PLL_SEL_MASK	0x3
#define CLK_DDRC_DIV_CON_SHIFT	0
#define CLK_DDRC_DIV_CON_MASK	0x07

static unsigned long rk3399_dmcclk_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct rk3399_dmcclk *dmc = to_rk3399_dmcclk(&hw);
	u32 val;

	/*
	 * Get parent rate since it changed in this clks set_rate op. The parent
	 * rate passed into this function is cached before set_rate is called in
	 * the common clk code, so we have to get it here.
	 */
	parent_rate = clk_get_rate(clk_get_parent(hw->clk));

	val = readl(dmc->cru + CRU_CLKSEL6_CON);
	val = (val >> CLK_DDRC_DIV_CON_SHIFT) & CLK_DDRC_DIV_CON_MASK;

	return parent_rate / (val + 1);
}

/*
 * TODO: set ddr frequcney in dcf which run in ATF
 */
static int rk3399_dmcclk_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	return 0;
}

static u8 rk3399_dmcclk_get_parent(struct clk_hw *hw)
{
	struct rk3399_dmcclk *dmc = to_rk3399_dmcclk(&hw);
	u32 val;

	val = readl(dmc->cru + CRU_CLKSEL6_CON);

	return (val >> CLK_DDRC_PLL_SEL_SHIFT) &
		CLK_DDRC_PLL_SEL_MASK;
}

static const struct clk_ops rk3399_dmcclk_ops = {
	.recalc_rate = rk3399_dmcclk_recalc_rate,
	.set_rate = rk3399_dmcclk_set_rate,
	.get_parent = rk3399_dmcclk_get_parent,
};

static const char *parent_clk_names[] = {
	"pll_dpll",
	"pll_gpll",
	"pll_alpll",
	"pll_abpll",
};

static int rk3399_register_dmcclk(struct rk3399_dmcclk *dmc)
{
	struct clk_init_data init;
	struct clk *clk;

	init.name = "dmc_clk";
	init.parent_names = parent_clk_names;
	init.num_parents = ARRAY_SIZE(parent_clk_names);
	init.ops = &rk3399_dmcclk_ops;
	init.flags = 0;
	dmc->hw->init = &init;

	clk = devm_clk_register(dmc->dev, dmc->hw);
	if (IS_ERR(clk)) {
		dev_err(dmc->dev, "could not register cpuclk dmc_clk\n");
		return PTR_ERR(clk);
	}
	clk_register_clkdev(clk, "dmc_clk", NULL);
	of_clk_add_provider(dmc->dev->of_node, of_clk_src_simple_get, clk);

	return 0;
}

static int rk3399_dmcclk_probe(struct platform_device *pdev)
{
	struct rk3399_dmcclk *dmc;
	struct resource *res;
	struct device_node *node;
	int ret;

	dmc = devm_kzalloc(&pdev->dev, sizeof(*dmc), GFP_KERNEL);
	if (!dmc)
		return -ENOMEM;

	dmc->hw = devm_kzalloc(&pdev->dev, sizeof(*dmc->hw), GFP_KERNEL);
	if (!dmc->hw)
		return -ENOMEM;

	dmc->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dmc->ctrl_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dmc->ctrl_regs))
		return PTR_ERR(dmc->ctrl_regs);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	dmc->dfi_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dmc->dfi_regs))
		return PTR_ERR(dmc->dfi_regs);

	node = of_parse_phandle(dmc->dev->of_node, "rockchip,cru", 0);
	if (!node)
		return -ENODEV;

	ret = of_address_to_resource(node, 0, res);
	if (ret)
		return ret;

	dmc->cru = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dmc->cru))
		return PTR_ERR(dmc->cru);

	/* register dpllddr clock */
	ret = rk3399_register_dmcclk(dmc);
	if (ret) {
		dev_err(dmc->dev, "failed to register clk dmc_clk %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, dmc);
	platform_device_register_data(dmc->dev, "rk3399-dmc-freq",
				      PLATFORM_DEVID_AUTO, NULL, 0);

	return 0;
}

static const struct of_device_id rk3399_dmcclk_of_match[] = {
	{ .compatible = "rockchip,rk3399-dmc", },
	{ },
};
MODULE_DEVICE_TABLE(of, rk3399_dmcclk_of_match);


static struct platform_driver rk3399_dmcclk_driver = {
	.probe = rk3399_dmcclk_probe,
	.driver = {
		.name = "rk3399-dmc",
		.of_match_table = rk3399_dmcclk_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init rk3399_dmcclk_modinit(void)
{
	int ret;

	ret = platform_driver_register(&rk3399_dmcclk_driver);
	if (ret < 0)
		pr_err("Failed to register platform driver %s\n",
				rk3399_dmcclk_driver.driver.name);

	return ret;
}

module_init(rk3399_dmcclk_modinit);

MODULE_DESCRIPTION("rockchip rk3399 DMC CLK driver");
MODULE_LICENSE("GPL v2");

