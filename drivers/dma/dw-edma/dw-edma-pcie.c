// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare eDMA PCIe driver
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/dma/edma.h>

#include "dw-edma-core.h"

enum dw_edma_pcie_bar {
	BAR_0,
	BAR_1,
	BAR_2,
	BAR_3,
	BAR_4,
	BAR_5
};

struct dw_edma_pcie_data {
	enum dw_edma_pcie_bar		regs_bar;
	u64				regs_off;
	enum dw_edma_pcie_bar		ll_bar;
	u64				ll_off;
	size_t				ll_sz;
	u32				version;
	enum dw_edma_mode		mode;
};

static const struct dw_edma_pcie_data snps_edda_data = {
	/* eDMA registers location */
	.regs_bar			= BAR_0,
	.regs_off			= 0x1000,	/*   4 KBytes */
	/* eDMA memory linked list location */
	.ll_bar				= BAR_2,
	.ll_off				= 0,		/*   0 KBytes */
	.ll_sz				= 0x20000,	/* 128 KBytes */
	/* Other */
	.version			= 0,
	.mode				= EDMA_MODE_UNROLL,
};

static int dw_edma_pcie_probe(struct pci_dev *pdev,
			      const struct pci_device_id *pid)
{
	const struct dw_edma_pcie_data *pdata = (void *)pid->driver_data;
	struct device *dev = &pdev->dev;
	struct dw_edma_chip *chip;
	struct dw_edma *dw;
	void __iomem *reg;
	int err, irq = -1;
	u32 addr_hi, addr_lo;
	u16 flags;
	u8 cap_off;

	if (!pdata) {
		dev_err(dev, "%s missing data struture\n",
			pci_name(pdev));
		return -EFAULT;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(dev, "%s enabling device failed\n",
			pci_name(pdev));
		return err;
	}

	err = pcim_iomap_regions(pdev,
				 BIT(pdata->regs_bar) | BIT(pdata->ll_bar),
				 pci_name(pdev));
	if (err) {
		dev_err(dev, "%s eDMA BAR I/O remapping failed\n",
			pci_name(pdev));
		return err;
	}

	pci_set_master(pdev);

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (err) {
		dev_err(dev, "%s DMA mask set failed\n",
			pci_name(pdev));
		return err;
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	if (err) {
		dev_err(dev, "%s consistent DMA mask set failed\n",
			pci_name(pdev));
		return err;
	}

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	dw = devm_kzalloc(&pdev->dev, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return -ENOMEM;

	irq = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (irq < 0) {
		dev_err(dev, "%s failed to alloc IRQ vector\n",
			pci_name(pdev));
		return -EPERM;
	}

	chip->dw = dw;
	chip->dev = dev;
	chip->id = pdev->devfn;
	chip->irq = pdev->irq;

	dw->regs = pcim_iomap_table(pdev)[pdata->regs_bar];
	dw->regs += pdata->regs_off;

	dw->va_ll = pcim_iomap_table(pdev)[pdata->ll_bar];
	dw->va_ll += pdata->ll_off;
	dw->pa_ll = pdev->resource[pdata->ll_bar].start;
	dw->pa_ll += pdata->ll_off;
	dw->ll_sz = pdata->ll_sz;

	dw->msi_addr = 0;
	dw->msi_data = 0;

	dw->version = pdata->version;
	dw->mode = pdata->mode;

	dev_dbg(dev, "Version:\t%u\n", dw->version);

	dev_dbg(dev, "Mode:\t%s\n",
		dw->mode == EDMA_MODE_LEGACY ? "Legacy" : "Unroll");

	dev_dbg(dev, "Registers:\tBAR=%u, off=0x%.16llx B, addr=0x%.8lx\n",
		pdata->regs_bar, pdata->regs_off,
		(unsigned long)dw->regs);

	dev_dbg(dev, "L. List:\tBAR=%u, off=0x%.16llx B, sz=0x%.8x B, vaddr=0x%.8lx, paddr=0x%.8lx",
		pdata->ll_bar, pdata->ll_off, pdata->ll_sz,
		(unsigned long)dw->va_ll,
		(unsigned long)dw->pa_ll);

	if (pdev->msi_cap && pdev->msi_enabled) {
		cap_off = pdev->msi_cap + PCI_MSI_FLAGS;
		pci_read_config_word(pdev, cap_off, &flags);
		if (flags & PCI_MSI_FLAGS_ENABLE) {
			cap_off = pdev->msi_cap + PCI_MSI_ADDRESS_LO;
			pci_read_config_dword(pdev, cap_off, &addr_lo);

			if (flags & PCI_MSI_FLAGS_64BIT) {
				cap_off = pdev->msi_cap + PCI_MSI_ADDRESS_HI;
				pci_read_config_dword(pdev, cap_off, &addr_hi);
				cap_off = pdev->msi_cap + PCI_MSI_DATA_64;
			} else {
				addr_hi = 0;
				cap_off = pdev->msi_cap + PCI_MSI_DATA_32;
			}

			dw->msi_addr = addr_hi;
			dw->msi_addr <<= 32;
			dw->msi_addr |= addr_lo;

			pci_read_config_dword(pdev, cap_off, &dw->msi_data);
			dw->msi_data &= 0xffff;

			dev_dbg(dev, "MSI:\t\taddr=0x%.16llx, data=0x%.8x, nr=%d\n",
				dw->msi_addr, dw->msi_data, pdev->irq);
		}
	}

	if (pdev->msix_cap && pdev->msix_enabled) {
		u32 offset;
		u8 bir;

		cap_off = pdev->msix_cap + PCI_MSIX_FLAGS;
		pci_read_config_word(pdev, cap_off, &flags);

		if (flags & PCI_MSIX_FLAGS_ENABLE) {
			cap_off = pdev->msix_cap + PCI_MSIX_TABLE;
			pci_read_config_dword(pdev, cap_off, &offset);

			bir = offset & PCI_MSIX_TABLE_BIR;
			offset &= PCI_MSIX_TABLE_OFFSET;

			reg = pcim_iomap_table(pdev)[bir];
			reg += offset;

			addr_lo = readl(reg + PCI_MSIX_ENTRY_LOWER_ADDR);
			addr_hi = readl(reg + PCI_MSIX_ENTRY_UPPER_ADDR);
			dw->msi_addr = addr_hi;
			dw->msi_addr <<= 32;
			dw->msi_addr |= addr_lo;

			dw->msi_data = readl(reg + PCI_MSIX_ENTRY_DATA);

			dev_dbg(dev,
				"MSI-X:\taddr=0x%.16llx, data=0x%.8x, nr=%d\n",
				dw->msi_addr, dw->msi_data, pdev->irq);
		}
	}

	if (!pdev->msi_enabled && !pdev->msix_enabled) {
		dev_err(dev, "%s enable interrupt failed\n",
			pci_name(pdev));
		return -EPERM;
	}

	err = dw_edma_probe(chip);
	if (err) {
		dev_err(dev, "%s eDMA probe failed\n",
			pci_name(pdev));
		return err;
	}

	pci_set_drvdata(pdev, chip);

	dev_info(dev, "DesignWare eDMA PCIe driver loaded completely\n");

	return 0;
}

static void dw_edma_pcie_remove(struct pci_dev *pdev)
{
	struct dw_edma_chip *chip = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int err;

	err = dw_edma_remove(chip);
	if (err)
		dev_warn(dev, "can't remove device properly: %d\n", err);

	pci_free_irq_vectors(pdev);

	dev_info(dev, "DesignWare eDMA PCIe driver unloaded completely\n");
}

static const struct pci_device_id dw_edma_pcie_id_table[] = {
	{ PCI_DEVICE_DATA(SYNOPSYS, EDDA, &snps_edda_data) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dw_edma_pcie_id_table);

static struct pci_driver dw_edma_pcie_driver = {
	.name		= "dw-edma-pcie",
	.id_table	= dw_edma_pcie_id_table,
	.probe		= dw_edma_pcie_probe,
	.remove		= dw_edma_pcie_remove,
};

module_pci_driver(dw_edma_pcie_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Synopsys DesignWare eDMA PCIe driver");
MODULE_AUTHOR("Gustavo Pimentel <gustavo.pimentel@synopsys.com>");
