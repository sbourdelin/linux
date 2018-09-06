// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for UniPhier SoCs
 * Copyright 2018 Socionext Inc.
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "pcie-designware.h"

#define PCL_PINCTRL0			0x002c
#define PCL_PERST_PLDN_REGEN		BIT(12)
#define PCL_PERST_NOE_REGEN		BIT(11)
#define PCL_PERST_OUT_REGEN		BIT(8)
#define PCL_PERST_PLDN_REGVAL		BIT(4)
#define PCL_PERST_NOE_REGVAL		BIT(3)
#define PCL_PERST_OUT_REGVAL		BIT(0)

#define PCL_PIPEMON			0x0044
#define PCL_PCLK_ALIVE			BIT(15)

#define PCL_APP_READY_CTRL		0x8008
#define PCL_APP_LTSSM_ENABLE		BIT(0)

#define PCL_APP_PM0			0x8078
#define PCL_SYS_AUX_PWR_DET		BIT(8)

#define PCL_RCV_INT			0x8108
#define PCL_CFG_BW_MGT_ENABLE		BIT(20)
#define PCL_CFG_LINK_AUTO_BW_ENABLE	BIT(19)
#define PCL_CFG_AER_RC_ERR_MSI_ENABLE	BIT(18)
#define PCL_CFG_PME_MSI_ENABLE		BIT(17)
#define PCL_CFG_BW_MGT_STATUS		BIT(4)
#define PCL_CFG_LINK_AUTO_BW_STATUS	BIT(3)
#define PCL_CFG_AER_RC_ERR_MSI_STATUS	BIT(2)
#define PCL_CFG_PME_MSI_STATUS		BIT(1)
#define PCL_RCV_INT_ALL_ENABLE			\
	(PCL_CFG_BW_MGT_ENABLE | PCL_CFG_LINK_AUTO_BW_ENABLE \
	 | PCL_CFG_AER_RC_ERR_MSI_ENABLE | PCL_CFG_PME_MSI_ENABLE)

#define PCL_RCV_INTX			0x810c
#define PCL_RADM_INTD_ENABLE		BIT(19)
#define PCL_RADM_INTC_ENABLE		BIT(18)
#define PCL_RADM_INTB_ENABLE		BIT(17)
#define PCL_RADM_INTA_ENABLE		BIT(16)
#define PCL_RADM_INTD_STATUS		BIT(3)
#define PCL_RADM_INTC_STATUS		BIT(2)
#define PCL_RADM_INTB_STATUS		BIT(1)
#define PCL_RADM_INTA_STATUS		BIT(0)
#define PCL_RCV_INTX_ALL_ENABLE			\
	(PCL_RADM_INTD_ENABLE | PCL_RADM_INTC_ENABLE \
	 | PCL_RADM_INTB_ENABLE	| PCL_RADM_INTA_ENABLE)

#define PCL_STATUS_LINK			0x8140
#define PCL_RDLH_LINK_UP		BIT(1)
#define PCL_XMLH_LINK_UP		BIT(0)

struct uniphier_pcie_priv {
	void __iomem *base;
	struct dw_pcie pci;
	struct clk *clk;
	struct reset_control *rst;
	struct phy *phy;
	struct irq_domain *irq_domain;
};

#define to_uniphier_pcie(x)	dev_get_drvdata((x)->dev)

static void uniphier_pcie_ltssm_enable(struct uniphier_pcie_priv *priv)
{
	u32 val;

	val = readl(priv->base + PCL_APP_READY_CTRL);
	val |= PCL_APP_LTSSM_ENABLE;
	writel(val, priv->base + PCL_APP_READY_CTRL);
}

static void uniphier_pcie_ltssm_disable(struct uniphier_pcie_priv *priv)
{
	u32 val;

	val = readl(priv->base + PCL_APP_READY_CTRL);
	val &= ~PCL_APP_LTSSM_ENABLE;
	writel(val, priv->base + PCL_APP_READY_CTRL);
}

static void uniphier_pcie_init_rc(struct uniphier_pcie_priv *priv)
{
	u32 val;

	/* use auxiliary power detection */
	val = readl(priv->base + PCL_APP_PM0);
	val |= PCL_SYS_AUX_PWR_DET;
	writel(val, priv->base + PCL_APP_PM0);

	/* assert PERST# */
	val = readl(priv->base + PCL_PINCTRL0);
	val &= ~(PCL_PERST_NOE_REGVAL | PCL_PERST_OUT_REGVAL
		 | PCL_PERST_PLDN_REGVAL);
	val |= PCL_PERST_NOE_REGEN | PCL_PERST_OUT_REGEN
		| PCL_PERST_PLDN_REGEN;
	writel(val, priv->base + PCL_PINCTRL0);

	uniphier_pcie_ltssm_disable(priv);

	usleep_range(100000, 200000);

	/* deassert PERST# */
	val = readl(priv->base + PCL_PINCTRL0);
	val |= PCL_PERST_OUT_REGVAL | PCL_PERST_OUT_REGEN;
	writel(val, priv->base + PCL_PINCTRL0);
}

