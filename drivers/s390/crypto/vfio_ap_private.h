/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private data and functions for adjunct processor VFIO matrix driver.
 *
 * Copyright IBM Corp. 2018
 * Author(s): Tony Krowiak <akrowiak@linux.ibm.com>
 */

#ifndef _VFIO_AP_PRIVATE_H_
#define _VFIO_AP_PRIVATE_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mdev.h>
#include <linux/delay.h>

#include "ap_bus.h"

#define VFIO_AP_MODULE_NAME "vfio_ap"
#define VFIO_AP_DRV_NAME "vfio_ap"
/**
 * There must be one mediated matrix device for every guest using AP devices.
 * If every APQN is assigned to a guest, then the maximum number of guests with
 * a unique APQN assigned would be 255 adapters x 255 domains = 72351 guests.
 */
#define AP_MATRIX_MAX_AVAILABLE_INSTANCES 72351

struct ap_matrix_dev {
	struct device device;
	atomic_t available_instances;
};

/**
 * The AP matrix is comprised of three bit masks identifying the adapters,
 * queues (domains) and control domains that belong to an AP matrix. The bits i
 * each mask, from least significant to most significant bit, correspond to IDs
 * 0 to 255. When a bit is set, the corresponding ID belongs to the matrix.
 *
 * @apm identifies the AP adapters in the matrix
 * @apm_max: max adapter number in @apm
 * @aqm identifies the AP queues (domains) in the matrix
 * @aqm_max: max domain number in @aqm
 * @adm identifies the AP control domains in the matrix
 * @adm_max: max domain number in @adm
 */
struct ap_matrix {
	unsigned long apm_max;
	DECLARE_BITMAP(apm, 256);
	unsigned long aqm_max;
	DECLARE_BITMAP(aqm, 256);
	unsigned long adm_max;
	DECLARE_BITMAP(adm, 256);
	struct ap_config_info info;
};

struct ap_matrix_mdev {
	const char *name;
	struct list_head list;
	struct ap_matrix matrix;
	struct notifier_block group_notifier;
	struct kvm *kvm;
};

static struct ap_matrix_dev *to_ap_matrix_dev(struct device *dev)
{
	return container_of(dev, struct ap_matrix_dev, device);
}

extern int vfio_ap_mdev_register(struct ap_matrix_dev *matrix_dev);
extern void vfio_ap_mdev_unregister(struct ap_matrix_dev *matrix_dev);

static inline int vfio_ap_reset_queue(unsigned long apid, unsigned long apqi)
{
	int count = 50;
	struct ap_queue_status status;

	while (count--) {
		status = ap_zapq(AP_MKQID(apid, apqi));
		switch (status.response_code) {
		case AP_RESPONSE_NORMAL:
			return 0;
		case AP_RESPONSE_RESET_IN_PROGRESS:
		case AP_RESPONSE_BUSY:
			msleep(20);
			break;
		default:
			pr_err("%s: error zeroizing %02lx.%04lx: response code %d ",
			       VFIO_AP_MODULE_NAME, apid, apqi,
			       status.response_code);
			return -EIO;
		}
	};

	return -EBUSY;
}

#endif /* _VFIO_AP_PRIVATE_H_ */
