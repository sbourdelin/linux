// SPDX-License-Identifier: GPL-2.0
/**
 * dwc3-uniphier.c - Socionext UniPhier DWC3 specific glue layer
 *
 * Copyright 2015-2018 Socionext Inc.
 *
 * Author:
 *	Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 * Contributors:
 *      Motoya Tanigawa <tanigawa.motoya@socionext.com>
 *      Masami Hiramatsu <masami.hiramatsu@linaro.org>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define RESET_CTL		0x000
#define LINK_RESET		BIT(15)

#define VBUS_CONTROL(n)		(0x100 + 0x10 * (n))
#define DRVVBUS_REG		BIT(4)
#define DRVVBUS_REG_EN		BIT(3)

#define U2PHY_CFG0(n)		(0x200 + 0x10 * (n))
#define U2PHY_CFG0_HS_I_MASK	GENMASK(31, 28)
#define U2PHY_CFG0_HSDISC_MASK	GENMASK(27, 26)
#define U2PHY_CFG0_SWING_MASK	GENMASK(17, 16)
#define U2PHY_CFG0_SEL_T_MASK	GENMASK(15, 12)
#define U2PHY_CFG0_RTERM_MASK	GENMASK(7, 6)
#define U2PHY_CFG0_TRIMMASK	(U2PHY_CFG0_HS_I_MASK \
				 | U2PHY_CFG0_SEL_T_MASK \
				 | U2PHY_CFG0_RTERM_MASK)

#define U2PHY_CFG1(n)		(0x204 + 0x10 * (n))
#define U2PHY_CFG1_DAT_EN	BIT(29)
#define U2PHY_CFG1_ADR_EN	BIT(28)
#define U2PHY_CFG1_ADR_MASK	GENMASK(27, 16)
#define U2PHY_CFG1_DAT_MASK	GENMASK(23, 16)

#define U3PHY_TESTI(n)		(0x300 + 0x10 * (n))
#define U3PHY_TESTO(n)		(0x304 + 0x10 * (n))
#define TESTI_DAT_MASK		GENMASK(13, 6)
#define TESTI_ADR_MASK		GENMASK(5, 1)
#define TESTI_WR_EN		BIT(0)

#define HOST_CONFIG0		0x400
#define NUM_U3_MASK		GENMASK(13, 11)
#define NUM_U2_MASK		GENMASK(10, 8)

#define PHY_MAX_PARAMS	32

struct dwc3u_phy_param {
	u32 addr;
	u32 mask;
	u32 val;
};

struct dwc3u_trim_param {
	u32 rterm;
	u32 sel_t;
	u32 hs_i;
};

#define trim_param_is_valid(p)	((p)->rterm || (p)->sel_t || (p)->hs_i)

struct dwc3u_priv {
	struct device		*dev;
	void __iomem		*base;
	struct clk		**clks;
	int			nclks;
	struct reset_control	*rst;
	int			nvbus;
	const struct dwc3u_soc_data	*data;
};

struct dwc3u_soc_data {
	int ss_nparams;
	struct dwc3u_phy_param ss_param[PHY_MAX_PARAMS];
	int hs_nparams;
	struct dwc3u_phy_param hs_param[PHY_MAX_PARAMS];
	u32 hs_config0;
	u32 hs_config1;
	void (*trim_func)(struct dwc3u_priv *priv, u32 *pconfig,
			  struct dwc3u_trim_param *trim);
};

static inline u32 dwc3u_read(struct dwc3u_priv *priv, off_t offset)
{
	return readl(priv->base + offset);
}

static inline void dwc3u_write(struct dwc3u_priv *priv,
			       off_t offset, u32 val)
{
	writel(val, priv->base + offset);
}

static inline void dwc3u_maskwrite(struct dwc3u_priv *priv,
				   off_t offset, u32 mask, u32 val)
{
	u32 tmp;

	tmp = dwc3u_read(priv, offset);
	dwc3u_write(priv, offset, (tmp & ~mask) | (val & mask));
}

static int dwc3u_get_hsport_num(struct dwc3u_priv *priv)
{
	return FIELD_GET(NUM_U2_MASK, dwc3u_read(priv, HOST_CONFIG0));
}

static int dwc3u_get_ssport_num(struct dwc3u_priv *priv)
{
	return FIELD_GET(NUM_U3_MASK, dwc3u_read(priv, HOST_CONFIG0));
}

static int dwc3u_get_nvparam(struct dwc3u_priv *priv,
			     const char *basename, int index, u8 *dst,
			     int maxlen)
{
	struct nvmem_cell *cell;
	char name[16];
	size_t len;
	u8 *buf;

	snprintf(name, sizeof(name) - 1, "%s%d", basename, index);
	memset(dst, 0, maxlen);

	cell = nvmem_cell_get(priv->dev, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	len = min_t(u32, len, maxlen);
	memcpy(dst, buf, len);
	kfree(buf);

	return 0;
}

static int dwc3u_get_nvparam_u32(struct dwc3u_priv *priv,
				 const char *basename, int index, u32 *p_val)
{
	return dwc3u_get_nvparam(priv, basename, index, (u8 *)p_val,
				 sizeof(u32));
}

static void dwc3u_ssphy_testio_write(struct dwc3u_priv *priv, int port,
				     u32 data)
{
	/* need to read TESTO twice after accessing TESTI */
	dwc3u_write(priv, U3PHY_TESTI(port), data);
	dwc3u_read(priv, U3PHY_TESTO(port));
	dwc3u_read(priv, U3PHY_TESTO(port));
}

