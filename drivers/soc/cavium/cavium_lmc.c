/*
 * These PCI devices contain RAS functionality and PMU counters. To allow
 * independent RAS and PMU drivers this driver registers for the PCI devices
 * and multi-plexes probe and removal.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright: Cavium, Inc. (C) 2017
 *
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/soc/cavium/lmc.h>

static int cvm_lmc_probe(struct pci_dev *pdev,
			 const struct pci_device_id *ent)
{
	if (IS_ENABLED(CONFIG_EDAC_THUNDERX))
		thunderx_edac_lmc_probe(pdev, ent);
	return 0;
}

static void cvm_lmc_remove(struct pci_dev *pdev)
{
	if (IS_ENABLED(CONFIG_EDAC_THUNDERX))
		thunderx_edac_lmc_remove(pdev);
}

static const struct pci_device_id cvm_lmc_pci_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, 0xa022) },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, cvm_lmc_pci_table);

static struct pci_driver cvm_lmc_pci_driver = {
	.name     = "Cavium SOC memory controller",
	.id_table = cvm_lmc_pci_table,
	.probe    = cvm_lmc_probe,
	.remove   = cvm_lmc_remove,
};

module_pci_driver(cvm_lmc_pci_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Cavium, Inc.");
MODULE_DESCRIPTION("PCI driver for Cavium SOC memory controller");
