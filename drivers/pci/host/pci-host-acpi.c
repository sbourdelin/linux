/*
 * Generic PCI host controller driver for ACPI based systems
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (c) 2015 Broadcom Corporation
 *
 * Based on drivers/pci/host/pci-host-generic.c
 * Copyright (C) 2014 ARM Limited
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include <linux/sfi_acpi.h>
#include <linux/slab.h>

#define PREFIX			"pci-host-acpi:"
#define MCFG_NAMELEN		32
#define MCFG_SHIFT		20

/* ECFG window for this root bus */
struct gen_mcfg_window {
	struct resource		res;
	unsigned int		domain_nr;
	unsigned int		bus_start;
	unsigned int		bus_end;
	char			name[MCFG_NAMELEN];
	void __iomem		*win;
};

/* sysdata pointer is ->root_info */
struct gen_acpi_pci {
	struct acpi_pci_root_info	root_info;
	struct gen_mcfg_window		cfg;
};

/* MCFG entries  */
struct mcfg_entry {
	int			segment;
	int			bus_start;
	int			bus_end;
	u64			addr;
};

static struct mcfg_entries {
	int			size;
	struct mcfg_entry	*entries;
} mcfg_sav;

/* find mapping of a MCFG area */
static void __iomem *gen_acpi_map_cfg_bus(struct pci_bus *bus,
			unsigned int devfn, int where)
{
	struct gen_acpi_pci *pci = bus->sysdata;
	struct gen_mcfg_window *cfg = &pci->cfg;

	if (bus->number < cfg->bus_start || bus->number > cfg->bus_end)
		return NULL;

	return cfg->win + ((bus->number - cfg->bus_start) << MCFG_SHIFT) +
			((devfn << 12) | where);
}

/* Map the ECFG area for a root bus */
static int gen_acpi_pci_map_mcfg(struct acpi_pci_root *root,
				struct gen_acpi_pci *pci)
{
	struct gen_mcfg_window *cfg = &pci->cfg;
	struct acpi_device *device = root->device;
	void __iomem  *vaddr;

	/* if there is info from _CBA, use that, otherwise use MCFG table */
	if (root->mcfg_addr) {
		cfg->bus_start = root->secondary.start;
		cfg->bus_end = root->secondary.end;
		cfg->res.start = root->mcfg_addr;
	} else {
		struct mcfg_entry *e = mcfg_sav.entries;
		int i, n = mcfg_sav.size;

		for (i = 0; i < n; i++, e++)
			if (e->segment == root->segment)
				break;
		if (i >= n)
			return -ENODEV;
		cfg->bus_start = e->bus_start;
		cfg->bus_end = e->bus_end;
		cfg->res.start = e->addr;
	}

	cfg->res.flags = IORESOURCE_MEM;
	cfg->res.name = cfg->name;
	cfg->res.end = cfg->res.start +
			((cfg->bus_end - cfg->bus_start + 1) << MCFG_SHIFT) - 1;
	snprintf(cfg->name, MCFG_NAMELEN, "PCI MMCONFIG %04x [bus %02x-%02x]",
		 root->segment, cfg->bus_start, cfg->bus_end);

	/* map ECFG space for the bus range */
	vaddr = devm_ioremap_resource(&device->dev, &cfg->res);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);

	cfg->win = vaddr;
	return 0;
}

static struct pci_ops gen_acpi_pci_ops = {
	.map_bus	= gen_acpi_map_cfg_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

static struct acpi_pci_root_ops pci_acpi_root_ops = {
	.pci_ops = &gen_acpi_pci_ops,
};

struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_device *device = root->device;
	struct gen_acpi_pci *pci;
	struct pci_bus *bus, *child;
	int err;

	pci = devm_kzalloc(&device->dev, sizeof(*pci), GFP_KERNEL);
	if (!pci) {
		dev_err(&device->dev,
			"pci_bus %04x:%02x: ignored (out of memory)\n",
			root->segment, (int)root->secondary.start);
		return NULL;
	}

	err = gen_acpi_pci_map_mcfg(root, pci);
	if (err) {
		dev_err(&device->dev, "MCFG lookup for domain %d failed",
			root->segment);
		return NULL;
	}
	bus =  acpi_pci_root_create(root, &pci_acpi_root_ops,
					&pci->root_info, pci);
	if (!bus) {
		dev_err(&device->dev, "Scanning rootbus failed");
		return NULL;
	}

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	pci_bus_add_devices(bus);
	return bus;
}

/* save MCFG entries */
static __init int handle_mcfg(struct acpi_table_header *header)
{
	struct acpi_table_mcfg *mcfg;
	struct acpi_mcfg_allocation *mptr;
	struct mcfg_entry *e;
	int i, n;

	if (!header)
		return -EINVAL;

	mcfg = (struct acpi_table_mcfg *)header;
	mptr = (struct acpi_mcfg_allocation *) &mcfg[1];
	n = (header->length - sizeof(*mcfg)) / sizeof(*mptr);
	if (n <= 0 || n > 255) {
		pr_err(PREFIX " MCFG has incorrect entries (%d).\n", n);
		return -EINVAL;
	}
	mcfg_sav.entries = e = kcalloc(n, sizeof(*e), GFP_KERNEL);
	if (e == NULL)
		return -ENOMEM;
	mcfg_sav.size = n;
	for (i = 0; i < n; i++, mptr++) {
		e->segment = mptr->pci_segment;
		e->bus_start = mptr->start_bus_number;
		e->bus_end = mptr->end_bus_number;
		e->addr = mptr->address;
	}
	return 0;
}

static __init int parse_save_mcfg(void)
{
	int err;

	err = acpi_sfi_table_parse(ACPI_SIG_MCFG, handle_mcfg);
	if (err) {
		pr_err(PREFIX " Failed to parse MCFG (%d)\n", err);
		mcfg_sav.size = -1;
	} else {
		pr_info(PREFIX " MCFG table at %p, %d entries.\n",
			mcfg_sav.entries, mcfg_sav.size);
	}
	return err;
}

arch_initcall(parse_save_mcfg);
