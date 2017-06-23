/*
 * Copyright (c) 2017, Linaro Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <linux/regmap.h>

#include "../clk/qcom/clk-regmap.h"
#include "../clk/qcom/clk-regmap-mux-div.h"

enum {
	P_GPLL0,
	P_A53PLL,
};

static const struct parent_map gpll0_a53cc_map[] = {
	{ P_GPLL0, 4 },
	{ P_A53PLL, 5 },
};

static const char * const gpll0_a53cc[] = {
	"gpll0_vote",
	"a53pll",
};

static const struct regmap_config a53cc_regmap_config = {
	.reg_bits		= 32,
	.reg_stride		= 4,
	.val_bits		= 32,
	.max_register		= 0x1000,
	.fast_io		= true,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};

#define QCOM_APCS_IPC_BITS	32

struct qcom_apcs_ipc {
	struct mbox_controller mbox;
	struct mbox_chan mbox_chans[QCOM_APCS_IPC_BITS];

	void __iomem *reg;
	unsigned long offset;
};

static int qcom_apcs_ipc_send_data(struct mbox_chan *chan, void *data)
{
	struct qcom_apcs_ipc *apcs = container_of(chan->mbox,
						  struct qcom_apcs_ipc, mbox);
	unsigned long idx = (unsigned long)chan->con_priv;

	writel(BIT(idx), apcs->reg);

	return 0;
}

static const struct mbox_chan_ops qcom_apcs_ipc_ops = {
	.send_data = qcom_apcs_ipc_send_data,
};

/*
 * We use the notifier function for switching to a temporary safe configuration
 * (mux and divider), while the A53 PLL is reconfigured.
 */
static int a53cc_notifier_cb(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	int ret = 0;
	struct clk_regmap_mux_div *md = container_of(nb,
						     struct clk_regmap_mux_div,
						     clk_nb);
	if (event == PRE_RATE_CHANGE)
		/* set the mux and divider to safe frequency (400mhz) */
		ret = __mux_div_set_src_div(md, 4, 3);

	return notifier_from_errno(ret);
}

static int msm8916_register_clk(struct device *dev, void __iomem *base)
{
	struct clk_regmap_mux_div *a53cc;
	struct clk *pclk;
	struct regmap *regmap;
	struct clk_init_data init = { };
	int ret;

	a53cc = devm_kzalloc(dev, sizeof(*a53cc), GFP_KERNEL);
	if (!a53cc)
		return -ENOMEM;

	a53cc->reg_offset = 0x50;
	a53cc->hid_width = 5;
	a53cc->hid_shift = 0;
	a53cc->src_width = 3;
	a53cc->src_shift = 8;
	a53cc->parent_map = gpll0_a53cc_map;

	init.name = "a53mux";
	init.parent_names = gpll0_a53cc;
	init.num_parents = 2;
	init.ops = &clk_regmap_mux_div_ops;
	init.flags = CLK_SET_RATE_PARENT;
	a53cc->clkr.hw.init = &init;

	pclk = __clk_lookup(gpll0_a53cc[1]);
	if (!pclk)
		return -EPROBE_DEFER;

	a53cc->clk_nb.notifier_call = a53cc_notifier_cb;
	ret = clk_notifier_register(pclk, &a53cc->clk_nb);
	if (ret) {
		dev_err(dev, "failed to register clock notifier: %d\n", ret);
		return ret;
	}

	regmap = devm_regmap_init_mmio(dev, base, &a53cc_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "failed to init regmap mmio: %d\n", ret);
		goto err;
	}

	a53cc->clkr.regmap = regmap;

	ret = devm_clk_register_regmap(dev, &a53cc->clkr);
	if (ret) {
		dev_err(dev, "failed to register regmap clock: %d\n", ret);
		goto err;
	}

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_simple_get,
				     &a53cc->clkr.hw);
	if (ret) {
		dev_err(dev, "failed to add clock provider: %d\n", ret);
		goto err;
	}

	return 0;

err:
	clk_notifier_unregister(pclk, &a53cc->clk_nb);
	return ret;
}

static int qcom_apcs_ipc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct qcom_apcs_ipc *apcs;
	struct resource *res;
	unsigned long offset;
	void __iomem *base;
	unsigned long i;
	int ret;

	apcs = devm_kzalloc(&pdev->dev, sizeof(*apcs), GFP_KERNEL);
	if (!apcs)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (of_device_is_compatible(np, "qcom,msm8916-apcs-kpss-global")) {
		/* register the APCS mux and divider clock */
		ret = msm8916_register_clk(&pdev->dev, base);
		if (ret)
			return ret;
	}

	offset = (unsigned long)of_device_get_match_data(&pdev->dev);

	apcs->reg = base + offset;

	/* Initialize channel identifiers */
	for (i = 0; i < ARRAY_SIZE(apcs->mbox_chans); i++)
		apcs->mbox_chans[i].con_priv = (void *)i;

	apcs->mbox.dev = &pdev->dev;
	apcs->mbox.ops = &qcom_apcs_ipc_ops;
	apcs->mbox.chans = apcs->mbox_chans;
	apcs->mbox.num_chans = ARRAY_SIZE(apcs->mbox_chans);

	ret = mbox_controller_register(&apcs->mbox);
	if (ret) {
		dev_err(&pdev->dev, "failed to register APCS IPC controller\n");
		return ret;
	}

	platform_set_drvdata(pdev, apcs);

	return 0;
}

static int qcom_apcs_ipc_remove(struct platform_device *pdev)
{
	struct qcom_apcs_ipc *apcs = platform_get_drvdata(pdev);

	mbox_controller_unregister(&apcs->mbox);

	return 0;
}

/* .data is the offset of the ipc register within the global block */
static const struct of_device_id qcom_apcs_ipc_of_match[] = {
	{ .compatible = "qcom,msm8916-apcs-kpss-global", .data = (void *)8 },
	{ .compatible = "qcom,msm8996-apcs-hmss-global", .data = (void *)16 },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_apcs_ipc_of_match);

static struct platform_driver qcom_apcs_ipc_driver = {
	.probe = qcom_apcs_ipc_probe,
	.remove = qcom_apcs_ipc_remove,
	.driver = {
		.name = "qcom_apcs_ipc",
		.of_match_table = qcom_apcs_ipc_of_match,
	},
};

static int __init qcom_apcs_ipc_init(void)
{
	return platform_driver_register(&qcom_apcs_ipc_driver);
}
postcore_initcall(qcom_apcs_ipc_init);

static void __exit qcom_apcs_ipc_exit(void)
{
	platform_driver_unregister(&qcom_apcs_ipc_driver);
}
module_exit(qcom_apcs_ipc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm APCS IPC driver");
