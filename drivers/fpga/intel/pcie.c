/*
 * Driver for Intel FPGA PCIe device
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Zhang Yi <Yi.Z.Zhang@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/aer.h>

#define DRV_VERSION	"EXPERIMENTAL VERSION"
#define DRV_NAME	"intel-fpga-pci"

/* PCI Device ID */
#define PCIe_DEVICE_ID_PF_INT_5_X	0xBCBD
#define PCIe_DEVICE_ID_PF_INT_6_X	0xBCC0
#define PCIe_DEVICE_ID_PF_DSC_1_X	0x09C4
/* VF Device */
#define PCIe_DEVICE_ID_VF_INT_5_X	0xBCBF
#define PCIe_DEVICE_ID_VF_INT_6_X	0xBCC1
#define PCIe_DEVICE_ID_VF_DSC_1_X	0x09C5

static struct pci_device_id cci_pcie_id_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_PF_DSC_1_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIe_DEVICE_ID_VF_DSC_1_X),},
	{0,}
};
MODULE_DEVICE_TABLE(pci, cci_pcie_id_tbl);

static
int cci_pci_probe(struct pci_dev *pcidev, const struct pci_device_id *pcidevid)
{
	int ret;

	ret = pci_enable_device(pcidev);
	if (ret < 0) {
		dev_err(&pcidev->dev, "Failed to enable device %d.\n", ret);
		goto exit;
	}

	ret = pci_enable_pcie_error_reporting(pcidev);
	if (ret && ret != -EINVAL)
		dev_info(&pcidev->dev, "PCIE AER unavailable %d.\n", ret);

	ret = pci_request_regions(pcidev, DRV_NAME);
	if (ret) {
		dev_err(&pcidev->dev, "Failed to request regions.\n");
		goto disable_error_report_exit;
	}

	pci_set_master(pcidev);
	pci_save_state(pcidev);

	if (!dma_set_mask(&pcidev->dev, DMA_BIT_MASK(64))) {
		dma_set_coherent_mask(&pcidev->dev, DMA_BIT_MASK(64));
	} else if (!dma_set_mask(&pcidev->dev, DMA_BIT_MASK(32))) {
		dma_set_coherent_mask(&pcidev->dev, DMA_BIT_MASK(32));
	} else {
		ret = -EIO;
		dev_err(&pcidev->dev, "No suitable DMA support available.\n");
		goto release_region_exit;
	}

	/* TODO: create and add the platform device per feature list */
	return 0;

release_region_exit:
	pci_release_regions(pcidev);
disable_error_report_exit:
	pci_disable_pcie_error_reporting(pcidev);
	pci_disable_device(pcidev);
exit:
	return ret;
}

static void cci_pci_remove(struct pci_dev *pcidev)
{
	pci_release_regions(pcidev);
	pci_disable_pcie_error_reporting(pcidev);
	pci_disable_device(pcidev);
}

static struct pci_driver cci_pci_driver = {
	.name = DRV_NAME,
	.id_table = cci_pcie_id_tbl,
	.probe = cci_pci_probe,
	.remove = cci_pci_remove,
};

static int __init ccidrv_init(void)
{
	pr_info("Intel(R) FPGA PCIe Driver: Version %s\n", DRV_VERSION);

	return pci_register_driver(&cci_pci_driver);
}

static void __exit ccidrv_exit(void)
{
	pci_unregister_driver(&cci_pci_driver);
}

module_init(ccidrv_init);
module_exit(ccidrv_exit);

MODULE_DESCRIPTION("Intel FPGA PCIe Device Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");
