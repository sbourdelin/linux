/*
 * Cadence PCIe host controller driver.
 *
 * Copyright (c) 2017 Cadence
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/pci-epc.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/delay.h>

#include "pcie-cadence.h"

#define CDNS_PCIE_EP_MIN_APERTURE		128	/* 128 bytes */
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_NONE		0x1
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY	0x3

/**
 * struct cdns_pcie_ep_data - hardware specific data
 * @max_regions: maximum nmuber of regions supported by hardware
 */
struct cdns_pcie_ep_data {
	size_t				max_regions;
};

/**
 * struct cdns_pcie_ep - private data for this PCIe endpoint controller driver
 * @pcie: Cadence PCIe controller
 * @data: pointer to a 'struct cdns_pcie_data'
 */
struct cdns_pcie_ep {
	struct cdns_pcie		pcie;
	const struct cdns_pcie_ep_data	*data;
	struct pci_epc			*epc;
	unsigned long			ob_region_map;
	phys_addr_t			*ob_addr;
	phys_addr_t			irq_phys_addr;
	void __iomem			*irq_cpu_addr;
	u64				irq_pci_addr;
	u8				irq_pending;
};

static int cdns_pcie_ep_write_header(struct pci_epc *epc,
				     struct pci_epf_header *hdr)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u8 fn = 0;

	if (fn == 0) {
		u32 id = CDNS_PCIE_LM_ID_VENDOR(hdr->vendorid) |
			 CDNS_PCIE_LM_ID_SUBSYS(hdr->subsys_vendor_id);
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_DEVICE_ID, hdr->deviceid);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_REVISION_ID, hdr->revid);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CLASS_PROG, hdr->progif_code);
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_CLASS_DEVICE,
			       hdr->subclass_code | hdr->baseclass_code << 8);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CACHE_LINE_SIZE,
			       hdr->cache_line_size);
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_SUBSYSTEM_ID, hdr->subsys_id);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_INTERRUPT_PIN, hdr->interrupt_pin);

	return 0;
}

static int cdns_pcie_ep_set_bar(struct pci_epc *epc, enum pci_barno bar,
				dma_addr_t bar_phys, size_t size, int flags)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 addr0, addr1, reg, cfg, b, aperture, ctrl;
	u8 fn = 0;
	u64 sz;

	/* BAR size is 2^(aperture + 7) */
	sz = max_t(size_t, size, CDNS_PCIE_EP_MIN_APERTURE);
	sz = 1ULL << fls64(sz - 1);
	aperture = ilog2(sz) - 7; /* 128B -> 0, 256B -> 1, 512B -> 2, ... */

	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_IO_32BITS;
	} else {
		bool is_prefetch = !!(flags & PCI_BASE_ADDRESS_MEM_PREFETCH);
		bool is_64bits = sz > SZ_2G;

		if (is_64bits && (bar & 1))
			return -EINVAL;

		switch (is_64bits << 1 | is_prefetch) {
		case 0:
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_32BITS;
			break;

		case 1:
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS;
			break;

		case 2:
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_64BITS;
			break;

		case 3:
			ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS;
			break;
		}
	}

	addr0 = lower_32_bits(bar_phys);
	addr1 = upper_32_bits(bar_phys);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar),
			 addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar),
			 addr1);

	if (bar < BAR_4) {
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG0(fn);
		b = bar;
	} else {
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG1(fn);
		b = bar - BAR_4;
	}

	cfg = cdns_pcie_readl(pcie, reg);
	cfg &= ~(CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	cfg |= (CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE(b, aperture) |
		CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl));
	cdns_pcie_writel(pcie, reg, cfg);

	return 0;
}

static void cdns_pcie_ep_clear_bar(struct pci_epc *epc, enum pci_barno bar)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 reg, cfg, b, ctrl;
	u8 fn = 0;

	if (bar < BAR_4) {
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG0(fn);
		b = bar;
	} else {
		reg = CDNS_PCIE_LM_EP_FUNC_BAR_CFG1(fn);
		b = bar - BAR_4;
	}

	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	cfg = cdns_pcie_readl(pcie, reg);
	cfg &= ~(CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	cfg |= CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(ctrl);
	cdns_pcie_writel(pcie, reg, cfg);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar), 0);
}

