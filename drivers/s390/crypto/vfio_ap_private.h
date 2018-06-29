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

struct ap_matrix_mdev {
	const char *name;
	struct list_head list;
};

static struct ap_matrix_dev *to_ap_matrix_dev(struct device *dev)
{
	return container_of(dev, struct ap_matrix_dev, device);
}

extern int vfio_ap_mdev_register(struct ap_matrix_dev *matrix_dev);
extern void vfio_ap_mdev_unregister(struct ap_matrix_dev *matrix_dev);

#endif /* _VFIO_AP_PRIVATE_H_ */
