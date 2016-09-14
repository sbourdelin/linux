/*
 * pcie-dra7xx - PCIe controller driver for TI DRA7xx SoCs
 *
 * Copyright (C) 2013-2014 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

/* PCIe controller wrapper DRA7XX configuration registers */

#define	PCIECTRL_DRA7XX_CONF_SYSCONFIG			0x0010
#define SIDLE_MASK					3
#define SIDLE_SHIFT					2
#define SIDLE_FORCE					0x0
#define SIDLE_NO					0x1
#define SIDLE_SMART					0x2
#define SIDLE_SMART_WKUP				0x3

#define	PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN		0x0024
#define	PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MAIN		0x0028
#define	ERR_SYS						BIT(0)
#define	ERR_FATAL					BIT(1)
#define	ERR_NONFATAL					BIT(2)
#define	ERR_COR						BIT(3)
#define	ERR_AXI						BIT(4)
#define	ERR_ECRC					BIT(5)
#define	PME_TURN_OFF					BIT(8)
#define	PME_TO_ACK					BIT(9)
#define	PM_PME						BIT(10)
#define	LINK_REQ_RST					BIT(11)
#define	LINK_UP_EVT					BIT(12)
#define	CFG_BME_EVT					BIT(13)
#define	CFG_MSE_EVT					BIT(14)
#define	INTERRUPTS (ERR_SYS | ERR_FATAL | ERR_NONFATAL | ERR_COR | ERR_AXI | \
			ERR_ECRC | PME_TURN_OFF | PME_TO_ACK | PM_PME | \
			LINK_REQ_RST | LINK_UP_EVT | CFG_BME_EVT | CFG_MSE_EVT)

#define	PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI		0x0034
#define	PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MSI		0x0038
#define	INTA						BIT(0)
#define	INTB						BIT(1)
#define	INTC						BIT(2)
#define	INTD						BIT(3)
#define	MSI						BIT(4)
#define	LEG_EP_INTERRUPTS (INTA | INTB | INTC | INTD)

#define	PCIECTRL_TI_CONF_DEVICE_TYPE			0x0100
#define	DEVICE_TYPE_EP					0x0
#define	DEVICE_TYPE_LEG_EP				0x1
#define	DEVICE_TYPE_RC					0x4

#define	PCIECTRL_DRA7XX_CONF_DEVICE_CMD			0x0104
#define	LTSSM_EN					0x1

#define	PCIECTRL_DRA7XX_CONF_PHY_CS			0x010C
#define	LINK_UP						BIT(16)
#define	DRA7XX_CPU_TO_BUS_ADDR				0x0FFFFFFF

#define	PCIECTRL_TI_CONF_INTX_ASSERT			0x0124
#define	PCIECTRL_TI_CONF_INTX_DEASSERT			0x0128

struct dra7xx_pcie {
	void __iomem		*base;
	struct phy		**phy;
	int			phy_count;
	struct device		*dev;
	struct dw_pcie		*pci;
	enum dw_pcie_device_mode mode;
};

struct dra7xx_pcie_of_data {
	enum dw_pcie_device_mode mode;
};

#define to_dra7xx_pcie(x)	dev_get_drvdata((x)->dev)

static inline u32 dra7xx_pcie_readl(struct dra7xx_pcie *pcie, u32 offset)
{
	return readl(pcie->base + offset);
}

static inline void dra7xx_pcie_writel(struct dra7xx_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->base + offset);
}

static inline u32 dra7xx_pcie_readl_dbi(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}

static inline void dra7xx_pcie_writel_dbi(void __iomem *base, u32 offset,
					  u32 value)
{
	writel(value, base + offset);
}

static int dra7xx_pcie_link_up(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_PHY_CS);

	return !!(reg & LINK_UP);
}

static int dra7xx_pcie_start_link(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg;

	if (dw_pcie_link_up(pci)) {
		dev_err(pci->dev, "link is already up\n");
		return -EBUSY;
	}

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	reg |= LTSSM_EN;
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);

	return 0;
}

static void dra7xx_pcie_stop_link(struct dw_pcie *pci)
{
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	reg &= ~LTSSM_EN;
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);
}

static void dra7xx_pcie_enable_msi_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI,
			   ~LEG_EP_INTERRUPTS & ~MSI);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dra7xx_pcie_writel(dra7xx,
				   PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MSI, MSI);
	else
		dra7xx_pcie_writel(dra7xx,
				   PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MSI,
				   LEG_EP_INTERRUPTS);
}

static void dra7xx_pcie_enable_wrapper_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN,
			   ~INTERRUPTS);
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQENABLE_SET_MAIN,
			   INTERRUPTS);
}

static void dra7xx_pcie_enable_interrupts(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_enable_wrapper_interrupts(dra7xx);
	dra7xx_pcie_enable_msi_interrupts(dra7xx);
}