static int cdns_pcie_ep_map_addr(struct pci_epc *epc, phys_addr_t addr,
				 u64 pci_addr, size_t size)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 r;

	r = find_first_zero_bit(&ep->ob_region_map, sizeof(ep->ob_region_map));
	if (r >= ep->data->max_regions - 1) {
		dev_err(&epc->dev, "no free outbound region\n");
		return -EINVAL;
	}

	cdns_pcie_set_outbound_region(pcie, r, false, addr, pci_addr, size);

	set_bit(r, &ep->ob_region_map);
	ep->ob_addr[r] = addr;

	return 0;
}

static void cdns_pcie_ep_unmap_addr(struct pci_epc *epc, phys_addr_t addr)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 r;

	for (r = 0; r < ep->data->max_regions - 1; r++)
		if (ep->ob_addr[r] == addr)
			break;

	if (r >= ep->data->max_regions - 1)
		return;

	cdns_pcie_reset_outbound_region(pcie, r);

	ep->ob_addr[r] = 0;
	clear_bit(r, &ep->ob_region_map);
}

static int cdns_pcie_ep_set_msi(struct pci_epc *epc, u8 mmc)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	u16 flags;
	u8 fn = 0;

	/* Validate the ID of the MSI Capability structure. */
	if (cdns_pcie_ep_fn_readb(pcie, fn, cap) != PCI_CAP_ID_MSI)
		return -EINVAL;

	/*
	 * Set the Multiple Message Capable bitfield into the Message Control
	 * register.
	 */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	flags = (flags & ~PCI_MSI_FLAGS_QMASK) | (mmc << 1);
	flags |= PCI_MSI_FLAGS_64BIT;
	flags &= ~PCI_MSI_FLAGS_MASKBIT;
	cdns_pcie_ep_fn_writew(pcie, fn, cap + PCI_MSI_FLAGS, flags);

	return 0;
}

static int cdns_pcie_ep_get_msi(struct pci_epc *epc)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	u16 flags, mmc, mme;
	u8 fn = 0;

	/* Validate the ID of the MSI Capability structure. */
	if (cdns_pcie_ep_fn_readb(pcie, fn, cap) != PCI_CAP_ID_MSI)
		return -EINVAL;

	/* Validate that the MSI feature is actually enabled. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/*
	 * Get the Multiple Message Enable bitfield from the Message Control
	 * register.
	 */
	mmc = (flags & PCI_MSI_FLAGS_QMASK) >> 1;
	mme = (flags & PCI_MSI_FLAGS_QSIZE) >> 4;
	if (mme > mmc)
		mme = mmc;
	if (mme > 5)
		mme = 5;

	return mme;
}

static void cdns_pcie_ep_assert_intx(struct cdns_pcie_ep *ep, u8 fn,
				     u8 intx, bool is_asserted)
{
	struct cdns_pcie *pcie = &ep->pcie;
	u32 r = ep->data->max_regions - 1;
	u32 offset;
	u16 status;
	u8 msg_code;

	intx &= 3;

	/* Set the outbound region if needed. */
	if (unlikely(ep->irq_pci_addr != CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY)) {
		/* Last region was reserved for IRQ writes. */
		cdns_pcie_set_outbound_region_for_normal_msg(pcie, r,
							     ep->irq_phys_addr);
		ep->irq_pci_addr = CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY;
	}

	if (is_asserted) {
		ep->irq_pending |= BIT(intx);
		msg_code = MSG_CODE_ASSERT_INTA + intx;
	} else {
		ep->irq_pending &= ~BIT(intx);
		msg_code = MSG_CODE_DEASSERT_INTA + intx;
	}

	status = cdns_pcie_ep_fn_readw(pcie, fn, PCI_STATUS);
	if (((status & PCI_STATUS_INTERRUPT) != 0) ^ (ep->irq_pending != 0)) {
		status ^= PCI_STATUS_INTERRUPT;
		cdns_pcie_ep_fn_writew(pcie, fn, PCI_STATUS, status);
	}

	offset = CDNS_PCIE_NORMAL_MSG_ROUTING(MSG_ROUTING_LOCAL) |
		 CDNS_PCIE_NORMAL_MSG_CODE(msg_code) |
		 CDNS_PCIE_MSG_NO_DATA;
	writel(0, ep->irq_cpu_addr + offset);
}

