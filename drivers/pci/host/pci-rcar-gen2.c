/*
 *  pci-rcar-gen2: internal PCI bus support
 *
 * Copyright (C) 2013 Renesas Solutions Corp.
 * Copyright (C) 2013 Cogent Embedded, Inc.
 *
 * Author: Valentine Barshak <valentine.barshak@cogentembedded.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>

/* AHB-PCI Bridge PCI communication registers */
#define RCAR_AHBPCI_PCICOM_OFFSET	0x800

#define RCAR_PCIAHB_WIN1_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x00)
#define RCAR_PCIAHB_WIN2_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x04)
#define RCAR_PCIAHB_PREFETCH0		0x0
#define RCAR_PCIAHB_PREFETCH4		0x1
#define RCAR_PCIAHB_PREFETCH8		0x2
#define RCAR_PCIAHB_PREFETCH16		0x3

#define RCAR_AHBPCI_WIN1_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x10)
#define RCAR_AHBPCI_WIN2_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x14)
#define RCAR_AHBPCI_WIN_CTR_MEM		(3 << 1)
#define RCAR_AHBPCI_WIN_CTR_CFG		(5 << 1)
#define RCAR_AHBPCI_WIN1_HOST		(1 << 30)
#define RCAR_AHBPCI_WIN1_DEVICE		(1 << 31)

#define RCAR_PCI_INT_ENABLE_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x20)
#define RCAR_PCI_INT_STATUS_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x24)
#define RCAR_PCI_INT_SIGTABORT		(1 << 0)
#define RCAR_PCI_INT_SIGRETABORT	(1 << 1)
#define RCAR_PCI_INT_REMABORT		(1 << 2)
#define RCAR_PCI_INT_PERR		(1 << 3)
#define RCAR_PCI_INT_SIGSERR		(1 << 4)
#define RCAR_PCI_INT_RESERR		(1 << 5)
#define RCAR_PCI_INT_WIN1ERR		(1 << 12)
#define RCAR_PCI_INT_WIN2ERR		(1 << 13)
#define RCAR_PCI_INT_A			(1 << 16)
#define RCAR_PCI_INT_B			(1 << 17)
#define RCAR_PCI_INT_PME		(1 << 19)
#define RCAR_PCI_INT_ALLERRORS (RCAR_PCI_INT_SIGTABORT		| \
				RCAR_PCI_INT_SIGRETABORT	| \
				RCAR_PCI_INT_SIGRETABORT	| \
				RCAR_PCI_INT_REMABORT		| \
				RCAR_PCI_INT_PERR		| \
				RCAR_PCI_INT_SIGSERR		| \
				RCAR_PCI_INT_RESERR		| \
				RCAR_PCI_INT_WIN1ERR		| \
				RCAR_PCI_INT_WIN2ERR)

#define RCAR_AHB_BUS_CTR_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x30)
#define RCAR_AHB_BUS_MMODE_HTRANS	(1 << 0)
#define RCAR_AHB_BUS_MMODE_BYTE_BURST	(1 << 1)
#define RCAR_AHB_BUS_MMODE_WR_INCR	(1 << 2)
#define RCAR_AHB_BUS_MMODE_HBUS_REQ	(1 << 7)
#define RCAR_AHB_BUS_SMODE_READYCTR	(1 << 17)
#define RCAR_AHB_BUS_MODE		(RCAR_AHB_BUS_MMODE_HTRANS |	\
					RCAR_AHB_BUS_MMODE_BYTE_BURST |	\
					RCAR_AHB_BUS_MMODE_WR_INCR |	\
					RCAR_AHB_BUS_MMODE_HBUS_REQ |	\
					RCAR_AHB_BUS_SMODE_READYCTR)

#define RCAR_USBCTR_REG			(RCAR_AHBPCI_PCICOM_OFFSET + 0x34)
#define RCAR_USBCTR_USBH_RST		(1 << 0)
#define RCAR_USBCTR_PCICLK_MASK		(1 << 1)
#define RCAR_USBCTR_PLL_RST		(1 << 2)
#define RCAR_USBCTR_DIRPD		(1 << 8)
#define RCAR_USBCTR_PCIAHB_WIN2_EN	(1 << 9)
#define RCAR_USBCTR_PCIAHB_WIN1_256M	(0 << 10)
#define RCAR_USBCTR_PCIAHB_WIN1_512M	(1 << 10)
#define RCAR_USBCTR_PCIAHB_WIN1_1G	(2 << 10)
#define RCAR_USBCTR_PCIAHB_WIN1_2G	(3 << 10)
#define RCAR_USBCTR_PCIAHB_WIN1_MASK	(3 << 10)

