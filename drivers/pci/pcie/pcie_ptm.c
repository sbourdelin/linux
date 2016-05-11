/*
 * PCI Express Precision Time Measurement
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include "../pci.h"

static bool noptm;

static int ptm_commit(struct pci_dev *dev)
{
	u32 dword;
	int pos;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);

	pci_read_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, &dword);
	dword = dev->ptm_enabled ? dword | PCI_PTM_CTRL_ENABLE :
		dword & ~PCI_PTM_CTRL_ENABLE;
	dword = dev->ptm_root ? dword | PCI_PTM_CTRL_ROOT :
		dword & ~PCI_PTM_CTRL_ROOT;

	/* Only requester should have it set */
	if (dev->ptm_requester)
		dword = (dword & ~PCI_PTM_GRANULARITY_MASK) |
		(((u32)dev->ptm_effective_granularity) << 8);
	return pci_write_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET,
		dword);
}

/**
 * pci_enable_ptm - Try to activate PTM functionality on device.
 * @dev: PCI Express device with PTM requester role to enable.
 *
 * All PCIe Switches/Bridges in between need to be enabled for this to work.
 *
 * NOTE: Each requester must be associated with a PTM root (not to be confused
 * with a root port or root complex). There can be multiple PTM roots in a
 * a system forming multiple domains. All intervening bridges/switches in a
 * domain must support PTM responder roles to relay PTM dialogues.
 */
int pci_enable_ptm(struct pci_dev *dev)
{
	struct pci_dev *upstream;

	upstream = pci_upstream_bridge(dev);
	if (dev->ptm_root_capable) {
		/* If we are root capable but already part of a chain, don't set
		 * the root select bit, only enable PTM
		 */
		if (!upstream || !upstream->ptm_enabled)
			dev->ptm_root = 1;
		dev->ptm_enabled = 1;
	}

	/* Is possible to be part of the PTM chain? */
	if (dev->ptm_responder && upstream && upstream->ptm_enabled)
		dev->ptm_enabled = 1;

	if (dev->ptm_requester && upstream && upstream->ptm_enabled) {
		dev->ptm_enabled = 1;
		if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
			dev->ptm_effective_granularity =
				upstream->ptm_clock_granularity;
		} else {
			dev->ptm_effective_granularity =
				upstream->ptm_clock_implemented ?
				upstream->ptm_max_clock_granularity : 0;
		}
	}

	/* Did we have a condition to allow PTM? */
	if (!dev->ptm_enabled)
		return -ENXIO;

	return ptm_commit(dev);
}

static void pci_ptm_info(struct pci_dev *dev)
{
	dev_info(&dev->dev, "PTM %s type\n",
		dev->ptm_root_capable ? "root" :
		dev->ptm_responder ? "respond" :
		dev->ptm_requester ? "requester" :
		"unknown");
	switch (dev->ptm_clock_granularity) {
	case 0x00:
		dev_info(&dev->dev, "PTM clock unimplemented\n");
		break;
	case 0xff:
		dev_info(&dev->dev, "PTM clock greater than 254ns\n");
		break;
	default:
		dev_info(&dev->dev, "PTM clock %huns\n",
			dev->ptm_clock_granularity);
	}
}

static void set_slow_ptm(struct pci_dev *dev, u16 from, u16 to)
{
	dev_warn(&dev->dev, "One of the devices higher in the PTM domain has a clock granularity higher than the current device, using worst case time, %huns -> %huns\n",
		from, to);
	dev->ptm_max_clock_granularity = to;
}

void pci_ptm_init(struct pci_dev *dev)
{
	u32 dword;
	int pos;
	struct pci_dev *ups;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	if (!pos)
		return;

	ups = pci_upstream_bridge(dev);

	/* Fill in caps, masters are implied to be responders as well */
	pci_read_config_dword(dev, pos + PCI_PTM_CAPABILITY_REG_OFFSET, &dword);
	dev->ptm_capable = 1;
	dev->ptm_root_capable   = (dword & PCI_PTM_CAP_ROOT) ? 1 : 0;
	dev->ptm_responder      = (dword & PCI_PTM_CAP_RSP) ? 1 : 0;
	dev->ptm_requester      = (dword & PCI_PTM_CAP_REQ) ? 1 : 0;
	dev->ptm_clock_granularity = dev->ptm_responder ?
		((dword & PCI_PTM_GRANULARITY_MASK) >> 8) : 0;
	dev->ptm_clock_implemented = dev->ptm_clock_granularity ? 1 : 0;
	pci_ptm_info(dev);

	/* Get existing settings */
	pci_read_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, &dword);
	dev->ptm_enabled            = (dword & PCI_PTM_CTRL_ENABLE) ? 1 : 0;
	dev->ptm_root               = (dword & PCI_PTM_CTRL_ROOT) ? 1 : 0;
	dev->ptm_effective_granularity =
		(dword & PCI_PTM_GRANULARITY_MASK) >> 8;

	/* Find out the maximum clock granularity thus far */
	if (dev->ptm_responder) {
		dev->ptm_max_clock_granularity =
			dev->ptm_clock_granularity;
		if (ups && ups->ptm_clock_implemented) {
			if (ups->ptm_max_clock_granularity >
				dev->ptm_clock_granularity) {
				set_slow_ptm(dev,
					dev->ptm_clock_granularity,
					ups->ptm_max_clock_granularity
					);
			}
		}
	}

	if (!noptm)
		pci_enable_ptm(dev);
}

void pci_no_ptm(void)
{
	noptm = 1;
}