static int uniphier_pcie_wait_rc(struct uniphier_pcie_priv *priv)
{
	u32 status;
	int ret;

	/* wait PIPE clock */
	ret = readl_poll_timeout(priv->base + PCL_PIPEMON, status,
				 status & PCL_PCLK_ALIVE, 100000, 1000000);
	if (ret) {
		dev_err(priv->pci.dev,
			"Failed to initialize controller in RC mode\n");
		return ret;
	}

	return 0;
}

static int uniphier_pcie_link_up(struct dw_pcie *pci)
{
	struct uniphier_pcie_priv *priv = to_uniphier_pcie(pci);
	u32 val, mask;

	val = readl(priv->base + PCL_STATUS_LINK);
	mask = PCL_RDLH_LINK_UP | PCL_XMLH_LINK_UP;

	return (val & mask) == mask;
}

static int uniphier_pcie_establish_link(struct dw_pcie *pci)
{
	struct uniphier_pcie_priv *priv = to_uniphier_pcie(pci);
	int ret;

	if (dw_pcie_link_up(pci))
		return 0;

	uniphier_pcie_ltssm_enable(priv);

	ret = dw_pcie_wait_for_link(pci);
	if (ret == -ETIMEDOUT) {
		dev_warn(pci->dev, "Link not up\n");
		ret = 0;
	}

	return ret;
}

static void uniphier_pcie_stop_link(struct dw_pcie *pci)
{
	struct uniphier_pcie_priv *priv = to_uniphier_pcie(pci);

	uniphier_pcie_ltssm_disable(priv);
}

static int uniphier_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops uniphier_intx_domain_ops = {
	.map = uniphier_pcie_intx_map,
};

static int uniphier_pcie_init_irq_domain(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct uniphier_pcie_priv *priv = to_uniphier_pcie(pci);
	struct device_node *np = pci->dev->of_node;
	struct device_node *np_intc = of_get_next_child(np, NULL);

	if (!np_intc) {
		dev_err(pci->dev, "Failed to get child node\n");
		return -ENODEV;
	}

	priv->irq_domain = irq_domain_add_linear(np_intc, PCI_NUM_INTX,
						 &uniphier_intx_domain_ops,
						 pp);
	if (!priv->irq_domain) {
		dev_err(pci->dev, "Failed to get INTx domain\n");
		return -ENODEV;
	}

	return 0;
}

static void uniphier_pcie_irq_enable(struct uniphier_pcie_priv *priv)
{
	writel(PCL_RCV_INT_ALL_ENABLE, priv->base + PCL_RCV_INT);
	writel(PCL_RCV_INTX_ALL_ENABLE, priv->base + PCL_RCV_INTX);
}

static void uniphier_pcie_irq_disable(struct uniphier_pcie_priv *priv)
{
	writel(0, priv->base + PCL_RCV_INT);
	writel(0, priv->base + PCL_RCV_INTX);
}

static irqreturn_t uniphier_pcie_irq_handler(int irq, void *arg)
{
	struct uniphier_pcie_priv *priv = arg;
	struct dw_pcie *pci = &priv->pci;
	u32 val;

	/* INT for debug */
	val = readl(priv->base + PCL_RCV_INT);

	if (val & PCL_CFG_BW_MGT_STATUS)
		dev_dbg(pci->dev, "Link Bandwidth Management Event\n");
	if (val & PCL_CFG_LINK_AUTO_BW_STATUS)
		dev_dbg(pci->dev, "Link Autonomous Bandwidth Event\n");
	if (val & PCL_CFG_AER_RC_ERR_MSI_STATUS)
		dev_dbg(pci->dev, "Root Error\n");
	if (val & PCL_CFG_PME_MSI_STATUS)
		dev_dbg(pci->dev, "PME Interrupt\n");

	writel(val, priv->base + PCL_RCV_INT);

	/* INTx */
	val = readl(priv->base + PCL_RCV_INTX);

	if (val & PCL_RADM_INTA_STATUS)
		generic_handle_irq(irq_find_mapping(priv->irq_domain, 0));
	if (val & PCL_RADM_INTB_STATUS)
		generic_handle_irq(irq_find_mapping(priv->irq_domain, 1));
	if (val & PCL_RADM_INTC_STATUS)
		generic_handle_irq(irq_find_mapping(priv->irq_domain, 2));
	if (val & PCL_RADM_INTD_STATUS)
		generic_handle_irq(irq_find_mapping(priv->irq_domain, 3));

	writel(val, priv->base + PCL_RCV_INTX);

	return IRQ_HANDLED;
}

