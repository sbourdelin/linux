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

#include "ap_bus.h"

#define VFIO_AP_MODULE_NAME "vfio_ap"
#define VFIO_AP_DRV_NAME "vfio_ap"

struct ap_matrix_dev {
	struct device device;
};

static inline struct ap_matrix_dev
*to_ap_matrix_parent_dev(struct device *dev)
{
	return container_of(dev, struct ap_matrix_dev, device.parent);
}

#endif /* _VFIO_AP_PRIVATE_H_ */