static int cdns_pcie_ep_send_legacy_irq(struct cdns_pcie_ep *ep, u8 fn, u8 intx)
{
	u16 cmd;

	cmd = cdns_pcie_ep_fn_readw(&ep->pcie, fn, PCI_COMMAND);
	if (cmd & PCI_COMMAND_INTX_DISABLE)
		return -EINVAL;

	cdns_pcie_ep_assert_intx(ep, fn, intx, true);
	mdelay(1);
	cdns_pcie_ep_assert_intx(ep, fn, intx, false);
	return 0;
}

static int cdns_pcie_ep_raise_irq(struct pci_epc *epc,
				  enum pci_epc_irq_type type, u8 interrupt_num)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	u16 flags, mmc, mme, data, data_mask;
	u8 msi_count;
	u64 pci_addr, pci_addr_mask = 0xff;
	u8 fn = 0;

	/* Handle legacy IRQ. */
	if (type == PCI_EPC_IRQ_LEGACY)
		return cdns_pcie_ep_send_legacy_irq(ep, fn, 0);

	/* Otherwise MSI. */
	if (type != PCI_EPC_IRQ_MSI)
		return -EINVAL;

	/* Check whether the MSI feature has been enabled by the PCI host. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/* Get the number of enabled MSIs */
	mmc = (flags & PCI_MSI_FLAGS_QMASK) >> 1;
	mme = (flags & PCI_MSI_FLAGS_QSIZE) >> 4;
	if (mme > mmc)
		mme = mmc;
	if (mme > 5)
		mme = 5;

	msi_count = 1 << mme;
	if (!interrupt_num || interrupt_num > msi_count)
		return -EINVAL;

	/* Compute the data value to be written. */
	data_mask = msi_count - 1;
	data = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_DATA_64);
	data = (data & ~data_mask) | ((interrupt_num - 1) & data_mask);

	/* Get the PCI address where to write the data into. */
	pci_addr = cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_HI);
	pci_addr <<= 32;
	pci_addr |= cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_LO);
	pci_addr &= GENMASK_ULL(63, 2);

	/* Set the outbound region if needed. */
	if (unlikely(ep->irq_pci_addr != pci_addr)) {
		/* Last region was reserved for IRQ writes. */
		cdns_pcie_set_outbound_region(pcie, ep->data->max_regions - 1,
					      false,
					      ep->irq_phys_addr,
					      pci_addr & ~pci_addr_mask,
					      pci_addr_mask + 1);
		ep->irq_pci_addr = pci_addr;
	}
	writew(data, ep->irq_cpu_addr + (pci_addr & pci_addr_mask));

	return 0;
}

static int cdns_pcie_ep_start(struct pci_epc *epc)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	struct pci_epf *epf;
	u32 cfg;
	u8 fn = 0;

	/* Enable this endpoint function. */
	cfg = cdns_pcie_readl(pcie, CDNS_PCIE_LM_EP_FUNC_CFG);
	cfg |= BIT(fn);
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_EP_FUNC_CFG, cfg);

	/*
	 * Already linked-up: don't call directly pci_epc_linkup() because we've
	 * already locked the epc->lock.
	 */
	list_for_each_entry(epf, &epc->pci_epf, list)
		pci_epf_linkup(epf);

	return 0;
}

static void cdns_pcie_ep_stop(struct pci_epc *epc)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cfg;
	u8 fn = 0;

	/* Disable this endpoint function (function 0 can't be disabled). */
	cfg = cdns_pcie_readl(pcie, CDNS_PCIE_LM_EP_FUNC_CFG);
	cfg &= ~BIT(fn);
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_EP_FUNC_CFG, cfg);
}

