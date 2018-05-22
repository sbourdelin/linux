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

#define aer_stats_aggregate_attr(field)                                        \
	static ssize_t                                                         \
	field##_show(struct device *dev, struct device_attribute *attr,        \
		     char *buf)                                                \
{                                                                              \
	struct pci_dev *pdev = to_pci_dev(dev);                                \
	return sprintf(buf, "0x%llx\n", pdev->aer_stats->field);               \
}                                                                              \
static DEVICE_ATTR_RO(field)

aer_stats_aggregate_attr(dev_total_cor_errs);
aer_stats_aggregate_attr(dev_total_fatal_errs);
aer_stats_aggregate_attr(dev_total_nonfatal_errs);
aer_stats_aggregate_attr(rootport_total_cor_errs);
aer_stats_aggregate_attr(rootport_total_fatal_errs);
aer_stats_aggregate_attr(rootport_total_nonfatal_errs);

#define aer_stats_breakdown_attr(field, stats_array, strings_array)            \
	static ssize_t                                                         \
	field##_show(struct device *dev, struct device_attribute *attr,        \
		     char *buf)                                                \
{                                                                              \
	unsigned int i;                                                        \
	char *str = buf;                                                       \
	struct pci_dev *pdev = to_pci_dev(dev);                                \
	u64 *stats = pdev->aer_stats->stats_array;                             \
	for (i = 0; i < ARRAY_SIZE(strings_array); i++) {                      \
		if (strings_array[i])                                          \
			str += sprintf(str, "%s = 0x%llx\n",                   \
				       strings_array[i], stats[i]);            \
	}                                                                      \
	return str-buf;                                                        \
}                                                                              \
static DEVICE_ATTR_RO(field)

aer_stats_breakdown_attr(dev_breakdown_correctable, dev_cor_errs,
			 aer_correctable_error_string);
aer_stats_breakdown_attr(dev_breakdown_uncorrectable, dev_uncor_errs,
			 aer_uncorrectable_error_string);

static struct attribute *aer_stats_attrs[] __ro_after_init = {
	&dev_attr_dev_total_cor_errs.attr,
	&dev_attr_dev_total_fatal_errs.attr,
	&dev_attr_dev_total_nonfatal_errs.attr,
	&dev_attr_dev_breakdown_correctable.attr,
	&dev_attr_dev_breakdown_uncorrectable.attr,
	&dev_attr_rootport_total_cor_errs.attr,
	&dev_attr_rootport_total_fatal_errs.attr,
	&dev_attr_rootport_total_nonfatal_errs.attr,
	NULL
};

static umode_t aer_stats_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_stats)
		return 0;

	if ((a == &dev_attr_rootport_total_cor_errs.attr ||
	     a == &dev_attr_rootport_total_fatal_errs.attr ||
	     a == &dev_attr_rootport_total_nonfatal_errs.attr) &&
	    pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT)
		return 0;

	return a->mode;
}

const struct attribute_group aer_stats_attr_group = {
	.name  = "aer_stats",
	.attrs  = aer_stats_attrs,
	.is_visible = aer_stats_attrs_are_visible,
};

void pci_dev_aer_stats_incr(struct pci_dev *pdev, struct aer_err_info *info)
{
	int status, i, max = -1;
	u64 *counter = NULL;
	struct aer_stats *aer_stats = pdev->aer_stats;

	if (unlikely(!aer_stats))
		return;

	switch (info->severity) {
	case AER_CORRECTABLE:
		aer_stats->dev_total_cor_errs++;
		counter = &aer_stats->dev_cor_errs[0];
		max = AER_MAX_TYPEOF_CORRECTABLE_ERRS;
		break;
	case AER_NONFATAL:
		aer_stats->dev_total_nonfatal_errs++;
		counter = &aer_stats->dev_uncor_errs[0];
		max = AER_MAX_TYPEOF_UNCORRECTABLE_ERRS;
		break;
	case AER_FATAL:
		aer_stats->dev_total_fatal_errs++;
		counter = &aer_stats->dev_uncor_errs[0];
		max = AER_MAX_TYPEOF_UNCORRECTABLE_ERRS;
		break;
	}

	status = (info->status & ~info->mask);
	for (i = 0; i < max; i++)
		if (status & (1 << i))
			counter[i]++;
}

void pci_rootport_aer_stats_incr(struct pci_dev *pdev,
				 struct aer_err_source *e_src)
{
	struct aer_stats *aer_stats = pdev->aer_stats;

	if (unlikely(!aer_stats))
		return;

	if (e_src->status & PCI_ERR_ROOT_COR_RCV)
		aer_stats->rootport_total_cor_errs++;

	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) {
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV)
			aer_stats->rootport_total_fatal_errs++;
		else
			aer_stats->rootport_total_nonfatal_errs++;
	}
}

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
