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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/bitops.h>

#include "hinic_hw_if.h"
#include "hinic_hw_eqs.h"
#include "hinic_hw_mgmt.h"
#include "hinic_hw_dev.h"

#define MAX_IRQS(max_qps, num_aeqs, num_ceqs)	\
		 (2 * (max_qps) + (num_aeqs) + (num_ceqs))

enum intr_type {
	INTR_MSIX_TYPE,
};

/* HW struct */
struct hinic_dev_cap {
	u8	status;
	u8	version;
	u8	rsvd0[6];

	u8	rsvd1[5];
	u8	intr_type;
	u8	rsvd2[66];
	u16	max_sqs;
	u16	max_rqs;
	u8	rsvd3[208];
};

/**
 * get_capability - convert device capabilities to NIC capabilities
 * @hwdev: the HW device to set and convert device capabilities for
 * @dev_cap: device capabilities from FW
 *
 * Return 0 - Success, negative - Failure
 **/
static int get_capability(struct hinic_hwdev *hwdev,
			  struct hinic_dev_cap *dev_cap)
{
	struct hinic_hwif *hwif = hwdev->hwif;
	struct hinic_cap *nic_cap = &hwdev->nic_cap;
	int num_aeqs, num_ceqs, num_irqs, num_qps;

	if (!HINIC_IS_PF(hwif) && !HINIC_IS_PPF(hwif))
		return -EINVAL;

	if (dev_cap->intr_type != INTR_MSIX_TYPE)
		return -EFAULT;

	num_aeqs = HINIC_HWIF_NUM_AEQS(hwif);
	num_ceqs = HINIC_HWIF_NUM_CEQS(hwif);
	num_irqs = HINIC_HWIF_NUM_IRQS(hwif);

	/* Each QP has its own (SQ + RQ) interrupts */
	num_qps = (num_irqs - (num_aeqs + num_ceqs)) / 2;

	/* num_qps must be power of 2 */
	num_qps = BIT(fls(num_qps) - 1);

	nic_cap->max_qps = dev_cap->max_sqs + 1;
	if (nic_cap->max_qps != (dev_cap->max_rqs + 1))
		return -EFAULT;

	if (num_qps < nic_cap->max_qps)
		nic_cap->num_qps = num_qps;
	else
		nic_cap->num_qps = nic_cap->max_qps;

	return 0;
}

/**
 * get_cap_from_fw - get device capabilities from FW
 * @pfhwdev: the PF HW device to get capabilities for
 *
 * Return 0 - Success, negative - Failure
 **/
static int get_cap_from_fw(struct hinic_pfhwdev *pfhwdev)
{
	struct hinic_hwdev *hwdev = &pfhwdev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_dev_cap dev_cap;
	u16 in_len, out_len;
	int err;

	in_len = 0;
	out_len = sizeof(dev_cap);

	err = hinic_msg_to_mgmt(&pfhwdev->pf_to_mgmt, HINIC_MOD_CFGM,
				HINIC_CFG_NIC_CAP, &dev_cap, in_len, &dev_cap,
				&out_len, HINIC_MGMT_MSG_SYNC);
	if (err) {
		dev_err(&pdev->dev, "Failed to get capability from FW\n");
		return err;
	}

	return get_capability(hwdev, &dev_cap);
}

/**
 * get_dev_cap - get device capabilities
 * @hwdev: the NIC HW device to get capabilities for
 *
 * Return 0 - Success, negative - Failure
 **/
static int get_dev_cap(struct hinic_hwdev *hwdev)
{
	struct hinic_pfhwdev *pfhwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;
	int err;

	switch (HINIC_FUNC_TYPE(hwif)) {
	case HINIC_PPF:
	case HINIC_PF:
		pfhwdev = container_of(hwdev, struct hinic_pfhwdev, hwdev);

		err = get_cap_from_fw(pfhwdev);
		if (err) {
			dev_err(&pdev->dev, "Failed to get capability from FW\n");
			return err;
		}
		break;

	default:
		pr_err("Unsupported PCI Function type\n");
		return -EINVAL;
	}

	return 0;
}

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
 * hinic_port_msg_cmd - send port msg to mgmt
 * @hwdev: the NIC HW device
 * @cmd: the port command
 * @buf_in: input buffer
 * @in_size: input size
 * @buf_out: output buffer
 * @out_size: returned output size
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_port_msg_cmd(struct hinic_hwdev *hwdev, enum hinic_port_cmd cmd,
		       void *buf_in, u16 in_size, void *buf_out, u16 *out_size)
{
	struct hinic_pfhwdev *pfhwdev;
	struct hinic_hwif *hwif = hwdev->hwif;

	if (!HINIC_IS_PF(hwif) && !HINIC_IS_PPF(hwif)) {
		pr_err("unsupported PCI Function type\n");
		return -EINVAL;
	}

	pfhwdev = container_of(hwdev, struct hinic_pfhwdev, hwdev);

	return hinic_msg_to_mgmt(&pfhwdev->pf_to_mgmt, HINIC_MOD_L2NIC, cmd,
				 buf_in, in_size, buf_out, out_size,
				 HINIC_MGMT_MSG_SYNC);
}

/**
 * init_pfhwdev - Initialize the extended components of PF
 * @pfhwdev: the HW device for PF
 *
 * Return 0 - success, negative - failure
 **/
static int init_pfhwdev(struct hinic_pfhwdev *pfhwdev)
{
	struct hinic_hwdev *hwdev = &pfhwdev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	int err;

	err = hinic_pf_to_mgmt_init(&pfhwdev->pf_to_mgmt, hwif);
	if (err) {
		pr_err("Failed to initialize PF to MGMT channel\n");
		return err;
	}

	return 0;
}

/**
 * free_pfhwdev - Free the extended components of PF
 * @pfhwdev: the HW device for PF
 **/
static void free_pfhwdev(struct hinic_pfhwdev *pfhwdev)
{
	hinic_pf_to_mgmt_free(&pfhwdev->pf_to_mgmt);
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
	int err, num_aeqs;

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

	num_aeqs = HINIC_HWIF_NUM_AEQS(hwif);

	err = hinic_aeqs_init(&(*hwdev)->aeqs, hwif, num_aeqs,
			      HINIC_DEFAULT_AEQ_LEN, HINIC_EQ_PAGE_SIZE,
			      (*hwdev)->msix_entries);
	if (err) {
		pr_err("Failed to init async event queues\n");
		goto aeqs_init_err;
	}

	err = init_pfhwdev(pfhwdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to init PF HW device\n");
		goto init_pfhwdev_err;
	}

	err = get_dev_cap(*hwdev);
	if (err) {
		pr_err("Failed to get device capabilities\n");
		goto dev_cap_err;
	}

	return 0;

dev_cap_err:
	free_pfhwdev(pfhwdev);

init_pfhwdev_err:
	hinic_aeqs_free(&(*hwdev)->aeqs);

aeqs_init_err:
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

	hinic_aeqs_free(&hwdev->aeqs);

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
	struct hinic_cap *nic_cap = &hwdev->nic_cap;

	return nic_cap->num_qps;
}
