/*
 * Oleksij Rempel <o.rempel@pengutronix.de>
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#define IMX7D_ENABLE_M4			BIT(3)
#define IMX7D_SW_M4P_RST		BIT(2)
#define IMX7D_SW_M4C_RST		BIT(1)
#define IMX7D_SW_M4C_NON_SCLR_RST	BIT(0)

#define IMX7D_M4_RST_MASK		0xf


#define IMX7D_RPROC_MEM_MAX		2
enum {
	IMX7D_RPROC_IMEM,
	IMX7D_RPROC_DMEM,
};

static const char *mem_names[IMX7D_RPROC_MEM_MAX] = {
	[IMX7D_RPROC_IMEM]	= "imem",
	[IMX7D_RPROC_DMEM]	= "dmem",
};

/**
 * struct imx_rproc_mem - slim internal memory structure
 * @cpu_addr: MPU virtual address of the memory region
 * @bus_addr: Bus address used to access the memory region
 * @size: Size of the memory region
 */
struct imx_rproc_mem {
	void __iomem *cpu_addr;
	phys_addr_t bus_addr;
	size_t size;
};

struct imx_rproc_dcfg {
	int offset;
};

struct imx_rproc {
	struct device			*dev;
	struct regmap			*regmap;
	struct rproc			*rproc;
	const struct imx_rproc_dcfg	*dcfg;
	struct imx_rproc_mem mem[IMX7D_RPROC_MEM_MAX];
	struct clk		*clk;
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx7d = {
	.offset = 0xc,
};

static int imx_rproc_start(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	int ret;

	ret = clk_enable(priv->clk);
	if (ret) {
		dev_err(&rproc->dev, "Failed to enable clock\n");
		return ret;
	}

	ret = regmap_update_bits(priv->regmap, dcfg->offset,
				 IMX7D_M4_RST_MASK,
				 IMX7D_SW_M4C_RST | IMX7D_SW_M4P_RST | IMX7D_ENABLE_M4);
	if (ret) {
		dev_err(dev, "Filed to enable M4!\n");
		clk_disable(priv->clk);
	}

	return ret;
}

static int imx_rproc_stop(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	int ret;

	ret = regmap_update_bits(priv->regmap, dcfg->offset,
				 IMX7D_M4_RST_MASK,
				 IMX7D_SW_M4C_NON_SCLR_RST);
	if (ret)
		dev_err(dev, "Filed to stop M4!\n");

	clk_disable(priv->clk);

	return ret;
}

static void *imx_rproc_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct imx_rproc *priv = rproc->priv;
	void *va = NULL;
	int i;

	for (i = 0; i < IMX7D_RPROC_MEM_MAX; i++) {
		if (da != priv->mem[i].bus_addr)
			continue;

		if (len <= priv->mem[i].size) {
			/* __force to make sparse happy with type conversion */
			va = (__force void *)priv->mem[i].cpu_addr;
			break;
		}
	}

	dev_dbg(&rproc->dev, "da = 0x%llx len = 0x%x va = 0x%p\n", da, len, va);

	return va;
}

static const struct rproc_ops imx_rproc_ops = {
	.start		= imx_rproc_start,
	.stop		= imx_rproc_stop,
	.da_to_va       = imx_rproc_da_to_va,
};

static const struct of_device_id imx_rproc_of_match[] = {
	{ .compatible = "fsl,imx7d-rproc", .data = &imx_rproc_cfg_imx7d },
	{},
};
MODULE_DEVICE_TABLE(of, imx_rproc_of_match);

static int imx_rproc_addr_init(struct imx_rproc *priv,
			       struct platform_device *pdev)
{
	int i, err;
	struct resource *res;

	/* get imem and dmem */
	for (i = 0; i < ARRAY_SIZE(mem_names); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						mem_names[i]);
		if (!res)
			continue;

		priv->mem[i].cpu_addr = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->mem[i].cpu_addr)) {
			dev_err(&pdev->dev, "devm_ioremap_resource failed\n");
			err = PTR_ERR(priv->mem[i].cpu_addr);
			return err;
		}
		priv->mem[i].bus_addr = res->start & 0xffff;
		priv->mem[i].size = resource_size(res);
	}

	return 0;
}

static int imx_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct imx_rproc *priv;
	struct rproc *rproc;
	struct regmap_config config = { .name = "imx_rproc" };
	const struct imx_rproc_dcfg *dcfg;
	struct regmap *regmap;
	int ret;

	regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(regmap)) {
		dev_err(dev, "failed to find syscon\n");
		return PTR_ERR(regmap);
	}
        regmap_attach_dev(dev, regmap, &config);

	/* set some other name then imx */
	rproc = rproc_alloc(dev, "imx_rproc", &imx_rproc_ops, NULL, sizeof(*priv));
	if (!rproc) {
		ret = -ENOMEM;
		goto err;
	}

	dcfg = of_device_get_match_data(dev);
	if (!dcfg)
		return -EINVAL;

	priv = rproc->priv;
	priv->rproc = rproc;
	priv->regmap = regmap;
	priv->dcfg = dcfg;

	dev_set_drvdata(dev, rproc);

	ret = imx_rproc_addr_init(priv, pdev);
	if (ret) {
		dev_err(dev, "filed on imx_rproc_addr_init\n");
		goto err_put_rproc;
	}

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "Failed to get clock\n");
		return PTR_ERR(priv->clk);
	}

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed\n");
		goto err_put_rproc;
	}

	ret = clk_prepare(priv->clk);
	if (ret)
		dev_err(dev, "failed to get clock\n");

	return ret;

err_put_rproc:
	rproc_free(rproc);
err:
	return ret;
}

static int imx_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);
	rproc_free(rproc);

	return 0;
}

static struct platform_driver imx_rproc_driver = {
	.probe = imx_rproc_probe,
	.remove = imx_rproc_remove,
	.driver = {
		.name = "imx_rproc",
		.of_match_table = imx_rproc_of_match,
	},
};

module_platform_driver(imx_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("IMX6/7 remote processor control driver");
MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
