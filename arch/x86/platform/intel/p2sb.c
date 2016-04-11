/*
 * Primary to Sideband bridge (P2SB) driver
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * Authors: Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 *			Jonathan Yong <jonathan.yong@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>

#include <asm/p2sb.h>

#define SBREG_BAR	0x10
#define SBREG_HIDE	0xe1

static DEFINE_SPINLOCK(p2sb_spinlock);

/*
 * p2sb_bar - Get Primary to Sideband bridge (P2SB) BAR
 * @pdev:	PCI device to get PCI bus to communicate with
 * @devfn:	PCI device and function to communicate with
 * @res:	resources to be filled in
 *
 * The BIOS prevents the P2SB device from being enumerated by the PCI
 * subsystem, so we need to unhide and hide it back to lookup the P2SB BAR.
 *
 * Locking is handled by spinlock - cannot sleep.
 *
 * Return:
 * 0 on success or appropriate errno value on error.
 */
int p2sb_bar(struct pci_dev *pdev, unsigned int devfn,
	struct resource *res)
{
	u32 base_addr;
	u64 base64_addr;
	unsigned long flags;

	if (!res)
		return -EINVAL;

	spin_lock(&p2sb_spinlock);

	/* Unhide the P2SB device */
	pci_bus_write_config_byte(pdev->bus, devfn, SBREG_HIDE, 0x00);

	/* Check if device present */
	pci_bus_read_config_dword(pdev->bus, devfn, 0, &base_addr);
	if (base_addr == 0xffffffff || base_addr == 0x00000000) {
		spin_unlock(&p2sb_spinlock);
		dev_warn(&pdev->dev, "P2SB device access disabled by BIOS?\n");
		return -ENODEV;
	}

	/* Get IO or MMIO BAR */
	pci_bus_read_config_dword(pdev->bus, devfn, SBREG_BAR, &base_addr);
	if ((base_addr & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		flags = IORESOURCE_IO;
		base64_addr = base_addr & PCI_BASE_ADDRESS_IO_MASK;
	} else {
		flags = IORESOURCE_MEM;
		base64_addr = base_addr & PCI_BASE_ADDRESS_MEM_MASK;
		if (base_addr & PCI_BASE_ADDRESS_MEM_TYPE_64) {
			flags |= IORESOURCE_MEM_64;
			pci_bus_read_config_dword(pdev->bus, devfn,
				SBREG_BAR + 4, &base_addr);
			base64_addr |= (u64)base_addr << 32;
		}
	}

	/* Hide the P2SB device */
	pci_bus_write_config_byte(pdev->bus, devfn, SBREG_HIDE, 0x01);

	spin_unlock(&p2sb_spinlock);

	/* User provides prefilled resources */
	res->start += (resource_size_t)base64_addr;
	res->end += (resource_size_t)base64_addr;
	res->flags = flags;

	return 0;
}
EXPORT_SYMBOL(p2sb_bar);

MODULE_LICENSE("GPL");