#define RCAR_PCI_ARBITER_CTR_REG	(RCAR_AHBPCI_PCICOM_OFFSET + 0x40)
#define RCAR_PCI_ARBITER_PCIREQ0	(1 << 0)
#define RCAR_PCI_ARBITER_PCIREQ1	(1 << 1)
#define RCAR_PCI_ARBITER_PCIBP_MODE	(1 << 12)

#define RCAR_PCI_UNIT_REV_REG		(RCAR_AHBPCI_PCICOM_OFFSET + 0x48)

struct rcar_pci {
	struct device *dev;
	void __iomem *reg;
	struct resource mem_res;
	struct resource *cfg_res;
	unsigned busnr;
	int irq;
	unsigned long window_size;
	unsigned long window_addr;
	unsigned long window_pci;
};

static u32 rcar_pci_readl(struct rcar_pci *rcar_pci, u32 offset)
{
	return ioread32(rcar_pci->reg + offset);
}

static void rcar_pci_writel(struct rcar_pci *rcar_pci, u32 offset, u32 val)
{
	iowrite32(val, rcar_pci->reg + offset);
}

/* PCI configuration space operations */
static void __iomem *rcar_pci_cfg_base(struct pci_bus *bus, unsigned int devfn,
				       int where)
{
	struct pci_sys_data *sys = bus->sysdata;
	struct rcar_pci *rcar_pci = sys->private_data;
	int slot, val;

	if (sys->busnr != bus->number || PCI_FUNC(devfn))
		return NULL;

	/* Only one EHCI/OHCI device built-in */
	slot = PCI_SLOT(devfn);
	if (slot > 2)
		return NULL;

	/* bridge logic only has registers to 0x40 */
	if (slot == 0x0 && where >= 0x40)
		return NULL;

	val = slot ? RCAR_AHBPCI_WIN1_DEVICE | RCAR_AHBPCI_WIN_CTR_CFG :
		     RCAR_AHBPCI_WIN1_HOST | RCAR_AHBPCI_WIN_CTR_CFG;

	rcar_pci_writel(rcar_pci, RCAR_AHBPCI_WIN1_CTR_REG, val);
	return rcar_pci->reg + (slot >> 1) * 0x100 + where;
}

/* PCI interrupt mapping */
static int rcar_pci_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct pci_sys_data *sys = dev->bus->sysdata;
	struct rcar_pci *rcar_pci = sys->private_data;
	int irq;

	irq = of_irq_parse_and_map_pci(dev, slot, pin);
	if (!irq)
		irq = rcar_pci->irq;

	return irq;
}

#ifdef CONFIG_PCI_DEBUG
/* if debug enabled, then attach an error handler irq to the bridge */