static void dra7xx_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);

	pp->io_base &= DRA7XX_CPU_TO_BUS_ADDR;
	pp->mem_base &= DRA7XX_CPU_TO_BUS_ADDR;
	pp->cfg0_base &= DRA7XX_CPU_TO_BUS_ADDR;
	pp->cfg1_base &= DRA7XX_CPU_TO_BUS_ADDR;

	dw_pcie_setup_rc(pp);

	dra7xx_pcie_start_link(pci);
	dw_pcie_wait_for_link(pci);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);

	dra7xx_pcie_enable_interrupts(dra7xx);
}

static const struct dw_pcie_host_ops dra7xx_pcie_host_ops = {
	.host_init = dra7xx_pcie_host_init,
};

static int dra7xx_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = dra7xx_pcie_intx_map,
};

static int dra7xx_pcie_init_irq_domain(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node =  of_get_next_child(node, NULL);

	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODEV;
	}

	pp->irq_domain = irq_domain_add_linear(pcie_intc_node, 4,
					       &intx_domain_ops, pp);
	if (!pp->irq_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENODEV;
	}

	return 0;
}

static irqreturn_t dra7xx_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI);

	switch (reg) {
	case MSI:
		dw_handle_msi_irq(pp);
		break;
	case INTA:
	case INTB:
	case INTC:
	case INTD:
		generic_handle_irq(irq_find_mapping(pp->irq_domain, ffs(reg)));
		break;
	}

	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MSI, reg);

	return IRQ_HANDLED;
}


static irqreturn_t dra7xx_pcie_irq_handler(int irq, void *arg)
{
	struct dra7xx_pcie *dra7xx = arg;
	struct dw_pcie *pci = dra7xx->pci;
	struct dw_pcie_ep *ep = &pci->ep;
	u32 reg;

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN);

	if (reg & ERR_SYS)
		dev_dbg(dra7xx->dev, "System Error\n");

	if (reg & ERR_FATAL)
		dev_dbg(dra7xx->dev, "Fatal Error\n");

	if (reg & ERR_NONFATAL)
		dev_dbg(dra7xx->dev, "Non Fatal Error\n");

	if (reg & ERR_COR)
		dev_dbg(dra7xx->dev, "Correctable Error\n");

	if (reg & ERR_AXI)
		dev_dbg(dra7xx->dev, "AXI tag lookup fatal Error\n");

	if (reg & ERR_ECRC)
		dev_dbg(dra7xx->dev, "ECRC Error\n");

	if (reg & PME_TURN_OFF)
		dev_dbg(dra7xx->dev,
			"Power Management Event Turn-Off message received\n");

	if (reg & PME_TO_ACK)
		dev_dbg(dra7xx->dev,
			"Power Management Turn-Off Ack message received\n");

	if (reg & PM_PME)
		dev_dbg(dra7xx->dev,
			"PM Power Management Event message received\n");

	if (reg & LINK_REQ_RST)
		dev_dbg(dra7xx->dev, "Link Request Reset\n");

	if (reg & LINK_UP_EVT) {
		if (dra7xx->mode == DW_PCIE_EP_TYPE)
			dw_pcie_ep_linkup(ep);
		dev_dbg(dra7xx->dev, "Link-up state change\n");
	}

	if (reg & CFG_BME_EVT)
		dev_dbg(dra7xx->dev, "CFG 'Bus Master Enable' change\n");

	if (reg & CFG_MSE_EVT)
		dev_dbg(dra7xx->dev, "CFG 'Memory Space Enable' change\n");

	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_IRQSTATUS_MAIN, reg);

	return IRQ_HANDLED;
}

static void dra7xx_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);

	dra7xx_pcie_enable_wrapper_interrupts(dra7xx);
}

static void dra7xx_pcie_raise_legacy_irq(struct dra7xx_pcie *dra7xx)
{
	dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_INTX_ASSERT, 0x1);
	mdelay(1);
	dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_INTX_DEASSERT, 0x1);
}

void dra7xx_pcie_raise_msi_irq(struct dra7xx_pcie *dra7xx)
{
	/* TODO */
}

