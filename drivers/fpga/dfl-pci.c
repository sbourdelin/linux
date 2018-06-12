// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for FPGA Device Feature List (DFL) PCIe device
 *
 * Copyright (C) 2017-2018 Intel Corporation, Inc.
 *
 * Authors:
 *   Zhang Yi <Yi.Z.Zhang@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 */

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/aer.h>

#include "dfl.h"

#define DRV_VERSION	"0.8"
#define DRV_NAME	"dfl-pci"

struct cci_drvdata {
	struct dfl_fpga_cdev *cdev;	/* container device */
	struct list_head regions;	/* list of pci bar mapping region */
};

/* pci bar mapping info */
struct cci_region {
	int bar;
	void __iomem *ioaddr;	/* pointer to mapped bar region */
	struct list_head node;
};

static void __iomem *cci_pci_ioremap_bar(struct pci_dev *pcidev, int bar)
{
	struct cci_drvdata *drvdata = pci_get_drvdata(pcidev);
	struct cci_region *region;

	list_for_each_entry(region, &drvdata->regions, node)
		if (region->bar == bar) {
			dev_dbg(&pcidev->dev, "BAR %d region exists\n", bar);
			return region->ioaddr;
		}

	region = devm_kzalloc(&pcidev->dev, sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	region->bar = bar;
	region->ioaddr = pci_ioremap_bar(pcidev, bar);
	if (!region->ioaddr) {
		dev_err(&pcidev->dev, "can't ioremap memory from BAR %d.\n",
			bar);
		devm_kfree(&pcidev->dev, region);
		return NULL;
	}

	list_add(&region->node, &drvdata->regions);

	return region->ioaddr;
}

/* PCI Device ID */
#define PCIE_DEVICE_ID_PF_INT_5_X	0xBCBD
#define PCIE_DEVICE_ID_PF_INT_6_X	0xBCC0
#define PCIE_DEVICE_ID_PF_DSC_1_X	0x09C4
/* VF Device */
#define PCIE_DEVICE_ID_VF_INT_5_X	0xBCBF
#define PCIE_DEVICE_ID_VF_INT_6_X	0xBCC1
#define PCIE_DEVICE_ID_VF_DSC_1_X	0x09C5

static struct pci_device_id cci_pcie_id_tbl[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_PF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_VF_INT_5_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_PF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_VF_INT_6_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_PF_DSC_1_X),},
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCIE_DEVICE_ID_VF_DSC_1_X),},
	{0,}
};
MODULE_DEVICE_TABLE(pci, cci_pcie_id_tbl);

