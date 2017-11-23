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
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "pcie-cadence.h"

/**
 * struct cdns_pcie_rc_data - hardware specific data
 * @max_regions: maximum number of regions supported by the hardware
 * @vendor_id: PCI vendor ID
 * @device_id: PCI device ID
 * @no_bar_nbits: Number of bits to keep for inbound (PCIe -> CPU) address
 *                translation (nbits sets into the "no BAR match" register).
 */
struct cdns_pcie_rc_data {
	size_t			max_regions;
	u16			vendor_id;
	u16			device_id;
	u8			no_bar_nbits;
};

/**
 * struct cdns_pcie_rc - private data for this PCIe Root Complex driver
 * @pcie: Cadence PCIe controller
 * @dev: pointer to PCIe device
 * @cfg_res: start/end offsets in the physical system memory to map PCI
 *           configuration space accesses
 * @bus_range: first/last buses behind the PCIe host controller
 * @cfg_base: IO mapped window to access the PCI configuration space of a
 *            single function at a time
 * @data: pointer to a 'struct cdns_pcie_rc_data'
 */
struct cdns_pcie_rc {
	struct cdns_pcie	pcie;
	struct device		*dev;
	struct resource		*cfg_res;
	struct resource		*bus_range;
	void __iomem		*cfg_base;
	const struct cdns_pcie_rc_data	*data;
};

static void __iomem *
cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	struct cdns_pcie *pcie = &rc->pcie;
	unsigned int busn = bus->number;
	u32 addr0, desc0;

	if (busn < rc->bus_range->start || busn > rc->bus_range->end)
		return NULL;

	if (busn == rc->bus_range->start) {
		if (devfn)
			return NULL;

		return pcie->reg_base + (where & 0xfff);
	}

	/* Update Output registers for AXI region 0. */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(busn);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(0), addr0);

	/* Configuration Type 0 or Type 1 access. */
	desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
		CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(0);
	/*
	 * The bus number was already set once for all in desc1 by
	 * cdns_pcie_host_init_address_translation().
	 */
	if (busn == rc->bus_range->start + 1)
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0;
	else
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(0), desc0);

	return rc->cfg_base + (where & 0xfff);
}

static struct pci_ops cdns_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

static const struct cdns_pcie_rc_data cdns_pcie_rc_data = {
	.max_regions	= 32,
	.vendor_id	= PCI_VENDOR_ID_CDNS,
	.device_id	= 0x0200,
	.no_bar_nbits	= 32,
};

static const struct of_device_id cdns_pcie_host_of_match[] = {
	{ .compatible = "cdns,cdns-pcie-host",
	  .data = &cdns_pcie_rc_data },

	{ },
};

static int cdns_pcie_parse_request_of_pci_ranges(struct device *dev,
						 struct list_head *resources,
						 struct resource **bus_range)
{
	int err, res_valid = 0;
	struct device_node *np = dev->of_node;
	resource_size_t iobase;
	struct resource_entry *win, *tmp;

	err = of_pci_get_host_bridge_resources(np, 0, 0xff, resources, &iobase);
	if (err)
		return err;

	err = devm_request_pci_bus_resources(dev, resources);
	if (err)
		return err;

	resource_list_for_each_entry_safe(win, tmp, resources) {
		struct resource *res = win->res;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
			err = pci_remap_iospace(res, iobase);
			if (err) {
				dev_warn(dev, "error %d: failed to map resource %pR\n",
					 err, res);
				resource_list_destroy_entry(win);
			}
			break;
		case IORESOURCE_MEM:
			res_valid |= !(res->flags & IORESOURCE_PREFETCH);
			break;
		case IORESOURCE_BUS:
			*bus_range = res;
			break;
		}
	}

	if (res_valid)
		return 0;

	dev_err(dev, "non-prefetchable memory resource required\n");
	return -EINVAL;
}

static int cdns_pcie_host_init_root_port(struct cdns_pcie_rc *rc)
{
	const struct cdns_pcie_rc_data *data = rc->data;
	struct cdns_pcie *pcie = &rc->pcie;
	u8 pbn, sbn, subn;
	u32 value, ctrl;

	/*
	 * Set the root complex BAR configuration register:
	 * - disable both BAR0 and BAR1.
	 * - enable Prefetchable Memory Base and Limit registers in type 1
	 *   config space (64 bits).
	 * - enable IO Base and Limit registers in type 1 config
	 *   space (32 bits).
	 */
	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	value = CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS;
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	/* Set root port configuration space */
	if (data->vendor_id != 0xffff)
		cdns_pcie_rp_writew(pcie, PCI_VENDOR_ID, data->vendor_id);
	if (data->device_id != 0xffff)
		cdns_pcie_rp_writew(pcie, PCI_DEVICE_ID, data->device_id);

	cdns_pcie_rp_writeb(pcie, PCI_CLASS_REVISION, 0);
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_PROG, 0);
	cdns_pcie_rp_writew(pcie, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);

	pbn = rc->bus_range->start;
	sbn = pbn + 1; /* Single root port. */
	subn = rc->bus_range->end;
	cdns_pcie_rp_writeb(pcie, PCI_PRIMARY_BUS, pbn);
	cdns_pcie_rp_writeb(pcie, PCI_SECONDARY_BUS, sbn);
	cdns_pcie_rp_writeb(pcie, PCI_SUBORDINATE_BUS, subn);

	return 0;
}