static void dwc3u_ssphy_set_param(struct dwc3u_priv *priv, int port,
				  const struct dwc3u_phy_param *p)
{
	u32 val, val_prev;

	/* read previous data */
	dwc3u_ssphy_testio_write(priv, port,
				 FIELD_PREP(TESTI_DAT_MASK, 1) |
				 FIELD_PREP(TESTI_ADR_MASK, p->addr));
	val_prev = dwc3u_read(priv, U3PHY_TESTO(port));

	/* update value */
	val = FIELD_PREP(TESTI_DAT_MASK,
			 (val_prev & ~p->mask) | (p->val & p->mask)) |
		FIELD_PREP(TESTI_ADR_MASK, p->addr);

	dwc3u_ssphy_testio_write(priv, port, val);
	dwc3u_ssphy_testio_write(priv, port, val | TESTI_WR_EN);
	dwc3u_ssphy_testio_write(priv, port, val);

	/* read current data as dummy */
	dwc3u_ssphy_testio_write(priv, port,
				 FIELD_PREP(TESTI_DAT_MASK, 1) |
				 FIELD_PREP(TESTI_ADR_MASK, p->addr));
	dwc3u_read(priv, U3PHY_TESTO(port));
}

static void dwc3u_ssphy_init(struct dwc3u_priv *priv)
{
	int nparams = min_t(u32, priv->data->ss_nparams, PHY_MAX_PARAMS);
	int nports = dwc3u_get_ssport_num(priv);
	int i, j;

	for (i = 0; i < nports; i++)
		for (j = 0; j < nparams; j++)
			dwc3u_ssphy_set_param(priv, i,
					      &priv->data->ss_param[j]);
}

static void dwc3u_hsphy_trim_ld20(struct dwc3u_priv *priv, u32 *pconfig,
				  struct dwc3u_trim_param *ptrim)
{
	*pconfig = (*pconfig & ~U2PHY_CFG0_TRIMMASK) |
		FIELD_PREP(U2PHY_CFG0_RTERM_MASK, ptrim->rterm) |
		FIELD_PREP(U2PHY_CFG0_SEL_T_MASK, ptrim->sel_t) |
		FIELD_PREP(U2PHY_CFG0_HS_I_MASK,  ptrim->hs_i);
}

static int dwc3u_hsphy_get_nvparams(struct dwc3u_priv *priv, int port,
				    struct dwc3u_trim_param *ptrim)
{
	int ret;

	ret = dwc3u_get_nvparam_u32(priv, "rterm", port, &ptrim->rterm);
	if (ret)
		return ret;

	ret = dwc3u_get_nvparam_u32(priv, "sel_t", port, &ptrim->sel_t);
	if (ret)
		return ret;

	return dwc3u_get_nvparam_u32(priv, "hs_i", port, &ptrim->hs_i);
}

static int dwc3u_hsphy_update_config(struct dwc3u_priv *priv, int port,
				     u32 *pconfig)
{
	struct dwc3u_trim_param trim;
	int ret, trimmed = 0;

	if (priv->data->trim_func) {
		ret = dwc3u_hsphy_get_nvparams(priv, port, &trim);
		if (ret == -EPROBE_DEFER)
			return ret;

		/*
		 * call trim_func only when trimming parameters that aren't
		 * all-zero can be acquired. All-zero parameters mean nothing
		 * has been written to nvmem.
		 */
		if (!ret && trim_param_is_valid(&trim)) {
			priv->data->trim_func(priv, pconfig, &trim);
			trimmed = 1;
		} else {
			dev_dbg(priv->dev,
				"can't get parameter for port%d from nvmem\n",
				port);
		}
	}

	/* use default parameters without trimming values */
	if (!trimmed)
		*pconfig = (*pconfig & ~U2PHY_CFG0_HSDISC_MASK) |
			FIELD_PREP(U2PHY_CFG0_HSDISC_MASK, 3);

	return 0;
}

