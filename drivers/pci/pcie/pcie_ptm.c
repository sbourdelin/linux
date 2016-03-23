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
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include "../pci.h"

int pci_enable_ptm(struct pci_dev *dev);
void pci_release_ptm(struct pci_dev *dev);

#ifdef CONFIG_PCIE_PTM
static bool disable_ptm;
#else
static bool disable_ptm = 1;
#endif

module_param_named(disable_ptm, disable_ptm, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(disable_ptm, "Don't automatically enable PCIe PTM even if supported.");

static int ptm_commit(struct pci_dev *dev)
{
	u32 dword;
	int pos;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);

	/* Is this even possible? */
	if (!pos)
		return -ENXIO;

	pci_read_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, &dword);
	dword = dev->is_ptm_enabled ? dword | PCI_PTM_CTRL_ENABLE :
		dword & ~PCI_PTM_CTRL_ENABLE;
	dword = dev->is_ptm_root ? dword | PCI_PTM_CTRL_ROOT :
		dword & ~PCI_PTM_CTRL_ROOT;

	/* Only requester should have it set */
	if (dev->is_ptm_requester)
		dword = dword | (((u32)dev->ptm_effective_granularity) << 8);
	return pci_write_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, dword);
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
	int type;
	struct pci_dev *upstream;

	upstream = pci_upstream_bridge(dev);
	type = pci_pcie_type(dev);

	if (dev->is_ptm_root_capable)
	{
		/* If we are root capable but already part of a chain, don't set
		 * the root select bit, only enable PTM */
		if (!upstream || !upstream->is_ptm_enabled)
			dev->is_ptm_root = 1;
		dev->is_ptm_enabled = 1;
	}

	/* Is possible to be part of the PTM chain */
	if (dev->is_ptm_responder && upstream && upstream->is_ptm_enabled)
		dev->is_ptm_enabled = 1;

	if (dev->is_ptm_requester && upstream && upstream->is_ptm_enabled) {
		dev->is_ptm_enabled = 1;
		dev->ptm_effective_granularity =
			upstream->ptm_clock_granularity;
	}
	return ptm_commit(dev);
}

void pci_ptm_init(struct pci_dev *dev)
{
	u32 dword;
	int pos;
	u8 ver;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	if (!pos)
		return;

	/* Check capability version */
	pci_read_config_dword(dev, pos, &dword);
	ver = PCI_EXT_CAP_VER(dword);
	if (ver != 0x1)
	{
		dev_warn(&dev->dev, "Expected PTM v1, got %u\n", ver);
		return;
	}

	/* Fill in caps, masters are implied to be responders as well */
	pci_read_config_dword(dev, pos + PCI_PTM_CAPABILITY_REG_OFFSET, &dword);
	dev->is_ptm_capable = 1;
	dev->is_ptm_root_capable   = (dword & PCI_PTM_CAP_ROOT) ? 1 : 0;
	dev->is_ptm_responder      = (dword & PCI_PTM_CAP_RSP) ? 1 : 0;
	dev->is_ptm_requester      = (dword & PCI_PTM_CAP_REQ) ? 1 : 0;
	dev->ptm_clock_granularity = dev->is_ptm_responder ?
		((dword & PCI_PTM_GRANULARITY_MASK) >> 8) : 0;
	dev_info(&dev->dev, "Found PTM %s type device with %uns clock\n",
		dev->is_ptm_root_capable ? "root" :
		dev->is_ptm_responder ? "responder" :
		dev->is_ptm_requester ? "requester" : "unknown",
		dev->ptm_clock_granularity);

	/* Get existing settings */
	pci_read_config_dword(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, &dword);
	dev->is_ptm_enabled            = (dword & PCI_PTM_CTRL_ENABLE) ? 1 : 0;
	dev->is_ptm_root               = (dword & PCI_PTM_CTRL_ROOT) ? 1 : 0;
	dev->ptm_effective_granularity =
		(dword & PCI_PTM_GRANULARITY_MASK) >> 8;

	if(!disable_ptm)
		pci_enable_ptm(dev);
}

static int do_disable_ptm(struct pci_dev *dev, void *v)
{
	if (dev->is_ptm_enabled)
	{
		dev->is_ptm_enabled            = 0;
		dev->is_ptm_root               = 0;
		dev->ptm_effective_granularity = 0;
		ptm_commit(dev);
	}
	return 0;
}

/**
 * pci_disable_ptm - Turn off PTM functionality on device.
 * @dev: PCI Express device with PTM function to disable.
 *
 * Disables PTM functionality by clearing the PTM enable bit, if device is a
 * switch/bridge it will also disable PTM function on other devices behind it.
 */
void pci_disable_ptm(struct pci_dev *dev)
{
	if (pci_is_bridge(dev))
		pci_walk_bus(dev->bus, &do_disable_ptm, NULL);
	else
		do_disable_ptm(dev, NULL);
}

static ssize_t ptm_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	u16 word;
	int pos;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PTM);
	if (!pos)
		return -ENXIO;

	pci_read_config_word(pdev, pos + PCI_PTM_CONTROL_REG_OFFSET, &word);
	return sprintf(buf, "%u\n", word & PCI_PTM_CTRL_ENABLE ? 1 : 0);
}

static ssize_t ptm_status_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	unsigned long val;
	ssize_t ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	if (val)
		return pci_enable_ptm(pdev);
	pci_disable_ptm(pdev);
	return 0;
}

static DEVICE_ATTR_RW(ptm_status);

void pci_release_ptm_sysfs(struct pci_dev *dev)
{
	if (!pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM))
		return;
	device_remove_file(&dev->dev, &dev_attr_ptm_status);
}

void pci_create_ptm_sysfs(struct pci_dev *dev)
{
	if (!pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM))
		return;
	device_create_file(&dev->dev, &dev_attr_ptm_status);
}