static int cdns_pcie_host_init_address_translation(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct resource *cfg_res = rc->cfg_res;
	struct resource *mem_res = pcie->mem_res;
	struct resource *bus_range = rc->bus_range;
	struct device *dev = rc->dev;
	struct device_node *np = dev->of_node;
	struct of_pci_range_parser parser;
	struct of_pci_range range;
	u32 addr0, addr1, desc1;
	u64 cpu_addr;
	int r, err;

	/*
	 * Reserve region 0 for PCI configure space accesses:
	 * OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
	 * cdns_pci_map_bus(), other region registers are set here once for all.
	 */
	addr1 = 0; /* Should be programmed to zero. */
	desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(bus_range->start);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(0), addr1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), desc1);

	cpu_addr = cfg_res->start - mem_res->start;
	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(0), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(0), addr1);

	err = of_pci_range_parser_init(&parser, np);
	if (err)
		return err;

	r = 1;
	for_each_of_pci_range(&parser, &range) {
		bool is_io;

		if (r >= rc->data->max_regions)
			break;

		if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_MEM)
			is_io = false;
		else if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_IO)
			is_io = true;
		else
			continue;

		cdns_pcie_set_outbound_region(pcie, r, is_io,
					      range.cpu_addr,
					      range.pci_addr,
					      range.size);
		r++;
	}

	/*
	 * Set Root Port no BAR match Inbound Translation registers:
	 * needed for MSI.
	 * Root Port BAR0 and BAR1 are disabled, hence no need to set their
	 * inbound translation registers.
	 */
	addr0 = CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(rc->data->no_bar_nbits);
	addr1 = 0;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR0(RP_NO_BAR), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR1(RP_NO_BAR), addr1);

	return 0;
}

static int cdns_pcie_host_init(struct device *dev,
			       struct list_head *resources,
			       struct cdns_pcie_rc *rc)
{
	struct resource *bus_range = NULL;
	int err;

	/* Parse our PCI ranges and request their resources */
	err = cdns_pcie_parse_request_of_pci_ranges(dev, resources, &bus_range);
	if (err)
		goto err_out;

	if (bus_range->start > bus_range->end) {
		err = -EINVAL;
		goto err_out;
	}
	rc->bus_range = bus_range;
	rc->pcie.bus = bus_range->start;

	err = cdns_pcie_host_init_root_port(rc);
	if (err)
		goto err_out;

	err = cdns_pcie_host_init_address_translation(rc);
	if (err)
		goto err_out;

	return 0;

 err_out:
	pci_free_resource_list(resources);
	return err;
}

static int cdns_pcie_host_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id;
	const char *type;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pci_bus *bus, *child;
	struct pci_host_bridge *bridge;
	struct list_head resources;
	struct cdns_pcie_rc *rc;
	struct cdns_pcie *pcie;
	struct resource *res;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;

	rc = pci_host_bridge_priv(bridge);
	rc->dev = dev;
	platform_set_drvdata(pdev, rc);

	pcie = &rc->pcie;
	pcie->is_rc = true;

	of_id = of_match_node(cdns_pcie_host_of_match, np);
	rc->data = (const struct cdns_pcie_rc_data *)of_id->data;

	type = of_get_property(np, "device_type", NULL);
	if (!type || strcmp(type, "pci")) {
		dev_err(dev, "invalid \"device_type\" %s\n", type);
		return -EINVAL;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg");
	pcie->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->reg_base)) {
		dev_err(dev, "missing \"reg\"\n");
		return PTR_ERR(pcie->reg_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	rc->cfg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(rc->cfg_base)) {
		dev_err(dev, "missing \"cfg\"\n");
		return PTR_ERR(rc->cfg_base);
	}
	rc->cfg_res = res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mem");
	if (!res) {
		dev_err(dev, "missing \"mem\"\n");
		return -EINVAL;
	}
	pcie->mem_res = res;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync() failed\n");
		goto err_get_sync;
	}

	INIT_LIST_HEAD(&resources);
	ret = cdns_pcie_host_init(dev, &resources, rc);
	if (ret)
		goto err_init;

	list_splice_init(&resources, &bridge->windows);
	bridge->dev.parent = dev;
	bridge->busnr = pcie->bus;
	bridge->ops = &cdns_pcie_host_ops;
	bridge->map_irq = of_irq_parse_and_map_pci;
	bridge->swizzle_irq = pci_common_swizzle;

	ret = pci_scan_root_bus_bridge(bridge);
	if (ret < 0) {
		dev_err(dev, "Scanning root bridge failed");
		goto err_init;
	}

	bus = bridge->bus;
	pci_bus_size_bridges(bus);
	pci_bus_assign_resources(bus);

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	pci_bus_add_devices(bus);

	return 0;

 err_init:
	pm_runtime_put_sync(dev);

 err_get_sync:
	pm_runtime_disable(dev);

	return ret;
}

static struct platform_driver cdns_pcie_host_driver = {
	.driver = {
		.name = "cdns-pcie-host",
		.of_match_table = cdns_pcie_host_of_match,
	},
	.probe = cdns_pcie_host_probe,
};
builtin_platform_driver(cdns_pcie_host_driver);
