/*
 * Copyright (C) 2014-2015 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/bootmem.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <asm/pci.h>

unsigned long pcibios_min_io = 0x100;
EXPORT_SYMBOL(pcibios_min_io);

unsigned long pcibios_min_mem = 0x100000;
EXPORT_SYMBOL(pcibios_min_mem);

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here
 */
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				resource_size_t size, resource_size_t align)
{
	return res->start;
}

/*
 * If the bus contains any of these devices, then we must not turn on
 * parity checking of any kind.  Currently this is CyberPro 20x0 only.
 */
static inline int pdev_bad_for_parity(struct pci_dev *dev)
{
	return ((dev->vendor == PCI_VENDOR_ID_INTERG &&
		 (dev->device == PCI_DEVICE_ID_INTERG_2000 ||
		  dev->device == PCI_DEVICE_ID_INTERG_2010)) ||
		(dev->vendor == PCI_VENDOR_ID_ITE &&
		 dev->device == PCI_DEVICE_ID_ITE_8152));

}

void pcibios_fixup_bus(struct pci_bus *bus)
{
}
EXPORT_SYMBOL(pcibios_fixup_bus);