static irqreturn_t uniphier_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;

	return dw_handle_msi_irq(pp);
}

static int uniphier_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	int ret;

	dw_pcie_setup_rc(pp);
	ret = uniphier_pcie_establish_link(pci);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);

	return 0;
}

static const struct dw_pcie_host_ops uniphier_pcie_host_ops = {
	.host_init = uniphier_pcie_host_init,
};

static int uniphier_add_pcie_port(struct uniphier_pcie_priv *priv,
				  struct platform_device *pdev)
{
	struct dw_pcie *pci = &priv->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->root_bus_nr = -1;
	pp->ops = &uniphier_pcie_host_ops;

	pp->irq = platform_get_irq_byname(pdev, "intx");
	if (pp->irq < 0) {
		dev_err(dev, "Failed to get intx irq\n");
		return pp->irq;
	}

	ret = devm_request_irq(dev, pp->irq, uniphier_pcie_irq_handler,
			       IRQF_SHARED, "pcie", priv);
	if (ret) {
		dev_err(dev, "Failed to request irq %d\n", pp->irq);
		return ret;
	}

	ret = uniphier_pcie_init_irq_domain(pp);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pp->msi_irq < 0)
			return pp->msi_irq;

		ret = devm_request_irq(dev, pp->msi_irq,
				       uniphier_pcie_msi_irq_handler,
				       IRQF_SHARED, "pcie-msi", pp);
		if (ret) {
			dev_err(dev, "failed to request msi_irq %d\n",
				pp->msi_irq);
			return ret;
		}
	}

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int uniphier_pcie_host_enable(struct uniphier_pcie_priv *priv)
{
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return ret;

	ret = reset_control_deassert(priv->rst);
	if (ret)
		goto out_clk_disable;

	uniphier_pcie_init_rc(priv);

	ret = phy_init(priv->phy);
	if (ret)
		goto out_rst_assert;

	ret = uniphier_pcie_wait_rc(priv);
	if (ret)
		goto out_phy_exit;

	uniphier_pcie_irq_enable(priv);

	return 0;

out_phy_exit:
	phy_exit(priv->phy);
out_rst_assert:
	reset_control_assert(priv->rst);
out_clk_disable:
	clk_disable_unprepare(priv->clk);

	return ret;
}

static void uniphier_pcie_host_disable(struct uniphier_pcie_priv *priv)
{
	uniphier_pcie_irq_disable(priv);
	phy_exit(priv->phy);
	reset_control_assert(priv->rst);
	clk_disable_unprepare(priv->clk);
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = uniphier_pcie_establish_link,
	.stop_link = uniphier_pcie_stop_link,
	.link_up = uniphier_pcie_link_up,
};

static int uniphier_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_pcie_priv *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pci.dev = dev;
	priv->pci.ops = &dw_pcie_ops;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	priv->pci.dbi_base = devm_pci_remap_cfg_resource(dev, res);
	if (IS_ERR(priv->pci.dbi_base))
		return PTR_ERR(priv->pci.dbi_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "link");
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);

	priv->rst = devm_reset_control_get_shared(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	priv->phy = devm_phy_optional_get(dev, "pcie-phy");
	if (IS_ERR(priv->phy))
		return PTR_ERR(priv->phy);

	platform_set_drvdata(pdev, priv);

	ret = uniphier_pcie_host_enable(priv);
	if (ret)
		return ret;

	return uniphier_add_pcie_port(priv, pdev);
}

static int uniphier_pcie_remove(struct platform_device *pdev)
{
	struct uniphier_pcie_priv *priv = platform_get_drvdata(pdev);

	uniphier_pcie_host_disable(priv);

	return 0;
}

static const struct of_device_id uniphier_pcie_match[] = {
	{ .compatible = "socionext,uniphier-pcie", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, uniphier_pcie_match);

static struct platform_driver uniphier_pcie_driver = {
	.probe  = uniphier_pcie_probe,
	.remove = uniphier_pcie_remove,
	.driver = {
		.name = "uniphier-pcie",
		.of_match_table = uniphier_pcie_match,
	},
};
builtin_platform_driver(uniphier_pcie_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("UniPhier PCIe host controller driver");
MODULE_LICENSE("GPL v2");
