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

/* sysdata pointer is ->root_info */
struct gen_acpi_root_info {
	struct acpi_pci_root_info	common;
	struct pci_mmcfg_region		*mcfg;
	bool				mcfg_added;
};

/* find mapping of a MCFG area */
static void __iomem *gen_acpi_map_cfg_bus(struct pci_bus *bus,
			unsigned int devfn, int where)
{
	struct gen_acpi_root_info *pci = bus->sysdata;
	struct pci_mmcfg_region *mcfg = pci->mcfg;

	if (bus->number < mcfg->start_bus || bus->number > mcfg->end_bus)
		return NULL;

	return mcfg->virt +
		PCI_MMCFG_OFFSET(bus->number - mcfg->start_bus, devfn) +
		where;
}

static struct pci_ops gen_acpi_pci_ops = {
	.map_bus	= gen_acpi_map_cfg_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

/* Insert the ECFG area for a root bus */
static int pci_acpi_root_init_info(struct acpi_pci_root_info *ci)
{
	struct gen_acpi_root_info *info;
	struct acpi_pci_root *root = ci->root;
	struct device *dev = &ci->bridge->dev;
	int err;

	info = container_of(ci, struct gen_acpi_root_info, common);
	err = pci_mmconfig_insert(dev, root->segment, root->secondary.start,
			root->secondary.end, root->mcfg_addr);
	if (err && err != -EEXIST)
		return err;

	info->mcfg = pci_mmconfig_lookup(root->segment, root->secondary.start);
	WARN_ON(info->mcfg == NULL);
	info->mcfg_added = (err == -EEXIST);
	return 0;
}

static void pci_acpi_root_release_info(struct acpi_pci_root_info *ci)
{
	struct gen_acpi_root_info *info;
	struct acpi_pci_root *root = ci->root;

	info = container_of(ci, struct gen_acpi_root_info, common);
	if (info->mcfg_added)
		pci_mmconfig_delete(root->segment, root->secondary.start,
					root->secondary.end);
	info->mcfg = NULL;
}

static struct acpi_pci_root_ops pci_acpi_root_ops = {
	.pci_ops = &gen_acpi_pci_ops,
	.init_info = pci_acpi_root_init_info,
	.release_info = pci_acpi_root_release_info,
};

struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_device *device = root->device;
	struct gen_acpi_root_info *ri;
	struct pci_bus *bus, *child;

	/* allocate acpi_info/sysdata */
	ri = devm_kzalloc(&device->dev, sizeof(*ri), GFP_KERNEL);
	if (!ri) {
		dev_err(&device->dev,
			"pci_bus %04x:%02x: ignored (out of memory)\n",
			root->segment, (int)root->secondary.start);
		return NULL;
	}

	bus =  acpi_pci_root_create(root, &pci_acpi_root_ops,
					&ri->common, ri);
	if (!bus) {
		dev_err(&device->dev, "Scanning rootbus failed");
		return NULL;
	}

	pci_bus_size_bridges(bus);
	pci_bus_assign_resources(bus);
	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	return bus;
}

int raw_pci_read(unsigned int seg, unsigned int bus,
		  unsigned int devfn, int reg, int len, u32 *val)
{
	struct pci_mmcfg_region *mcfg;
	void __iomem *addr;
	int err = -EINVAL;

	rcu_read_lock();
	mcfg = pci_mmconfig_lookup(seg, bus);
	if (!mcfg || !mcfg->virt)
		goto err;

	addr = mcfg->virt + PCI_MMCFG_OFFSET(bus, devfn);
	switch (len) {
	case 1:
		*val = readb(addr + reg);
		break;
	case 2:
		*val = readw(addr + reg);
		break;
	case 4:
		*val = readl(addr + reg);
		break;
	}
	err = 0;
err:
	rcu_read_unlock();
	return err;
}

int raw_pci_write(unsigned int seg, unsigned int bus,
		unsigned int devfn, int reg, int len, u32 val)
{
	struct pci_mmcfg_region *mcfg;
	void __iomem *addr;
	int err = -EINVAL;

	rcu_read_lock();
	mcfg = pci_mmconfig_lookup(seg, bus);
	if (!mcfg || !mcfg->virt)
		goto err;

	addr = mcfg->virt + PCI_MMCFG_OFFSET(bus, devfn);
	switch (len) {
	case 1:
		writeb(val, addr + reg);
		break;
	case 2:
		writew(val, addr + reg);
		break;
	case 4:
		writel(val, addr + reg);
		break;
	}
	err = 0;
err:
	rcu_read_unlock();
	return err;
}