static void dwc3u_hsphy_set_config(struct dwc3u_priv *priv, int port,
				   u32 config0, u32 config1)
{
	dwc3u_write(priv, U2PHY_CFG0(port), config0);
	dwc3u_write(priv, U2PHY_CFG1(port), config1);

	dwc3u_maskwrite(priv, U2PHY_CFG0(port),
			U2PHY_CFG0_SWING_MASK,
			FIELD_PREP(U2PHY_CFG0_SWING_MASK, 2));
}

static void dwc3u_hsphy_set_param(struct dwc3u_priv *priv, int port,
				  const struct dwc3u_phy_param *p)
{
	dwc3u_maskwrite(priv, U2PHY_CFG1(port),
			U2PHY_CFG1_ADR_EN | U2PHY_CFG1_ADR_MASK,
			U2PHY_CFG1_ADR_EN |
			FIELD_PREP(U2PHY_CFG1_ADR_MASK, p->addr));
	dwc3u_maskwrite(priv, U2PHY_CFG1(port),
			U2PHY_CFG1_ADR_EN, 0);

	dwc3u_maskwrite(priv, U2PHY_CFG1(port),
			U2PHY_CFG1_DAT_EN |
			FIELD_PREP(U2PHY_CFG1_DAT_MASK, p->mask),
			U2PHY_CFG1_DAT_EN |
			FIELD_PREP(U2PHY_CFG1_DAT_MASK, p->val));
	dwc3u_maskwrite(priv, U2PHY_CFG1(port),
			U2PHY_CFG1_DAT_EN, 0);
}

static int dwc3u_hsphy_init(struct dwc3u_priv *priv)
{
	int nparams = min(priv->data->hs_nparams, PHY_MAX_PARAMS);
	int nports = dwc3u_get_hsport_num(priv);
	u32 config0, config1;
	int i, ret, port;

	for (port = 0; port < nports; port++) {
		config0 = priv->data->hs_config0;
		config1 = priv->data->hs_config1;

		ret = dwc3u_hsphy_update_config(priv, port, &config0);
		if (ret)
			return ret;

		dwc3u_hsphy_set_config(priv, port, config0, config1);

		for (i = 0; i < nparams; i++)
			dwc3u_hsphy_set_param(priv, port,
					      &priv->data->hs_param[i]);
	}

	return 0;
}

static int dwc3u_phy_init(struct dwc3u_priv *priv)
{
	dwc3u_ssphy_init(priv);

	return dwc3u_hsphy_init(priv);
}

static void dwc3u_vbus_enable(struct dwc3u_priv *priv)
{
	int i;

	for (i = 0; i < priv->nvbus; i++) {
		dwc3u_maskwrite(priv, VBUS_CONTROL(i),
				DRVVBUS_REG_EN | DRVVBUS_REG,
				DRVVBUS_REG_EN | DRVVBUS_REG);
	}
}

static void dwc3u_vbus_disable(struct dwc3u_priv *priv)
{
	int i;

	for (i = 0; i < priv->nvbus; i++) {
		dwc3u_maskwrite(priv, VBUS_CONTROL(i),
				DRVVBUS_REG_EN | DRVVBUS_REG,
				DRVVBUS_REG_EN | 0);
	}
}

static void dwc3u_reset_init(struct dwc3u_priv *priv)
{
	dwc3u_maskwrite(priv, RESET_CTL, LINK_RESET, 0);
	usleep_range(1000, 2000);
	dwc3u_maskwrite(priv, RESET_CTL, LINK_RESET, LINK_RESET);
}

static void dwc3u_reset_clear(struct dwc3u_priv *priv)
{
	dwc3u_maskwrite(priv, RESET_CTL, LINK_RESET, 0);
}