static int dra7xx_pcie_raise_irq(struct dw_pcie_ep *ep,
				 enum pci_epc_irq_type type)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dra7xx_pcie *dra7xx = to_dra7xx_pcie(pci);

	switch (type) {
	case PCI_EPC_IRQ_LEGACY:
		dra7xx_pcie_raise_legacy_irq(dra7xx);
		break;
	case PCI_EPC_IRQ_MSI:
		dra7xx_pcie_raise_msi_irq(dra7xx);
		break;
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static struct dw_pcie_ep_ops pcie_ep_ops = {
	.ep_init = dra7xx_pcie_ep_init,
	.raise_irq = dra7xx_pcie_raise_irq,
};

static int __init dra7xx_add_pcie_ep(struct dra7xx_pcie *dra7xx,
				     struct platform_device *pdev)
{
	int ret;
	struct dw_pcie_ep *ep;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = dra7xx->pci;

	ep = &pci->ep;
	ep->ops = &pcie_ep_ops;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ep_dbics");
	pci->dbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->dbi_base)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ep_dbics2");
	pci->dbi_base2 = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->dbi_base2)
		return -ENOMEM;

	ret = dw_pcie_ep_init(ep);
	if (ret) {
		dev_err(dra7xx->dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int __init dra7xx_add_pcie_port(struct dra7xx_pcie *dra7xx,
				       struct platform_device *pdev)
{
	int ret;
	struct pcie_port *pp;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = dra7xx->pci;

	pp = &pci->pp;
	pp->ops = &dra7xx_pcie_host_ops;

	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0) {
		dev_err(dev, "missing IRQ resource\n");
		return -EINVAL;
	}

	ret = devm_request_irq(&pdev->dev, pp->irq,
			       dra7xx_pcie_msi_irq_handler,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       "dra7-pcie-msi",	pp);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		return ret;
	}

	if (!IS_ENABLED(CONFIG_PCI_MSI)) {
		ret = dra7xx_pcie_init_irq_domain(pp);
		if (ret < 0)
			return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbics");
	pci->dbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->dbi_base)
		return -ENOMEM;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dra7xx->dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static const struct dra7xx_pcie_of_data dra7xx_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,
};

static const struct dra7xx_pcie_of_data dra7xx_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id of_dra7xx_pcie_match[] = {
	{
		.compatible = "ti,dra7-pcie",
		.data = &dra7xx_pcie_rc_of_data,
	},
	{
		.compatible = "ti,dra7-pcie-ep",
		.data = &dra7xx_pcie_ep_of_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_dra7xx_pcie_match);

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = dra7xx_pcie_start_link,
	.stop_link = dra7xx_pcie_stop_link,
	.link_up = dra7xx_pcie_link_up,
};

static int dra7xx_pcie_gpio_reset(struct device *dev)
{
	int ret;
	int gpio_sel;
	enum of_gpio_flags flags;
	unsigned long gpio_flags;

	gpio_sel = of_get_gpio_flags(dev->of_node, 0, &flags);
	if (gpio_is_valid(gpio_sel)) {
		gpio_flags = (flags & OF_GPIO_ACTIVE_LOW) ?
			      GPIOF_OUT_INIT_LOW : GPIOF_OUT_INIT_HIGH;
		ret = devm_gpio_request_one(dev, gpio_sel, gpio_flags,
					    "pcie_reset");
		if (ret)
			dev_err(dev, "gpio%d request failed, ret %d\n",
				gpio_sel, ret);
		return ret;
	}
	if (gpio_sel == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	return 0;
}

static int __init dra7xx_pcie_probe(struct platform_device *pdev)
{
	u32 reg;
	int ret;
	int irq;
	int i;
	int phy_count;
	struct phy **phy;
	void __iomem *base;
	struct resource *res;
	struct dw_pcie *pci;
	struct dra7xx_pcie *dra7xx;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	char name[10];
	const struct of_device_id *match;
	const struct dra7xx_pcie_of_data *data;
	enum dw_pcie_device_mode mode;

	match = of_match_device(of_match_ptr(of_dra7xx_pcie_match), dev);
	if (!match)
		return -EINVAL;

	data = (struct dra7xx_pcie_of_data *)match->data;
	mode = (enum dw_pcie_device_mode)data->mode;

	dra7xx = devm_kzalloc(dev, sizeof(*dra7xx), GFP_KERNEL);
	if (!dra7xx)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "missing IRQ resource\n");
		return -EINVAL;
	}

	ret = devm_request_irq(dev, irq, dra7xx_pcie_irq_handler,
			       IRQF_SHARED, "dra7xx-pcie-main", dra7xx);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ti_conf");
	base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	phy_count = of_property_count_strings(np, "phy-names");
	if (phy_count < 0) {
		dev_err(dev, "unable to find the strings\n");
		return phy_count;
	}

	phy = devm_kzalloc(dev, sizeof(*phy) * phy_count, GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	for (i = 0; i < phy_count; i++) {
		snprintf(name, sizeof(name), "pcie-phy%d", i);
		phy[i] = devm_phy_get(dev, name);
		if (IS_ERR(phy[i]))
			return PTR_ERR(phy[i]);

		ret = phy_init(phy[i]);
		if (ret < 0)
			goto err_phy;

		ret = phy_power_on(phy[i]);
		if (ret < 0) {
			phy_exit(phy[i]);
			goto err_phy;
		}
	}

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	dra7xx->base = base;
	dra7xx->phy = phy;
	dra7xx->pci = pci;
	dra7xx->dev = dev;
	dra7xx->phy_count = phy_count;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD);
	reg &= ~LTSSM_EN;
	dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_DEVICE_CMD, reg);

	platform_set_drvdata(pdev, dra7xx);

	switch (mode) {
	case DW_PCIE_RC_TYPE:
		ret = dra7xx_pcie_gpio_reset(dev);
		if (ret)
			goto err_gpio;
		dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_DEVICE_TYPE,
				   DEVICE_TYPE_RC);
		ret = dra7xx_add_pcie_port(dra7xx, pdev);
		if (ret < 0)
			goto err_gpio;
		break;
	case DW_PCIE_EP_TYPE:
		reg = dra7xx_pcie_readl(dra7xx, PCIECTRL_DRA7XX_CONF_SYSCONFIG);
		reg &= ~(SIDLE_MASK << SIDLE_SHIFT);
		reg |= SIDLE_SMART_WKUP << SIDLE_SHIFT;
		dra7xx_pcie_writel(dra7xx, PCIECTRL_DRA7XX_CONF_SYSCONFIG, reg);
		dra7xx_pcie_writel(dra7xx, PCIECTRL_TI_CONF_DEVICE_TYPE,
				   DEVICE_TYPE_EP);
		ret = dra7xx_add_pcie_ep(dra7xx, pdev);
		if (ret < 0)
			goto err_gpio;
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
	}
	dra7xx->mode = mode;

	return 0;