static int cci_init_drvdata(struct pci_dev *pcidev)
{
	struct cci_drvdata *drvdata;

	drvdata = devm_kzalloc(&pcidev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	INIT_LIST_HEAD(&drvdata->regions);

	pci_set_drvdata(pcidev, drvdata);

	return 0;
}

static void cci_pci_release_regions(struct pci_dev *pcidev)
{
	struct cci_drvdata *drvdata = pci_get_drvdata(pcidev);
	struct cci_region *tmp, *region;

	list_for_each_entry_safe(region, tmp, &drvdata->regions, node) {
		list_del(&region->node);
		if (region->ioaddr)
			pci_iounmap(pcidev, region->ioaddr);
		devm_kfree(&pcidev->dev, region);
	}
}

static void cci_remove_drvdata(struct pci_dev *pcidev)
{
	struct cci_drvdata *drvdata = pci_get_drvdata(pcidev);

	cci_pci_release_regions(pcidev);
	pci_set_drvdata(pcidev, NULL);
	devm_kfree(&pcidev->dev, drvdata);
}

static void cci_remove_feature_devs(struct pci_dev *pcidev)
{
	struct cci_drvdata *drvdata = pci_get_drvdata(pcidev);

	/* remove all children feature devices */
	dfl_fpga_feature_devs_remove(drvdata->cdev);
}

/* enumerate feature devices under pci device */
static int cci_enumerate_feature_devs(struct pci_dev *pcidev)
{
	struct cci_drvdata *drvdata = pci_get_drvdata(pcidev);
	struct dfl_fpga_enum_info *info;
	struct dfl_fpga_cdev *cdev;
	resource_size_t start, len;
	int port_num, bar, i, ret = 0;
	void __iomem *base;
	u32 offset;
	u64 v;

	/* allocate enumeration info via pci_dev */
	info = dfl_fpga_enum_info_alloc(&pcidev->dev);
	if (!info)
		return -ENOMEM;

	/* start to find Device Feature List from Bar 0 */
	base = cci_pci_ioremap_bar(pcidev, 0);
	if (!base) {
		ret = -ENOMEM;
		goto enum_info_free_exit;
	}

	/*
	 * PF device has FME and Ports/AFUs, and VF device only has 1 Port/AFU.
	 * check them and add related "Device Feature List" info for the next
	 * step enumeration.
	 */
	if (dfl_feature_is_fme(base)) {
		start = pci_resource_start(pcidev, 0);
		len = pci_resource_len(pcidev, 0);

		dfl_fpga_enum_info_add_dfl(info, start, len, base);

		/*
		 * find more Device Feature Lists (e.g Ports) per information
		 * indicated by FME module.
		 */
		v = readq(base + FME_HDR_CAP);
		port_num = FIELD_GET(FME_CAP_NUM_PORTS, v);

		WARN_ON(port_num > MAX_DFL_FPGA_PORT_NUM);

		for (i = 0; i < port_num; i++) {
			v = readq(base + FME_HDR_PORT_OFST(i));

			/* skip ports which are not implemented. */
			if (!(v & FME_PORT_OFST_IMP))
				continue;

			/*
			 * add Port's Device Feature List information for next
			 * step enumeration.
			 */
			bar = FIELD_GET(FME_PORT_OFST_BAR_ID, v);
			offset = FIELD_GET(FME_PORT_OFST_DFH_OFST, v);
			base = cci_pci_ioremap_bar(pcidev, bar);
			if (!base)
				continue;

			start = pci_resource_start(pcidev, bar) + offset;
			len = pci_resource_len(pcidev, bar) - offset;

			dfl_fpga_enum_info_add_dfl(info, start, len,
						   base + offset);
		}
	} else if (dfl_feature_is_port(base)) {
		start = pci_resource_start(pcidev, 0);
		len = pci_resource_len(pcidev, 0);

		dfl_fpga_enum_info_add_dfl(info, start, len, base);
	} else {
		ret = -ENODEV;
		goto enum_info_free_exit;
	}

	/* start enumeration with prepared enumeration information */
	cdev = dfl_fpga_feature_devs_enumerate(info);
	if (IS_ERR(cdev)) {
		dev_err(&pcidev->dev, "Enumeration failure\n");
		ret = PTR_ERR(cdev);
		goto enum_info_free_exit;
	}

	drvdata->cdev = cdev;

enum_info_free_exit:
	dfl_fpga_enum_info_free(info);

	return ret;
}

static
int cci_pci_probe(struct pci_dev *pcidev, const struct pci_device_id *pcidevid)
{
	int ret;

	ret = pci_enable_device(pcidev);
	if (ret < 0) {
		dev_err(&pcidev->dev, "Failed to enable device %d.\n", ret);
		return ret;
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

	if (!pci_set_dma_mask(pcidev, DMA_BIT_MASK(64))) {
		ret = pci_set_consistent_dma_mask(pcidev, DMA_BIT_MASK(64));
		if (ret)
			goto release_region_exit;
	} else if (!pci_set_dma_mask(pcidev, DMA_BIT_MASK(32))) {
		ret = pci_set_consistent_dma_mask(pcidev, DMA_BIT_MASK(32));
		if (ret)
			goto release_region_exit;
	} else {
		ret = -EIO;
		dev_err(&pcidev->dev, "No suitable DMA support available.\n");
		goto release_region_exit;
	}

	ret = cci_init_drvdata(pcidev);
	if (ret) {
		dev_err(&pcidev->dev, "Fail to init drvdata %d.\n", ret);
		goto release_region_exit;
	}

	ret = cci_enumerate_feature_devs(pcidev);
	if (ret) {
		dev_err(&pcidev->dev, "enumeration failure %d.\n", ret);
		goto remove_drvdata_exit;
	}

	return ret;

remove_drvdata_exit:
	cci_remove_drvdata(pcidev);
release_region_exit:
	pci_release_regions(pcidev);
disable_error_report_exit:
	pci_disable_pcie_error_reporting(pcidev);
	pci_disable_device(pcidev);
	return ret;
}

static void cci_pci_remove(struct pci_dev *pcidev)
{
	cci_remove_feature_devs(pcidev);
	cci_remove_drvdata(pcidev);
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

module_pci_driver(cci_pci_driver);

MODULE_DESCRIPTION("FPGA DFL PCIe Device Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
