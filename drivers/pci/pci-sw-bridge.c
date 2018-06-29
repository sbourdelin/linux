// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Marvell
 *
 * Author: Thomas Petazzoni <thomas.petazzoni@bootlin.com>
 *
 * This file helps PCI controller drivers implement a fake root port
 * PCI bridge when the HW doesn't provide such a root port PCI
 * bridge.
 *
 * It emulates a PCI bridge by providing a fake PCI configuration
 * space (and optionally a PCIe capability configuration space) in
 * memory. By default the read/write operations simply read and update
 * this fake configuration space in memory. However, PCI controller
 * drivers can provide through the 'struct pci_sw_bridge_ops'
 * structure a set of operations to override or complement this
 * default behavior.
 */

#include <linux/pci-sw-bridge.h>
#include <linux/pci.h>

#define PCI_BRIDGE_CONF_END	(PCI_BRIDGE_CONTROL + 2)
#define PCI_CAP_PCIE_START	PCI_BRIDGE_CONF_END
#define PCI_CAP_PCIE_END	(PCI_CAP_PCIE_START + PCI_EXP_SLTSTA2 + 2)

/*
 * Initialize a pci_sw_bridge structure to represent a fake PCI
 * bridge. The caller needs to have initialized the PCI configuration
 * space with whatever values make sense (typically at least vendor,
 * device, revision), the ->ops pointer, and possibly ->data and
 * ->has_pcie.
 */
void pci_sw_bridge_init(struct pci_sw_bridge *bridge)
{
	bridge->conf.class = PCI_CLASS_BRIDGE_PCI;
	bridge->conf.header_type = PCI_HEADER_TYPE_BRIDGE;
	bridge->conf.cache_line_size = 0x10;
	bridge->conf.status = PCI_STATUS_CAP_LIST;

	if (bridge->has_pcie) {
		bridge->conf.capabilities_pointer = PCI_CAP_PCIE_START;
		bridge->pcie_conf.cap_id = PCI_CAP_ID_EXP;
		/* Set PCIe v2, root port, slot support */
		bridge->pcie_conf.cap = PCI_EXP_TYPE_ROOT_PORT << 4 | 2 |
			PCI_EXP_FLAGS_SLOT;
	}
}

/*
 * Should be called by the PCI controller driver when reading the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->read_base or ->ops->read_pcie operations.
 */
int pci_sw_bridge_read(struct pci_sw_bridge *bridge, int where,
		       int size, u32 *value)
{
	int ret;
	int reg = where & ~3;

	if (bridge->has_pcie && reg >= PCI_CAP_PCIE_END) {
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	if (!bridge->has_pcie && reg >= PCI_BRIDGE_CONF_END) {
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	if (bridge->has_pcie && reg >= PCI_CAP_PCIE_START) {
		reg -= PCI_CAP_PCIE_START;

		if (bridge->ops->read_pcie)
			ret = bridge->ops->read_pcie(bridge, reg, value);
		else
			ret = PCI_SW_BRIDGE_NOT_HANDLED;

		if (ret == PCI_SW_BRIDGE_NOT_HANDLED)
			*value = *((u32*) &bridge->pcie_conf + reg / 4);
	} else {
		if (bridge->ops->read_base)
			ret = bridge->ops->read_base(bridge, reg, value);
		else
			ret = PCI_SW_BRIDGE_NOT_HANDLED;

		if (ret == PCI_SW_BRIDGE_NOT_HANDLED)
			*value = *((u32*) &bridge->conf + reg / 4);
	}

	if (size == 1)
		*value = (*value >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*value = (*value >> (8 * (where & 3))) & 0xffff;
	else if (size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

/*
 * Should be called by the PCI controller driver when writing the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->write_base or ->ops->write_pcie operations.
 */
int pci_sw_bridge_write(struct pci_sw_bridge *bridge, int where,
			int size, u32 value)
{
	int reg = where & ~3;
	int mask, ret, old, new;

	if (bridge->has_pcie && reg >= PCI_CAP_PCIE_END)
		return PCIBIOS_SUCCESSFUL;

	if (!bridge->has_pcie && reg >= PCI_BRIDGE_CONF_END)
		return PCIBIOS_SUCCESSFUL;

	if (size == 4)
		mask = 0xffffffff;
	else if (size == 2)
		mask = 0xffff << ((where & 0x3) * 8);
	else if (size == 1)
		mask = 0xff << ((where & 0x3) * 8);
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	ret = pci_sw_bridge_read(bridge, reg, 4, &old);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	new = old & ~mask;
	new |= (value << ((where & 0x3) * 8)) & mask;

	if (bridge->has_pcie && reg >= PCI_CAP_PCIE_START) {
		reg -= PCI_CAP_PCIE_START;

		*((u32*) &bridge->pcie_conf + reg / 4) = new;

		if (bridge->ops->write_pcie)
			bridge->ops->write_pcie(bridge, reg, old, new, mask);
	} else {
		*((u32*) &bridge->conf + reg / 4) = new;

		if (bridge->ops->write_base)
			bridge->ops->write_base(bridge, reg, old, new, mask);
	}

	return PCIBIOS_SUCCESSFUL;
}