static irqreturn_t rcar_pci_err_irq(int irq, void *pw)
{
	struct rcar_pci *rcar_pci = pw;
	struct device *dev = rcar_pci->dev;
	u32 status = rcar_pci_readl(rcar_pci, RCAR_PCI_INT_STATUS_REG);

	if (status & RCAR_PCI_INT_ALLERRORS) {
		dev_err(dev, "error irq: status %08x\n", status);

		/* clear the error(s) */
		rcar_pci_writel(rcar_pci, RCAR_PCI_INT_STATUS_REG,
			    status & RCAR_PCI_INT_ALLERRORS);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void rcar_pci_setup_errirq(struct rcar_pci *rcar_pci)
{
	struct device *dev = rcar_pci->dev;
	int ret;
	u32 val;

	ret = devm_request_irq(dev, rcar_pci->irq, rcar_pci_err_irq,
			       IRQF_SHARED, "error irq", rcar_pci);
	if (ret) {
		dev_err(dev, "cannot claim IRQ for error handling\n");
		return;
	}

	val = rcar_pci_readl(rcar_pci, RCAR_PCI_INT_ENABLE_REG);
	val |= RCAR_PCI_INT_ALLERRORS;
	rcar_pci_writel(rcar_pci, RCAR_PCI_INT_ENABLE_REG, val);
}
#else
static inline void rcar_pci_setup_errirq(struct rcar_pci *rcar_pci) { }
#endif

/* PCI host controller setup */
static int rcar_pci_setup(int nr, struct pci_sys_data *sys)
{
	struct rcar_pci *rcar_pci = sys->private_data;
	struct device *dev = rcar_pci->dev;
	u32 val;
	int ret;

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	val = rcar_pci_readl(rcar_pci, RCAR_PCI_UNIT_REV_REG);
	dev_info(dev, "PCI: bus%u revision %x\n", sys->busnr, val);

	/* Disable Direct Power Down State and assert reset */
	val = rcar_pci_readl(rcar_pci, RCAR_USBCTR_REG) & ~RCAR_USBCTR_DIRPD;
	val |= RCAR_USBCTR_USBH_RST | RCAR_USBCTR_PLL_RST;
	rcar_pci_writel(rcar_pci, RCAR_USBCTR_REG, val);
	udelay(4);

	/* De-assert reset and reset PCIAHB window1 size */
	val &= ~(RCAR_USBCTR_PCIAHB_WIN1_MASK | RCAR_USBCTR_PCICLK_MASK |
		 RCAR_USBCTR_USBH_RST | RCAR_USBCTR_PLL_RST);

	/* Setup PCIAHB window1 size */
	switch (rcar_pci->window_size) {
	case SZ_2G:
		val |= RCAR_USBCTR_PCIAHB_WIN1_2G;
		break;
	case SZ_1G:
		val |= RCAR_USBCTR_PCIAHB_WIN1_1G;
		break;
	case SZ_512M:
		val |= RCAR_USBCTR_PCIAHB_WIN1_512M;
		break;
	default:
		pr_warn("unknown window size %ld - defaulting to 256M\n",
			rcar_pci->window_size);
		rcar_pci->window_size = SZ_256M;
		/* fall-through */
	case SZ_256M:
		val |= RCAR_USBCTR_PCIAHB_WIN1_256M;
		break;
	}
	rcar_pci_writel(rcar_pci, RCAR_USBCTR_REG, val);

	/* Configure AHB master and slave modes */
	rcar_pci_writel(rcar_pci, RCAR_AHB_BUS_CTR_REG, RCAR_AHB_BUS_MODE);

	/* Configure PCI arbiter */
	val = rcar_pci_readl(rcar_pci, RCAR_PCI_ARBITER_CTR_REG);
	val |= RCAR_PCI_ARBITER_PCIREQ0 | RCAR_PCI_ARBITER_PCIREQ1 |
	       RCAR_PCI_ARBITER_PCIBP_MODE;
	rcar_pci_writel(rcar_pci, RCAR_PCI_ARBITER_CTR_REG, val);

	/* PCI-AHB mapping */
	rcar_pci_writel(rcar_pci, RCAR_PCIAHB_WIN1_CTR_REG,
			rcar_pci->window_addr | RCAR_PCIAHB_PREFETCH16);

	/* AHB-PCI mapping: OHCI/EHCI registers */
	val = rcar_pci->mem_res.start | RCAR_AHBPCI_WIN_CTR_MEM;
	rcar_pci_writel(rcar_pci, RCAR_AHBPCI_WIN2_CTR_REG, val);

	/* Enable AHB-PCI bridge PCI configuration access */
	rcar_pci_writel(rcar_pci, RCAR_AHBPCI_WIN1_CTR_REG,
			RCAR_AHBPCI_WIN1_HOST | RCAR_AHBPCI_WIN_CTR_CFG);
	/* Set PCI-AHB Window1 address */
	rcar_pci_writel(rcar_pci, PCI_BASE_ADDRESS_1,
			rcar_pci->window_pci | PCI_BASE_ADDRESS_MEM_PREFETCH);
	/* Set AHB-PCI bridge PCI communication area address */
	val = rcar_pci->cfg_res->start + RCAR_AHBPCI_PCICOM_OFFSET;
	rcar_pci_writel(rcar_pci, PCI_BASE_ADDRESS_0, val);

	val = rcar_pci_readl(rcar_pci, PCI_COMMAND);
	val |= PCI_COMMAND_SERR | PCI_COMMAND_PARITY |
	       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	rcar_pci_writel(rcar_pci, PCI_COMMAND, val);

	/* Enable PCI interrupts */
	rcar_pci_writel(rcar_pci, RCAR_PCI_INT_ENABLE_REG,
			RCAR_PCI_INT_A | RCAR_PCI_INT_B | RCAR_PCI_INT_PME);

	if (rcar_pci->irq > 0)
		rcar_pci_setup_errirq(rcar_pci);

	/* Add PCI resources */
	pci_add_resource(&sys->resources, &rcar_pci->mem_res);
	ret = devm_request_pci_bus_resources(dev, &sys->resources);
	if (ret < 0)
		return ret;

	/* Setup bus number based on platform device id / of bus-range */
	sys->busnr = rcar_pci->busnr;
	return 1;
}

static struct pci_ops rcar_pci_ops = {
	.map_bus = rcar_pci_cfg_base,
	.read	= pci_generic_config_read,
	.write	= pci_generic_config_write,
};

static int pci_dma_range_parser_init(struct of_pci_range_parser *parser,
				     struct device_node *node)
{
	const int na = 3, ns = 2;
	int rlen;

	parser->node = node;
	parser->pna = of_n_addr_cells(node);
	parser->np = parser->pna + na + ns;

	parser->range = of_get_property(node, "dma-ranges", &rlen);
	if (!parser->range)
		return -ENOENT;

	parser->end = parser->range + rlen / sizeof(__be32);
	return 0;
}

static int rcar_pci_parse_map_dma_ranges(struct rcar_pci *rcar_pci,
					 struct device_node *np)
{
	struct device *dev = rcar_pci->dev;
	struct of_pci_range range;
	struct of_pci_range_parser parser;
	int index = 0;

	/* Failure to parse is ok as we fall back to defaults */
	if (pci_dma_range_parser_init(&parser, np))
		return 0;

	/* Get the dma-ranges from DT */
	for_each_of_pci_range(&parser, &range) {
		/* Hardware only allows one inbound 32-bit range */
		if (index)
			return -EINVAL;

		rcar_pci->window_addr = (unsigned long)range.cpu_addr;
		rcar_pci->window_pci = (unsigned long)range.pci_addr;
		rcar_pci->window_size = (unsigned long)range.size;

		/* Catch HW limitations */
		if (!(range.flags & IORESOURCE_PREFETCH)) {
			dev_err(dev, "window must be prefetchable\n");
			return -EINVAL;
		}
		if (rcar_pci->window_addr) {
			u32 lowaddr = 1 << (ffs(rcar_pci->window_addr) - 1);

			if (lowaddr < rcar_pci->window_size) {
				dev_err(dev, "invalid window size/addr\n");
				return -EINVAL;
			}
		}
		index++;
	}

	return 0;
}

static int rcar_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *cfg_res, *mem_res;
	struct rcar_pci *rcar_pci;
	void __iomem *reg;
	struct hw_pci hw;
	void *hw_private[1];

	cfg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg = devm_ioremap_resource(dev, cfg_res);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!mem_res || !mem_res->start)
		return -ENODEV;

	if (mem_res->start & 0xFFFF)
		return -EINVAL;

	rcar_pci = devm_kzalloc(dev, sizeof(*rcar_pci), GFP_KERNEL);
	if (!rcar_pci)
		return -ENOMEM;

	rcar_pci->mem_res = *mem_res;
	rcar_pci->cfg_res = cfg_res;

	rcar_pci->irq = platform_get_irq(pdev, 0);
	rcar_pci->reg = reg;
	rcar_pci->dev = dev;

	if (rcar_pci->irq < 0) {
		dev_err(dev, "no valid irq found\n");
		return rcar_pci->irq;
	}

	/* default window addr and size if not specified in DT */
	rcar_pci->window_addr = 0x40000000;
	rcar_pci->window_pci = 0x40000000;
	rcar_pci->window_size = SZ_1G;

	if (dev->of_node) {
		struct resource busnr;
		int ret;

		ret = of_pci_parse_bus_range(dev->of_node, &busnr);
		if (ret < 0) {
			dev_err(dev, "failed to parse bus-range\n");
			return ret;
		}

		rcar_pci->busnr = busnr.start;
		if (busnr.end != busnr.start)
			dev_warn(dev, "only one bus number supported\n");

		ret = rcar_pci_parse_map_dma_ranges(rcar_pci, dev->of_node);
		if (ret < 0) {
			dev_err(dev, "failed to parse dma-range\n");
			return ret;
		}
	} else {
		rcar_pci->busnr = pdev->id;
	}

	hw_private[0] = rcar_pci;
	memset(&hw, 0, sizeof(hw));
	hw.nr_controllers = ARRAY_SIZE(hw_private);
	hw.io_optional = 1;
	hw.private_data = hw_private;
	hw.map_irq = rcar_pci_map_irq;
	hw.ops = &rcar_pci_ops;
	hw.setup = rcar_pci_setup;
	pci_common_init_dev(dev, &hw);
	return 0;
}

static struct of_device_id rcar_pci_of_match[] = {
	{ .compatible = "renesas,pci-rcar-gen2", },
	{ .compatible = "renesas,pci-r8a7790", },
	{ .compatible = "renesas,pci-r8a7791", },
	{ .compatible = "renesas,pci-r8a7794", },
	{ },
};

static struct platform_driver rcar_pci_driver = {
	.driver = {
		.name = "pci-rcar-gen2",
		.suppress_bind_attrs = true,
		.of_match_table = rcar_pci_of_match,
	},
	.probe = rcar_pci_probe,
};
builtin_platform_driver(rcar_pci_driver);