static const struct pci_epc_ops cdns_pcie_epc_ops = {
	.write_header	= cdns_pcie_ep_write_header,
	.set_bar	= cdns_pcie_ep_set_bar,
	.clear_bar	= cdns_pcie_ep_clear_bar,
	.map_addr	= cdns_pcie_ep_map_addr,
	.unmap_addr	= cdns_pcie_ep_unmap_addr,
	.set_msi	= cdns_pcie_ep_set_msi,
	.get_msi	= cdns_pcie_ep_get_msi,
	.raise_irq	= cdns_pcie_ep_raise_irq,
	.start		= cdns_pcie_ep_start,
	.stop		= cdns_pcie_ep_stop,
};

static const struct cdns_pcie_ep_data cdns_pcie_ep_data = {
	.max_regions	= 16,
};

static const struct of_device_id cdns_pcie_ep_of_match[] = {
	{ .compatible = "cdns,cdns-pcie-ep",
	  .data = &cdns_pcie_ep_data },

	{ },
};

static int cdns_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct of_device_id *of_id;
	struct cdns_pcie_ep *ep;
	struct cdns_pcie *pcie;
	struct pci_epc *epc;
	struct resource *res;
	size_t max_regions;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	platform_set_drvdata(pdev, ep);

	pcie = &ep->pcie;
	pcie->is_rc = false;

	of_id = of_match_node(cdns_pcie_ep_of_match, np);
	ep->data = (const struct cdns_pcie_ep_data *)of_id->data;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg");
	pcie->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->reg_base)) {
		dev_err(dev, "missing \"reg\"\n");
		return PTR_ERR(pcie->reg_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mem");
	if (!res) {
		dev_err(dev, "missing \"mem\"\n");
		return -EINVAL;
	}
	pcie->mem_res = res;

	max_regions = ep->data->max_regions;
	ep->ob_addr = devm_kzalloc(dev, max_regions * sizeof(*ep->ob_addr),
				   GFP_KERNEL);
	if (!ep->ob_addr)
		return -ENOMEM;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync() failed\n");
		goto err_get_sync;
	}

	/* Disable all but function 0 (anyway BIT(0) is hardwired to 1). */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_EP_FUNC_CFG, BIT(0));

	epc = devm_pci_epc_create(dev, &cdns_pcie_epc_ops);
	if (IS_ERR(epc)) {
		dev_err(dev, "failed to create epc device\n");
		ret = PTR_ERR(epc);
		goto err_init;
	}

	ep->epc = epc;
	epc_set_drvdata(epc, ep);

	ret = of_property_read_u8(np, "max-functions", &epc->max_functions);
	if (ret < 0)
		epc->max_functions = 1;

	ret = pci_epc_mem_init(epc, pcie->mem_res->start,
			       resource_size(pcie->mem_res));
	if (ret < 0) {
		dev_err(dev, "failed to initialize the memory space\n");
		goto err_init;
	}

	ep->irq_cpu_addr = pci_epc_mem_alloc_addr(epc, &ep->irq_phys_addr,
						  SZ_128K);
	if (!ep->irq_cpu_addr) {
		dev_err(dev, "failed to reserve memory space for MSI\n");
		ret = -ENOMEM;
		goto free_epc_mem;
	}
	ep->irq_pci_addr = CDNS_PCIE_EP_IRQ_PCI_ADDR_NONE;

	return 0;

 free_epc_mem:
	pci_epc_mem_exit(epc);

 err_init:
	pm_runtime_put_sync(dev);

 err_get_sync:
	pm_runtime_disable(dev);

	return ret;
}

static struct platform_driver cdns_pcie_ep_driver = {
	.driver = {
		.name = "cdns-pcie-ep",
		.of_match_table = cdns_pcie_ep_of_match,
	},
	.probe = cdns_pcie_ep_probe,
};
builtin_platform_driver(cdns_pcie_ep_driver);
