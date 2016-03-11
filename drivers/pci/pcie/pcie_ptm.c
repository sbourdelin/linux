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

#define PCI_PTM_REQ		0x0001  /* Requester capable */
#define  PCI_PTM_RSP		0x0002  /* Responder capable */
#define  PCI_PTM_ROOT		0x0004  /* Root capable */
#define  PCI_PTM_GRANULITY	0xFF00  /* Local clock granulity */
#define PCI_PTM_ENABLE		0x0001  /* PTM enable */
#define  PCI_PTM_ROOT_SEL	0x0002  /* Root select */

#define PCI_PTM_HEADER_REG_OFFSET	0x00
#define PCI_PTM_CAPABILITY_REG_OFFSET	0x04
#define PCI_PTM_CONTROL_REG_OFFSET	0x08

#define PCI_EXT_CAP_ID_PTM		0x001f

#ifdef CONFIG_PCIE_PTM
static bool disable_ptm;
#else
static bool disable_ptm = 1;
#endif

module_param_named(disable_ptm, disable_ptm, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(disable_ptm,
	"Don't automatically enable even if supported.");

static inline u8 get_granularity(u32 in)
{
	return (in & PCI_PTM_GRANULITY) >> 8;
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
	return sprintf(buf, "%u\n", word & PCI_PTM_ENABLE);
}

static ssize_t ptm_status_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	unsigned long val;
	ssize_t ret;
	int pos;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PTM);
	if (!pos)
		return -ENXIO;

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

/**
 * pci_enable_ptm - Try to activate PTM functionality on device.
 * @dev: PCI Express device with PTM requester role to enable.
 *
 * The function will crawl through the PCI Hierarchy to determine if it is
 * possible to enable the Precision Time Measurement requester role on @dev,
 * and if so, activate it by setting the granularity field.
 *
 * NOTE: Each requester must be associated with a PTM root (not to be confused
 * with a root port or root complex). There can be multiple PTM roots in a
 * a system forming multiple domains. All intervening bridges/switches in a
 * domain must support PTM responder roles to relay PTM dialogues.
 */