static int dwc3u_init(struct dwc3u_priv *priv)
{
	int nr_hsports, nr_ssports;
	int ret;

	nr_hsports = dwc3u_get_hsport_num(priv);
	nr_ssports = dwc3u_get_ssport_num(priv);
	priv->nvbus = max(nr_hsports, nr_ssports);

	dwc3u_vbus_enable(priv);

	ret = dwc3u_phy_init(priv);
	if (ret)
		return ret;

	dwc3u_reset_init(priv);

	return 0;
}

static void dwc3u_exit(struct dwc3u_priv *priv)
{
	dwc3u_reset_clear(priv);
	dwc3u_vbus_disable(priv);
}

static void dwc3u_disable_clk(struct dwc3u_priv *priv)
{
	int i;

	for (i = 0; i < priv->nclks; i++) {
		clk_disable_unprepare(priv->clks[i]);
		clk_put(priv->clks[i]);
	}
}

static int dwc3u_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct dwc3u_priv *priv;
	struct resource	*res;
	struct clk *clk;
	int i, nr_clks;
	int ret = 0;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);
	if (WARN_ON(!priv->data))
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->dev = dev;

	node = dev->of_node;
	nr_clks = of_clk_get_parent_count(node);
	if (!nr_clks) {
		dev_err(dev, "failed to get clock property\n");
		return -ENODEV;
	}

	priv->clks = devm_kcalloc(priv->dev, nr_clks, sizeof(struct clk *),
				  GFP_KERNEL);
	if (!priv->clks)
		return -ENOMEM;

	for (i = 0; i < nr_clks; i++) {
		clk = of_clk_get(node, i);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			goto out_clk_disable;
		}
		ret = clk_prepare_enable(clk);
		if (ret < 0) {
			clk_put(clk);
			goto out_clk_disable;
		}
		priv->clks[i] = clk;
		priv->nclks = i;
	}

	priv->rst = devm_reset_control_array_get_optional_shared(priv->dev);
	if (IS_ERR(priv->rst)) {
		ret = PTR_ERR(priv->rst);
		goto out_clk_disable;
	}
	ret = reset_control_deassert(priv->rst);
	if (ret)
		goto out_clk_disable;

	ret = dwc3u_init(priv);
	if (ret)
		goto out_rst_assert;

	platform_set_drvdata(pdev, priv);

	ret = of_platform_populate(node, NULL, NULL, priv->dev);
	if (ret)
		goto out_exit;

	return 0;

out_exit:
	dwc3u_exit(priv);
out_rst_assert:
	reset_control_assert(priv->rst);
out_clk_disable:
	dwc3u_disable_clk(priv);

	return ret;
}

static int dwc3u_remove(struct platform_device *pdev)
{
	struct dwc3u_priv *priv = platform_get_drvdata(pdev);

	of_platform_depopulate(&pdev->dev);
	dwc3u_exit(priv);

	reset_control_assert(priv->rst);
	dwc3u_disable_clk(priv);

	return 0;
}

static const struct dwc3u_soc_data dwc3u_pxs2_data = {
	.ss_nparams = 7,
	.ss_param = {
		{  7, 0x0f, 0x0a },
		{  8, 0x0f, 0x03 },
		{  9, 0x0f, 0x05 },
		{ 11, 0x0f, 0x09 },
		{ 13, 0x60, 0x40 },
		{ 27, 0x07, 0x07 },
		{ 28, 0x03, 0x01 },
	},
	.hs_nparams = 0,
};

static const struct dwc3u_soc_data dwc3u_ld20_data = {
	.ss_nparams = 3,
	.ss_param = {
		{  7, 0x0f, 0x06 },
		{ 13, 0xff, 0xcc },
		{ 26, 0xf0, 0x50 },
	},
	.hs_nparams = 1,
	.hs_param = {
		{ 10, 0x60, 0x60 },
	},
	.trim_func = dwc3u_hsphy_trim_ld20,
	.hs_config0 = 0x92306680,
	.hs_config1 = 0x00000106,
};

static const struct of_device_id of_dwc3u_match[] = {
	{
		.compatible = "socionext,uniphier-pxs2-dwc3",
		.data = &dwc3u_pxs2_data,
	},
	{
		.compatible = "socionext,uniphier-ld20-dwc3",
		.data = &dwc3u_ld20_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_dwc3u_match);

static struct platform_driver dwc3u_driver = {
	.probe = dwc3u_probe,
	.remove = dwc3u_remove,
	.driver	= {
		.name = "uniphier-dwc3",
		.of_match_table	= of_dwc3u_match,
	},
};

module_platform_driver(dwc3u_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("DesignWare USB3 UniPhier glue layer");
MODULE_LICENSE("GPL v2");
