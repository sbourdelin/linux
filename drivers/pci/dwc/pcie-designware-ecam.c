/*
 * Driver for mostly ECAM compatible Synopsys dw PCIe controllers
 * configured by the firmware into RC mode
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2014 ARM Limited
 * Copyright (C) 2017 Linaro Limited
 *
 * Authors: Will Deacon <will.deacon@arm.com>
 *          Ard Biesheuvel <ard.biesheuvel@linaro.org>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>

static int pci_dw_ecam_config_read(struct pci_bus *bus, u32 devfn, int where,
				   int size, u32 *val)
{
	struct pci_config_window *cfg = bus->sysdata;

	/*
	 * The Synopsys dw PCIe controller in RC mode will not filter type 0
	 * config TLPs sent to devices 1 and up on its downstream port,
	 * resulting in devices appearing multiple times on bus 0 unless we
	 * filter them here.
	 */
	if (bus->number == cfg->busr.start && PCI_SLOT(devfn) > 0) {
		*val = 0xffffffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return pci_generic_config_read(bus, devfn, where, size, val);
}

static int pci_dw_ecam_config_write(struct pci_bus *bus, u32 devfn, int where,
				    int size, u32 val)
{
	struct pci_config_window *cfg = bus->sysdata;

	if (bus->number == cfg->busr.start && PCI_SLOT(devfn) > 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return pci_generic_config_write(bus, devfn, where, size, val);
}

static struct pci_ecam_ops pci_dw_ecam_bus_ops = {
	.pci_ops.map_bus		= pci_ecam_map_bus,
	.pci_ops.read			= pci_dw_ecam_config_read,
	.pci_ops.write			= pci_dw_ecam_config_write,
	.bus_shift			= 20,
};

static const struct of_device_id pci_dw_ecam_of_match[] = {
	{ .compatible = "snps,dw-pcie-ecam" },
	{ },
};

static int pci_dw_ecam_probe(struct platform_device *pdev)
{
	return pci_host_common_probe(pdev, &pci_dw_ecam_bus_ops);
}

static struct platform_driver pci_dw_ecam_driver = {
	.driver.name			= "pcie-designware-ecam",
	.driver.of_match_table		= pci_dw_ecam_of_match,
	.driver.suppress_bind_attrs	= true,
	.probe				= pci_dw_ecam_probe,
};
builtin_platform_driver(pci_dw_ecam_driver);
