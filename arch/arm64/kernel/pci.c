/*
 * Code borrowed from powerpc/kernel/pci-common.c
 *
 * Copyright (C) 2003 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci-acpi.h>
#include <linux/slab.h>

#include <asm/pci-bridge.h>

/*
 * Called after each bus is probed, but before its children are examined
 */
void pcibios_fixup_bus(struct pci_bus *bus)
{
	/* nothing to do, expected to be removed in the future */
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here
 */
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				resource_size_t size, resource_size_t align)
{
	return res->start;
}

/**
 * pcibios_enable_device - Enable I/O and memory.
 * @dev: PCI device to be enabled
 * @mask: bitmask of BARs to enable
 */
int pcibios_enable_device(struct pci_dev *dev, int mask)
{
	if (pci_has_flag(PCI_PROBE_ONLY))
		return 0;

#ifdef CONFIG_ACPI
	if (acpi_find_root_bridge_handle(dev))
		acpi_pci_irq_enable(dev);
#endif

	return pci_enable_resources(dev, mask);
}

void pcibios_disable_device(struct pci_dev *dev)
{
#ifdef CONFIG_ACPI
	if (acpi_find_root_bridge_handle(dev))
		acpi_pci_irq_disable(dev);
#endif
}

/*
 * Try to assign the IRQ number from DT when adding a new device
 */
int pcibios_add_device(struct pci_dev *dev)
{
	dev->irq = of_irq_parse_and_map_pci(dev, 0, 0);

	return 0;
}

/*
 * ACPI uses these - leave it to the generic ACPI PCI driver
 */
int __weak raw_pci_read(unsigned int domain, unsigned int bus,
		  unsigned int devfn, int reg, int len, u32 *val)
{
	return -ENXIO;
}

int __weak raw_pci_write(unsigned int domain, unsigned int bus,
		unsigned int devfn, int reg, int len, u32 val)
{
	return -ENXIO;
}

#ifdef CONFIG_ACPI

void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus);
}

void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus);
}

int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	/* ACPI root bus is created with NULL parent */
	if (!acpi_disabled && bridge->dev.parent == NULL) {
		struct pci_bus *b = bridge->bus;
		struct acpi_pci_root_info *ci = b->sysdata;

		ACPI_COMPANION_SET(&bridge->dev, ci->bridge);
		b->domain_nr = ci->root->segment;
	}
	return 0;
}

/*
 * Provide weak implementations of ACPI PCI hooks needed.
 * Leave it to the ACPI PCI driver implementation to do it
 */
struct pci_bus *__weak pci_acpi_scan_root(struct acpi_pci_root *root)
{
	return NULL;
}

void __init __weak pci_mmcfg_late_init(void)
{
}

static int __init pcibios_assign_resources(void)
{
	pci_assign_unassigned_resources();
	return 0;
}

fs_initcall(pcibios_assign_resources);

#endif
