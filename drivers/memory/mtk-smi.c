/*
 * Copyright (c) 2014-2015 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <soc/mediatek/smi.h>

#define SMI_LARB_MMU_EN		0xf00
#define F_SMI_MMU_EN(port)	BIT(port)

struct mtk_smi_common {
	struct device	*dev;
	struct clk	*clk_apb, *clk_smi;
};

struct mtk_smi_larb { /* larb: local arbiter */
	struct device	*dev;
	void __iomem	*base;
	struct clk	*clk_apb, *clk_smi;
	struct device	*smi_common_dev;
	u32		mmu;
};

static int
mtk_smi_enable(struct device *dev, struct clk *apb, struct clk *smi)
{
	int ret;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(apb);
	if (ret)
		goto err_put_pm;

	ret = clk_prepare_enable(smi);
	if (ret)
		goto err_disable_apb;

	return 0;

err_disable_apb:
	clk_disable_unprepare(apb);
err_put_pm:
	pm_runtime_put_sync(dev);
	return ret;
}

static void
mtk_smi_disable(struct device *dev, struct clk *apb, struct clk *smi)
{
	clk_disable_unprepare(smi);
	clk_disable_unprepare(apb);
	pm_runtime_put_sync(dev);
}

static int mtk_smi_common_enable(struct mtk_smi_common *common)
{
	return mtk_smi_enable(common->dev, common->clk_apb, common->clk_smi);
}

static void mtk_smi_common_disable(struct mtk_smi_common *common)
{
	mtk_smi_disable(common->dev, common->clk_apb, common->clk_smi);
}

static int mtk_smi_larb_enable(struct mtk_smi_larb *larb)
{
	return mtk_smi_enable(larb->dev, larb->clk_apb, larb->clk_smi);
}

static void mtk_smi_larb_disable(struct mtk_smi_larb *larb)
{
	mtk_smi_disable(larb->dev, larb->clk_apb, larb->clk_smi);
}

int mtk_smi_larb_get(struct device *larbdev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(larbdev);
	struct mtk_smi_common *common = dev_get_drvdata(larb->smi_common_dev);
	int ret;

	ret = mtk_smi_common_enable(common);
	if (ret)
		return ret;

	ret = mtk_smi_larb_enable(larb);
	if (ret)
		goto err_put_smi;

	/* Configure the iommu info */
	writel_relaxed(larb->mmu, larb->base + SMI_LARB_MMU_EN);

	return 0;

err_put_smi:
	mtk_smi_common_disable(common);
	return ret;
}

void mtk_smi_larb_put(struct device *larbdev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(larbdev);
	struct mtk_smi_common *common = dev_get_drvdata(larb->smi_common_dev);

	writel_relaxed(0, larb->base + SMI_LARB_MMU_EN);
	mtk_smi_larb_disable(larb);
	mtk_smi_common_disable(common);
}

void mtk_smi_config_port(struct device *larbdev, unsigned int larbportid,
			 bool enable)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(larbdev);

	dev_dbg(larbdev, "%s iommu port: %d\n",
		enable ? "enable" : "disable", larbportid);

	/*
	 * Only record the iommu info here,
	 * and it will work after its power and clocks is enabled.
	 */
	if (enable)
		larb->mmu |= F_SMI_MMU_EN(larbportid);
	else
		larb->mmu &= ~F_SMI_MMU_EN(larbportid);
}

static int
mtk_smi_larb_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void
mtk_smi_larb_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_smi_larb_component_ops = {
	.bind = mtk_smi_larb_bind,
	.unbind = mtk_smi_larb_unbind,
};

static int mtk_smi_larb_probe(struct platform_device *pdev)
{
	struct mtk_smi_larb *larb;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *smi_node;
	struct platform_device *smi_pdev;

	if (!dev->pm_domain)
		return -EPROBE_DEFER;

	larb = devm_kzalloc(dev, sizeof(*larb), GFP_KERNEL);
	if (!larb)
		return -ENOMEM;
	larb->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	larb->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(larb->base))
		return PTR_ERR(larb->base);

	larb->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(larb->clk_apb))
		return PTR_ERR(larb->clk_apb);

	larb->clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(larb->clk_smi))
		return PTR_ERR(larb->clk_smi);

	smi_node = of_parse_phandle(dev->of_node, "mediatek,smi", 0);
	if (!smi_node)
		return -EINVAL;

	smi_pdev = of_find_device_by_node(smi_node);
	of_node_put(smi_node);
	if (smi_pdev) {
		larb->smi_common_dev = &smi_pdev->dev;
	} else {
		dev_err(dev, "Failed to get the smi_common device\n");
		return -EINVAL;
	}

	pm_runtime_enable(dev);
	dev_set_drvdata(dev, larb);
	return component_add(dev, &mtk_smi_larb_component_ops);
}

static int mtk_smi_larb_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	component_del(&pdev->dev, &mtk_smi_larb_component_ops);
	return 0;
}

static const struct of_device_id mtk_smi_larb_of_ids[] = {
	{ .compatible = "mediatek,mt8173-smi-larb",},
	{}
};

static struct platform_driver mtk_smi_larb_driver = {
	.probe	= mtk_smi_larb_probe,
	.remove = mtk_smi_larb_remove,
	.driver	= {
		.name = "mtk-smi-larb",
		.of_match_table = mtk_smi_larb_of_ids,
	}
};

static int mtk_smi_common_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_smi_common *common;

	if (!dev->pm_domain)
		return -EPROBE_DEFER;

	common = devm_kzalloc(dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;
	common->dev = dev;

	common->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(common->clk_apb))
		return PTR_ERR(common->clk_apb);

	common->clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(common->clk_smi))
		return PTR_ERR(common->clk_smi);

	pm_runtime_enable(dev);
	dev_set_drvdata(dev, common);
	return 0;
}

static int mtk_smi_common_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id mtk_smi_common_of_ids[] = {
	{ .compatible = "mediatek,mt8173-smi-common", },
	{}
};

static struct platform_driver mtk_smi_common_driver = {
	.probe	= mtk_smi_common_probe,
	.remove = mtk_smi_common_remove,
	.driver	= {
		.name = "mtk-smi-common",
		.of_match_table = mtk_smi_common_of_ids,
	}
};

static int __init mtk_smi_init(void)
{
	int ret;

	ret = platform_driver_register(&mtk_smi_common_driver);
	if (ret != 0) {
		pr_err("Failed to register SMI driver\n");
		return ret;
	}

	ret = platform_driver_register(&mtk_smi_larb_driver);
	if (ret != 0) {
		pr_err("Failed to register SMI-LARB driver\n");
		goto err_unreg_smi;
	}
	return ret;

err_unreg_smi:
	platform_driver_unregister(&mtk_smi_common_driver);
	return ret;
}
subsys_initcall(mtk_smi_init);
