/**
 * pci-designware-ep.c - Synopsys Designware PCIe Endpoint controller driver
 *
 * Copyright (C) 2016 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/of.h>

#include "pcie-designware.h"

static void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar)
{
	u32 reg;

	reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	dw_pcie_write_dbi(pci, pci->dbi_base2, reg, 0x4, 0x0);
	dw_pcie_write_dbi(pci, pci->dbi_base, reg, 0x4, 0x0);
}

static int dw_pcie_ep_write_header(struct pci_epc *epc,
				   struct pci_epf_header *hdr)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	void __iomem *base = pci->dbi_base;

	dw_pcie_write_dbi(pci, base, PCI_VENDOR_ID, 0x2, hdr->vendorid);
	dw_pcie_write_dbi(pci, base, PCI_DEVICE_ID, 0x2, hdr->deviceid);
	dw_pcie_write_dbi(pci, base, PCI_REVISION_ID, 0x1, hdr->revid);
	dw_pcie_write_dbi(pci, base, PCI_CLASS_PROG, 0x1, hdr->progif_code);
	dw_pcie_write_dbi(pci, base, PCI_CLASS_DEVICE, 0x2,
			  hdr->subclass_code | hdr->baseclass_code << 8);
	dw_pcie_write_dbi(pci, base, PCI_CACHE_LINE_SIZE, 0x1,
			  hdr->cache_line_size);
	dw_pcie_write_dbi(pci, base, PCI_SUBSYSTEM_VENDOR_ID, 0x2,
			  hdr->subsys_vendor_id);
	dw_pcie_write_dbi(pci, base, PCI_SUBSYSTEM_ID, 0x2, hdr->subsys_id);
	dw_pcie_write_dbi(pci, base, PCI_INTERRUPT_PIN, 0x1,
			  hdr->interrupt_pin);

	return 0;
}

static int dw_pcie_ep_inbound_atu(struct dw_pcie_ep *ep, enum pci_barno bar,
				  dma_addr_t cpu_addr,
				  enum dw_pcie_as_type as_type)
{
	int ret;
	u32 free_win;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	free_win = find_first_zero_bit(&ep->ib_window_map,
				       sizeof(ep->ib_window_map));
	if (free_win >= ep->num_ib_windows) {
		dev_err(pci->dev, "no free inbound window\n");
		return -EINVAL;
	}

	ret = dw_pcie_prog_inbound_atu(pci, free_win, bar, cpu_addr,
				       as_type);
	if (ret < 0) {
		dev_err(pci->dev, "Failed to program IB window\n");
		return ret;
	}

	ep->bar_to_atu[bar] = free_win;
	set_bit(free_win, &ep->ib_window_map);

	return 0;
}

static int dw_pcie_ep_set_bar(struct pci_epc *epc, enum pci_barno bar,
			      dma_addr_t bar_phys, size_t size, int flags)
{
	int ret;
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum dw_pcie_as_type as_type;
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);

	if (!(flags & PCI_BASE_ADDRESS_SPACE))
		as_type = DW_PCIE_AS_MEM;
	else
		as_type = DW_PCIE_AS_IO;

	ret = dw_pcie_ep_inbound_atu(ep, bar, bar_phys, as_type);
	if (ret)
		return ret;

	dw_pcie_write_dbi(pci, pci->dbi_base2, reg, 0x4, size - 1);
	dw_pcie_write_dbi(pci, pci->dbi_base, reg, 0x4, flags);

	return 0;
}

static void dw_pcie_ep_clear_bar(struct pci_epc *epc, enum pci_barno bar)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	void __iomem *base = pci->dbi_base;

	dw_pcie_ep_reset_bar(pci, bar);

	dw_pcie_write_dbi(pci, base, PCIE_ATU_VIEWPORT, 0x4,
			  PCIE_ATU_REGION_INBOUND | ep->bar_to_atu[bar]);
	dw_pcie_write_dbi(pci, base, PCIE_ATU_CR2, 0x4, ~PCIE_ATU_ENABLE);
}

static void *dw_pcie_ep_alloc_addr(struct pci_epc *epc, size_t size)
{
	/* TODO */
	return NULL;
}

static void dw_pcie_ep_free_addr(struct pci_epc *epc)
{
	/* TODO */
}

static int dw_pcie_ep_raise_irq(struct pci_epc *epc,
				enum pci_epc_irq_type type)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);

	if (!ep->ops->raise_irq)
		return -EINVAL;

	return ep->ops->raise_irq(ep, type);
}

static int dw_pcie_ep_start(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	if (!pci->ops->start_link)
		return -EINVAL;

	return pci->ops->start_link(pci);
}

static void dw_pcie_ep_stop(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	if (!pci->ops->stop_link)
		return;

	pci->ops->stop_link(pci);
}

void dw_pcie_ep_linkup(struct dw_pcie_ep *ep)
{
	struct pci_epc *epc = ep->epc;
	struct pci_epf *epf = epc->epf;

	pci_epf_linkup(epf);
}

const struct pci_epc_ops epc_ops = {
	.write_header		= dw_pcie_ep_write_header,
	.set_bar		= dw_pcie_ep_set_bar,
	.clear_bar		= dw_pcie_ep_clear_bar,
	.alloc_addr_space	= dw_pcie_ep_alloc_addr,
	.free_addr_space	= dw_pcie_ep_free_addr,
	.raise_irq		= dw_pcie_ep_raise_irq,
	.start			= dw_pcie_ep_start,
	.stop			= dw_pcie_ep_stop,
};

int dw_pcie_ep_init(struct dw_pcie_ep *ep)
{
	int ret;
	enum pci_barno bar;
	struct pci_epc *epc;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;
	struct device_node *np = dev->of_node;

	ret = of_property_read_u32(np, "num-ib-windows", &ep->num_ib_windows);
	if (ret < 0) {
		dev_err(dev, "unable to read *num-ib-windows* property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "num-ob-windows", &ep->num_ob_windows);
	if (ret < 0) {
		dev_err(dev, "unable to read *num-ob-windows* property\n");
		return ret;
	}

	for (bar = BAR_0; bar <= BAR_5; bar++)
		dw_pcie_ep_reset_bar(pci, bar);

	if (ep->ops->ep_init)
		ep->ops->ep_init(ep);

	epc = devm_pci_epc_create(dev, &epc_ops);
	if (IS_ERR(epc)) {
		dev_err(dev, "failed to create epc device\n");
		return PTR_ERR(epc);
	}

	ep->epc = epc;
	epc_set_drvdata(epc, ep);
	dw_pcie_setup(pci);

	return 0;
}

MODULE_DESCRIPTION("Designware PCIe endpoint controller driver");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_LICENSE("GPL v2");
