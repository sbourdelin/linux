/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/bitops.h>

#include "hinic_hw_if.h"
#include "hinic_hw_dev.h"

#define MAX_IRQS(max_qps, num_aeqs, num_ceqs)	\
		 (2 * (max_qps) + (num_aeqs) + (num_ceqs))

/**
 * init_msix - enable the msix and save the entries
 * @hwdev: the NIC HW device
 *
 * Return 0 - Success, negative - Failure
 **/
static int init_msix(struct hinic_hwdev *hwdev)
{
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	int num_aeqs = HINIC_HWIF_NUM_AEQS(hwif);
	int num_ceqs = HINIC_HWIF_NUM_CEQS(hwif);
	int nr_irqs = MAX_IRQS(HINIC_MAX_QPS, num_aeqs, num_ceqs);
	size_t msix_entries_size;
	int i, err;

	if (nr_irqs > HINIC_HWIF_NUM_IRQS(hwif))
		nr_irqs = HINIC_HWIF_NUM_IRQS(hwif);

	msix_entries_size = nr_irqs * sizeof(*hwdev->msix_entries);

	hwdev->msix_entries = kzalloc(msix_entries_size, GFP_KERNEL);
	if (!hwdev->msix_entries)
		return -ENOMEM;

	for (i = 0; i < nr_irqs; i++)
		hwdev->msix_entries[i].entry = i;

	err = pci_enable_msix_exact(pdev, hwdev->msix_entries, nr_irqs);
	if (err) {
		dev_err(&pdev->dev, "Failed to enable pci msix\n");
		goto enable_msix_err;
	}

	return 0;

enable_msix_err:
	kfree(hwdev->msix_entries);
	return err;
}

/**
 * free_msix - disable the msix and free the saved entries
 * @hwdev: the NIC HW device
 **/
static void free_msix(struct hinic_hwdev *hwdev)
{
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;

	pci_disable_msix(pdev);
	kfree(hwdev->msix_entries);
}

/**
 * init_pfhwdev - Initialize the extended components of PF
 * @pfhwdev: the HW device for PF
 *
 * Return 0 - success, negative - failure
 **/
static int init_pfhwdev(struct hinic_pfhwdev *pfhwdev)
{
	/* Initialize PF HW device extended components */
	return 0;
}

/**
 * free_pfhwdev - Free the extended components of PF
 * @pfhwdev: the HW device for PF
 **/
static void free_pfhwdev(struct hinic_pfhwdev *pfhwdev)
{
}

/**
 * hinic_init_hwdev - Initialize the NIC HW
 * @hwdev: the NIC HW device that is returned from the initialization
 * @pdev: the NIC pci device
 *
 * Return 0 - Success, negative - Failure
 *
 * Initialize the NIC HW device and return a pointer to it in the first arg
 **/
int hinic_init_hwdev(struct hinic_hwdev **hwdev, struct pci_dev *pdev)
{
	struct hinic_pfhwdev *pfhwdev;
	struct hinic_hwif *hwif;
	int err;

	hwif = kzalloc(sizeof(*hwif), GFP_KERNEL);
	if (!hwif)
		return -ENOMEM;

	err = hinic_init_hwif(hwif, pdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to init HW interface\n");
		goto init_hwif_err;
	}

	if (!HINIC_IS_PF(hwif) && !HINIC_IS_PPF(hwif)) {
		dev_err(&pdev->dev, "Unsupported PCI Function type\n");
		err = -EFAULT;
		goto func_type_err;
	}

	pfhwdev = kzalloc(sizeof(*pfhwdev), GFP_KERNEL);
	if (!pfhwdev) {
		err = -ENOMEM;
		goto pfhwdev_alloc_err;
	}

	*hwdev = &pfhwdev->hwdev;
	(*hwdev)->hwif = hwif;

	err = init_msix(*hwdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to init msix\n");
		goto init_msix_err;
	}

	err = init_pfhwdev(pfhwdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to init PF HW device\n");
		goto init_pfhwdev_err;
	}

	return 0;

init_pfhwdev_err:
	free_msix(*hwdev);

init_msix_err:
	kfree(pfhwdev);

pfhwdev_alloc_err:
func_type_err:
	hinic_free_hwif(hwif);

init_hwif_err:
	kfree(hwif);
	return err;
}

/**
 * hinic_free_hwdev - Free the NIC HW device
 * @hwdev: the NIC HW device
 **/
void hinic_free_hwdev(struct hinic_hwdev *hwdev)
{
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_pfhwdev *pfhwdev;

	if (!HINIC_IS_PF(hwif) && !HINIC_IS_PPF(hwif)) {
		dev_err(&pdev->dev, "unsupported PCI Function type\n");
		return;
	}

	pfhwdev = container_of(hwdev, struct hinic_pfhwdev, hwdev);

	free_pfhwdev(pfhwdev);

	free_msix(hwdev);

	kfree(pfhwdev);

	hinic_free_hwif(hwif);
	kfree(hwif);
}

/**
 * hinic_hwdev_num_qps - return the number QPs available for use
 * @hwdev: the NIC HW device
 *
 * Return number QPs available for use
 **/
int hinic_hwdev_num_qps(struct hinic_hwdev *hwdev)
{
	struct hinic_hwif *hwif = hwdev->hwif;
	int num_aeqs, num_ceqs, nr_irqs, num_qps;

	num_aeqs = HINIC_HWIF_NUM_AEQS(hwif);
	num_ceqs = HINIC_HWIF_NUM_CEQS(hwif);
	nr_irqs = HINIC_HWIF_NUM_IRQS(hwif);

	/* Each QP has its own (SQ + RQ) interrupt */
	num_qps = (nr_irqs - (num_aeqs + num_ceqs)) / 2;

	/* num_qps must be power of 2 */
	return BIT(fls(num_qps) - 1);
}
