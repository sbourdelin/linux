// SPDX-License-Identifier: GPL-2.0+
/*
 * Adjunct processor matrix VFIO device driver callbacks.
 *
 * Copyright IBM Corp. 2018
 * Author(s): Tony Krowiak <akrowiak@linux.ibm.com>
 *
 */
#include <linux/string.h>
#include <linux/vfio.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/ctype.h>

#include "vfio_ap_private.h"

#define VFOP_AP_MDEV_TYPE_HWVIRT "passthrough"
#define VFIO_AP_MDEV_NAME_HWVIRT "VFIO AP Passthrough Device"

DEFINE_SPINLOCK(mdev_list_lock);
LIST_HEAD(mdev_list);

static void vfio_ap_matrix_init(struct ap_matrix *matrix)
{
	/* Test if PQAP(QCI) instruction is available */
	if (test_facility(12))
		ap_qci(&matrix->info);

	matrix->apm_max = matrix->info.apxa ? matrix->info.Na : 63;
	matrix->aqm_max = matrix->info.apxa ? matrix->info.Nd : 15;
	matrix->adm_max = matrix->info.apxa ? matrix->info.Nd : 15;
}

static int vfio_ap_mdev_create(struct kobject *kobj, struct mdev_device *mdev)
{
	struct ap_matrix_dev *matrix_dev =
		to_ap_matrix_dev(mdev_parent_dev(mdev));
	struct ap_matrix_mdev *matrix_mdev;

	matrix_mdev = kzalloc(sizeof(*matrix_mdev), GFP_KERNEL);
	if (!matrix_mdev)
		return -ENOMEM;

	matrix_mdev->name = dev_name(mdev_dev(mdev));
	vfio_ap_matrix_init(&matrix_mdev->matrix);
	mdev_set_drvdata(mdev, matrix_mdev);

	if (atomic_dec_if_positive(&matrix_dev->available_instances) < 0) {
		kfree(matrix_mdev);
		return -EPERM;
	}

	spin_lock_bh(&mdev_list_lock);
	list_add(&matrix_mdev->list, &mdev_list);
	spin_unlock_bh(&mdev_list_lock);

	return 0;
}

static int vfio_ap_mdev_remove(struct mdev_device *mdev)
{
	struct ap_matrix_dev *matrix_dev =
		to_ap_matrix_dev(mdev_parent_dev(mdev));
	struct ap_matrix_mdev *matrix_mdev = mdev_get_drvdata(mdev);

	spin_lock_bh(&mdev_list_lock);
	list_del(&matrix_mdev->list);
	spin_unlock_bh(&mdev_list_lock);
	kfree(matrix_mdev);
	mdev_set_drvdata(mdev, NULL);
	atomic_inc(&matrix_dev->available_instances);

	return 0;
}

static ssize_t name_show(struct kobject *kobj, struct device *dev, char *buf)
{
	return sprintf(buf, "%s\n", VFIO_AP_MDEV_NAME_HWVIRT);
}

MDEV_TYPE_ATTR_RO(name);

static ssize_t available_instances_show(struct kobject *kobj,
					struct device *dev, char *buf)
{
	struct ap_matrix_dev *matrix_dev = to_ap_matrix_dev(dev);

	return sprintf(buf, "%d\n",
		       atomic_read(&matrix_dev->available_instances));
}

MDEV_TYPE_ATTR_RO(available_instances);

static ssize_t device_api_show(struct kobject *kobj, struct device *dev,
			       char *buf)
{
	return sprintf(buf, "%s\n", VFIO_DEVICE_API_AP_STRING);
}

MDEV_TYPE_ATTR_RO(device_api);

static struct attribute *vfio_ap_mdev_type_attrs[] = {
	&mdev_type_attr_name.attr,
	&mdev_type_attr_device_api.attr,
	&mdev_type_attr_available_instances.attr,
	NULL,
};

static struct attribute_group vfio_ap_mdev_hwvirt_type_group = {
	.name = VFOP_AP_MDEV_TYPE_HWVIRT,
	.attrs = vfio_ap_mdev_type_attrs,
};

static struct attribute_group *vfio_ap_mdev_type_groups[] = {
	&vfio_ap_mdev_hwvirt_type_group,
	NULL,
};

static const struct mdev_parent_ops vfio_ap_matrix_ops = {
	.owner			= THIS_MODULE,
	.supported_type_groups	= vfio_ap_mdev_type_groups,
	.create			= vfio_ap_mdev_create,
	.remove			= vfio_ap_mdev_remove,
};

int vfio_ap_mdev_register(struct ap_matrix_dev *matrix_dev)
{
	int ret;

	ret = mdev_register_device(&matrix_dev->device, &vfio_ap_matrix_ops);
	if (ret)
		return ret;

	atomic_set(&matrix_dev->available_instances,
		   AP_MATRIX_MAX_AVAILABLE_INSTANCES);

	return 0;
}

void vfio_ap_mdev_unregister(struct ap_matrix_dev *matrix_dev)
{
	mdev_unregister_device(&matrix_dev->device);
}
