// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2018 Google Inc, All Rights Reserved.
 *  Rajat Jain (rajatja@google.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * AER Statistics - exposed to userspace via /sysfs attributes.
 */

#include <linux/pci.h>
#include "aerdrv.h"

/* AER stats for the device */
struct aer_stats {

	/*
	 * Fields for all AER capable devices. They indicate the errors
	 * "as seen by this device". Note that this may mean that if an
	 * end point is causing problems, the AER counters may increment
	 * at its link partner (e.g. root port) because the errors will be
	 * "seen" by the link partner and not the the problematic end point
	 * itself (which may report all counters as 0 as it never saw any
	 * problems).
	 */
	/* Individual counters for different type of correctable errors */
	u64 dev_cor_errs[AER_MAX_TYPEOF_CORRECTABLE_ERRS];
	/* Individual counters for different type of uncorrectable errors */
	u64 dev_uncor_errs[AER_MAX_TYPEOF_UNCORRECTABLE_ERRS];
	/* Total number of correctable errors seen by this device */
	u64 dev_total_cor_errs;
	/* Total number of fatal uncorrectable errors seen by this device */
	u64 dev_total_fatal_errs;
	/* Total number of fatal uncorrectable errors seen by this device */
	u64 dev_total_nonfatal_errs;

	/*
	 * Fields for Root ports only, these indicate the total number of
	 * ERR_COR, ERR_FATAL, and ERR_NONFATAL messages received by the
	 * rootport, INCLUDING the ones that are generated internally (by
	 * the rootport itself)
	 */
	u64 rootport_total_cor_errs;
	u64 rootport_total_fatal_errs;
	u64 rootport_total_nonfatal_errs;
};

int pci_aer_stats_init(struct pci_dev *pdev)
{
	pdev->aer_stats = kzalloc(sizeof(struct aer_stats), GFP_KERNEL);
	if (!pdev->aer_stats) {
		dev_err(&pdev->dev, "No memory for aer_stats\n");
		return -ENOMEM;
	}
	return 0;
}

void pci_aer_stats_exit(struct pci_dev *pdev)
{
	kfree(pdev->aer_stats);
	pdev->aer_stats = NULL;
}