int pci_enable_ptm(struct pci_dev *dev)
{
	struct pci_dev *curr, **steps;
	size_t i = 0, root = 0;
	int pos, pos2;
	u8 granularity = 0;
	u16 word;
	int ret = 0;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	if (!pos) {
		dev_dbg(&dev->dev, "Not PTM capable, skipping.\n");
		return -ENXIO;
	}

	if (disable_ptm)
		return 0;

	/* Skip if not requester role. */
	pci_read_config_word(dev, pos + PCI_PTM_CAPABILITY_REG_OFFSET, &word);
	if (!(word & PCI_PTM_REQ)) {
		dev_dbg(&dev->dev, "Not a PTM requester, skipping for now.\n");
		return 0;
	}

	/* Just copy and enable PTM granularity for integrated endpoints. */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
		dev_dbg(&dev->dev,
			"Root integrated endpoint, attempting to copy root granularity.\n");
		curr = pci_upstream_bridge(dev);
		if (!curr)
			return 0;

		pos2 = pci_find_ext_capability(curr, PCI_EXT_CAP_ID_PTM);
		if (!pos2)
			return 0;

		/* Get granularity field. */
		pci_read_config_word(curr,
			pos2 + PCI_PTM_CAPABILITY_REG_OFFSET,
			&word);
		word &= PCI_PTM_GRANULITY;

		/* Copy it over. */
		word |= PCI_PTM_ENABLE;
		pci_write_config_word(dev, pos + PCI_PTM_CONTROL_REG_OFFSET,
			word);

		/* Enable PTM on root complex if not already so. */
		pci_read_config_word(curr, pos2 + PCI_PTM_CONTROL_REG_OFFSET,
			&word);
		if (!(word & PCI_PTM_ENABLE)) {
			pci_read_config_word(curr,
				pos2 + PCI_PTM_CONTROL_REG_OFFSET, &word);
			word |= PCI_PTM_ENABLE | PCI_PTM_ROOT_SEL;
			pci_write_config_word(curr,
				pos2 + PCI_PTM_CONTROL_REG_OFFSET, word);
		}
	} else {
		/* For holding all the bridge/switches in between. */
		steps = kzalloc(sizeof(*steps) * dev->bus->number + 1,
			GFP_KERNEL);
		if (!steps)
			return -ENOMEM;

		/* Gather all the upstream devices. */
		curr = pci_upstream_bridge(dev);
		if (curr) {
			dev_dbg(&dev->dev,
				"Upstream is %d:%d:%x.%d\n",
				pci_domain_nr(curr->bus),
				curr->bus->number,
				PCI_SLOT(curr->devfn),
				PCI_FUNC(curr->devfn)
				);
		} else {
			dev_dbg(&dev->dev, "No upstream??\n");
			ret = -ENXIO;
			goto abort;
		}

		do {
			/* sanity check */
			if (i > dev->bus->number)
				break;
			steps[i++] = curr;
		} while ((curr = pci_upstream_bridge(curr)));

		/* Check upstream chains capability. */
		dev_dbg(&dev->dev,
				   "Checking hierarchy capabilities\n");
		for (i = 0; i < dev->bus->number + 1; i++) {
			curr = steps[i];
			if (!curr)
				break;

			pos2 = pci_find_ext_capability(curr,
				PCI_EXT_CAP_ID_PTM);

			if (!pos2) {
				dev_dbg(&curr->dev,
					"PTM Hierarchy %zx: not PTM aware\n",
					i);
				break;
			}

			/* End if upstream cannot respond. */
			pci_read_config_word(curr,
				pos2 + PCI_PTM_CAPABILITY_REG_OFFSET, &word);
			if (!(word & PCI_PTM_RSP)) {
				dev_dbg(&curr->dev,
				"PTM Hierarchy: skipping non-responder\n");
				break;
			}

			/* Is root capable? */
			if (word & PCI_PTM_ROOT) {
				root = i;
				granularity = get_granularity(word);
			}
		}

		if (!steps[root]) {
			dev_dbg(&dev->dev, "Cannot find root, aborting\n");
			ret = -ENXIO;
			goto abort;
		}

		dev_dbg(&dev->dev,
			"Found PTM root at %d:%d:%x.%d granularity %u\n",
			pci_domain_nr(steps[root]->bus),
			steps[root]->bus->number,
			PCI_SLOT(steps[root]->devfn),
			PCI_FUNC(steps[root]->devfn),
			granularity);

		/* Program granularity field. */
		for (i = root;;) {
			curr = steps[i];
			pos2 = pci_find_ext_capability(curr,
				PCI_EXT_CAP_ID_PTM);
			pci_read_config_word(curr,
				pos2 + PCI_PTM_CONTROL_REG_OFFSET, &word);

			/* If not yet PTM enabled. */
			if (!(word & PCI_PTM_ENABLE)) {
				pci_read_config_word(curr,
					pos2 + PCI_PTM_CAPABILITY_REG_OFFSET,
					&word);
				/* If requester capable, program granularity. */
				if (word & PCI_PTM_REQ) {
					dev_dbg(&curr->dev,
						"Programming granularity %u\n",
						granularity);
					pci_write_config_word(curr,
						pos2 +
						PCI_PTM_CONTROL_REG_OFFSET,
						((u16)granularity) << 8);
				}
				if ((word & PCI_PTM_ROOT) &&
					granularity != 0 &&
					((granularity < get_granularity(word))
					|| get_granularity(word) == 0)) {
					dev_dbg(&curr->dev,
					"Updating granularity %u to %u\n",
						granularity,
						get_granularity(word));
					granularity = get_granularity(word);
				}
			}
			if (!i)
				break;
			i--;
		}

		/* Program current device granularity and enable it. */
		pci_read_config_word(dev, pos + PCI_PTM_CONTROL_REG_OFFSET,
			&word);
		word = (word & ~PCI_PTM_GRANULITY) | ((u16)granularity) << 8
			| PCI_PTM_ENABLE;
		pci_write_config_word(dev, pos + PCI_PTM_CONTROL_REG_OFFSET,
			word);
		dev_dbg(&dev->dev,
				"Using granularity %u, %x\n", granularity,
				word);

		/* Enable PTM root. */
		pos2 = pci_find_ext_capability(steps[root],
			PCI_EXT_CAP_ID_PTM);
		pci_read_config_word(steps[root],
			pos2 + PCI_PTM_CONTROL_REG_OFFSET, &word);
		word |= PCI_PTM_ROOT_SEL | PCI_PTM_ENABLE;
		pci_write_config_word(steps[root],
			pos2 + PCI_PTM_CONTROL_REG_OFFSET, word);

		/* PTM enable from the bottom up. */
		for (i = 0; i <= root; i++) {
			pos2 = pci_find_ext_capability(steps[i],
				PCI_EXT_CAP_ID_PTM);
			pci_read_config_word(steps[i],
				pos2 + PCI_PTM_CONTROL_REG_OFFSET, &word);
			word |= PCI_PTM_ENABLE;
			pci_write_config_word(steps[i],
				pos2 + PCI_PTM_CONTROL_REG_OFFSET,
				word);
		}
abort:
		kfree(steps);
	}
	return ret;
}

static int do_disable_ptm(struct pci_dev *dev, void *v)
{
	int pos;
	u16 word;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	if (!pos)
		return 0;

	pci_read_config_word(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, &word);
	word &= ~PCI_PTM_ENABLE;
	pci_write_config_word(dev, pos + PCI_PTM_CONTROL_REG_OFFSET, word);
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