err_gpio:
	pm_runtime_put(dev);

err_get_sync:
	pm_runtime_disable(dev);

err_phy:
	while (--i >= 0) {
		phy_power_off(phy[i]);
		phy_exit(phy[i]);
	}

	return ret;
}

static int __exit dra7xx_pcie_remove(struct platform_device *pdev)
{
	struct dra7xx_pcie *dra7xx = platform_get_drvdata(pdev);
	struct dw_pcie *pci = dra7xx->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int count = dra7xx->phy_count;

	if (dra7xx->mode == DW_PCIE_RC_TYPE && pp->irq_domain)
		irq_domain_remove(pp->irq_domain);
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	while (count--) {
		phy_power_off(dra7xx->phy[count]);
		phy_exit(dra7xx->phy[count]);
	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int dra7xx_pcie_suspend(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	struct dw_pcie *pci = dra7xx->pci;
	u32 val;

	if (dra7xx->mode != DW_PCIE_RC_TYPE)
		return 0;

	/* clear MSE */
	val = dra7xx_pcie_readl_dbi(pci->dbi_base, PCI_COMMAND);
	val &= ~PCI_COMMAND_MEMORY;
	dra7xx_pcie_writel_dbi(pci->dbi_base, PCI_COMMAND, val);

	return 0;
}

static int dra7xx_pcie_resume(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	struct dw_pcie *pci = dra7xx->pci;
	u32 val;

	if (dra7xx->mode != DW_PCIE_RC_TYPE)
		return 0;

	/* set MSE */
	val = dra7xx_pcie_readl_dbi(pci->dbi_base, PCI_COMMAND);
	val |= PCI_COMMAND_MEMORY;
	dra7xx_pcie_writel_dbi(pci->dbi_base, PCI_COMMAND, val);

	return 0;
}

static int dra7xx_pcie_suspend_noirq(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	int count = dra7xx->phy_count;

	while (count--) {
		phy_power_off(dra7xx->phy[count]);
		phy_exit(dra7xx->phy[count]);
	}

	return 0;
}

static int dra7xx_pcie_resume_noirq(struct device *dev)
{
	struct dra7xx_pcie *dra7xx = dev_get_drvdata(dev);
	int phy_count = dra7xx->phy_count;
	int ret;
	int i;

	for (i = 0; i < phy_count; i++) {
		ret = phy_init(dra7xx->phy[i]);
		if (ret < 0)
			goto err_phy;

		ret = phy_power_on(dra7xx->phy[i]);
		if (ret < 0) {
			phy_exit(dra7xx->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(dra7xx->phy[i]);
		phy_exit(dra7xx->phy[i]);
	}

	return ret;
}
#endif

static const struct dev_pm_ops dra7xx_pcie_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dra7xx_pcie_suspend, dra7xx_pcie_resume)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(dra7xx_pcie_suspend_noirq,
				      dra7xx_pcie_resume_noirq)
};

static struct platform_driver dra7xx_pcie_driver = {
	.remove		= __exit_p(dra7xx_pcie_remove),
	.driver = {
		.name	= "dra7-pcie",
		.of_match_table = of_dra7xx_pcie_match,
		.pm	= &dra7xx_pcie_pm_ops,
	},
};

module_platform_driver_probe(dra7xx_pcie_driver, dra7xx_pcie_probe);

MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_DESCRIPTION("TI PCIe controller driver");
MODULE_LICENSE("GPL v2");
