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
#define KVM_AP_MASK_BYTES(n) DIV_ROUND_UP(n, BITS_PER_BYTE)

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

struct vfio_ap_qid_reserved {
	ap_qid_t qid;
	bool reserved;
};

struct vfio_id_reserved {
	unsigned long id;
	bool reserved;
};

/**
 * vfio_ap_qid_reserved
 *
 * @dev: an AP queue device
 * @data: a queue ID
 *
 * Flags whether any AP queue device has a particular qid
 *
 * Returns 0 to indicate the function succeeded
 */
static int vfio_ap_queue_has_qid(struct device *dev, void *data)
{
	struct vfio_ap_qid_reserved *qid_res = data;
	struct ap_queue *ap_queue = to_ap_queue(dev);

	if (qid_res->qid == ap_queue->qid)
		qid_res->reserved = true;

	return 0;
}

/**
 * vfio_ap_queue_has_apid
 *
 * @dev: an AP queue device
 * @data: an AP adapter ID
 *
 * Flags whether any AP queue device has a particular AP adapter ID
 *
 * Returns 0 to indicate the function succeeded
 */
static int vfio_ap_queue_has_apid(struct device *dev, void *data)
{
	struct vfio_id_reserved *id_res = data;
	struct ap_queue *ap_queue = to_ap_queue(dev);

	if (id_res->id == AP_QID_CARD(ap_queue->qid))
		id_res->reserved = true;

	return 0;
}

/**
 * vfio_ap_queue_has_apqi
 *
 * @dev: an AP queue device
 * @data: an AP queue index
 *
 * Flags whether any AP queue device has a particular AP queue index
 *
 * Returns 0 to indicate the function succeeded
 */
static int vfio_ap_queue_has_apqi(struct device *dev, void *data)
{
	struct vfio_id_reserved *id_res = data;
	struct ap_queue *ap_queue = to_ap_queue(dev);

	if (id_res->id == AP_QID_QUEUE(ap_queue->qid))
		id_res->reserved = true;

	return 0;
}

/**
 * vfio_ap_verify_qid_reserved
 *
 * @matrix_dev: a mediated matrix device
 * @qid: a qid (i.e., APQN)
 *
 * Verifies that the AP queue with @qid is reserved by the VFIO AP device
 * driver.
 *
 * Returns 0 if the AP queue with @qid is reserved; otherwise, returns -ENODEV.
 */
static int vfio_ap_verify_qid_reserved(struct ap_matrix_dev *matrix_dev,
				       ap_qid_t qid)
{
	int ret;
	struct vfio_ap_qid_reserved qid_res;

	qid_res.qid = qid;
	qid_res.reserved = false;

	ret = driver_for_each_device(matrix_dev->device.driver, NULL, &qid_res,
				     vfio_ap_queue_has_qid);
	if (ret)
		return ret;

	if (qid_res.reserved)
		return 0;

	return -EPERM;
}

/**
 * vfio_ap_verify_apid_reserved
 *
 * @matrix_dev: a mediated matrix device
 * @apid: an AP adapter ID
 *
 * Verifies that an AP queue with @apid is reserved by the VFIO AP device
 * driver.
 *
 * Returns 0 if an AP queue with @apid is reserved; otherwise, returns -ENODEV.
 */
static int vfio_ap_verify_apid_reserved(struct ap_matrix_dev *matrix_dev,
					const char *mdev_name,
					unsigned long apid)
{
	int ret;
	struct vfio_id_reserved id_res;

	id_res.id = apid;
	id_res.reserved = false;

	ret = driver_for_each_device(matrix_dev->device.driver, NULL, &id_res,
				     vfio_ap_queue_has_apid);
	if (ret)
		return ret;

	if (id_res.reserved)
		return 0;

	pr_err("%s: mdev %s using adapter %02lx not reserved by %s driver",
					VFIO_AP_MODULE_NAME, mdev_name, apid,
					VFIO_AP_DRV_NAME);

	return -EPERM;
}

/**
 * vfio_ap_verify_apqi_reserved
 *
 * @matrix_dev: a mediated matrix device
 * @apqi: an AP queue index
 *
 * Verifies that an AP queue with @apqi is reserved by the VFIO AP device
 * driver.
 *
 * Returns 0 if an AP queue with @apqi is reserved; otherwise, returns -ENODEV.
 */
static int vfio_ap_verify_apqi_reserved(struct ap_matrix_dev *matrix_dev,
					const char *mdev_name,
					unsigned long apqi)
{
	int ret;
	struct vfio_id_reserved id_res;

	id_res.id = apqi;
	id_res.reserved = false;

	ret = driver_for_each_device(matrix_dev->device.driver, NULL, &id_res,
				     vfio_ap_queue_has_apqi);
	if (ret)
		return ret;

	if (id_res.reserved)
		return 0;

	pr_err("%s: mdev %s using queue %04lx not reserved by %s driver",
					VFIO_AP_MODULE_NAME, mdev_name, apqi,
					VFIO_AP_DRV_NAME);

	return -EPERM;
}

static int vfio_ap_verify_queues_reserved(struct ap_matrix_dev *matrix_dev,
					  const char *mdev_name,
					  struct ap_matrix *matrix)
{
	unsigned long apid, apqi;
	int ret;
	int rc = 0;

	for_each_set_bit_inv(apid, matrix->apm, matrix->apm_max + 1) {
		for_each_set_bit_inv(apqi, matrix->aqm, matrix->aqm_max + 1) {
			ret = vfio_ap_verify_qid_reserved(matrix_dev,
							  AP_MKQID(apid, apqi));
			if (ret == 0)
				continue;

			/*
			 * We want to log every APQN that is not reserved by
			 * the driver, so record the return code, log a message
			 * and allow the loop to continue
			 */
			rc = ret;
			pr_err("%s: mdev %s using queue %02lx.%04lx not reserved by %s driver",
				VFIO_AP_MODULE_NAME, mdev_name, apid,
				apqi, VFIO_AP_DRV_NAME);
		}
	}

	return rc;
}

/**
 * vfio_ap_validate_apid
 *
 * @mdev: the mediated device
 * @matrix_mdev: the mediated matrix device
 * @apid: the APID to validate
 *
 * Validates the value of @apid:
 *	* If there are no AP domains assigned, then there must be at least
 *	  one AP queue device reserved by the VFIO AP device driver with an
 *	  APQN containing @apid.
 *
 *	* Else each APQN that can be derived from the intersection of @apid and
 *	  the IDs of the AP domains already assigned must identify an AP queue
 *	  that has been reserved by the VFIO AP device driver.
 *
 * Returns 0 if the value of @apid is valid; otherwise, returns an error.
 */
static int vfio_ap_validate_apid(struct mdev_device *mdev,
				 struct ap_matrix_mdev *matrix_mdev,
				 unsigned long apid)
{
	int ret;
	unsigned long aqmsz = matrix_mdev->matrix.aqm_max + 1;
	struct device *dev = mdev_parent_dev(mdev);
	struct ap_matrix_dev *matrix_dev = to_ap_matrix_dev(dev);
	struct ap_matrix matrix = matrix_mdev->matrix;

	/* If there are any queues assigned to the mediated device */
	if (find_first_bit_inv(matrix.aqm, aqmsz) < aqmsz) {
		matrix.apm_max = matrix_mdev->matrix.apm_max;
		memset(matrix.apm, 0,
		       ARRAY_SIZE(matrix.apm) * sizeof(matrix.apm[0]));
		set_bit_inv(apid, matrix.apm);
		matrix.aqm_max = matrix_mdev->matrix.aqm_max;
		memcpy(matrix.aqm, matrix_mdev->matrix.aqm,
		       ARRAY_SIZE(matrix.aqm) * sizeof(matrix.aqm[0]));
		ret = vfio_ap_verify_queues_reserved(matrix_dev,
						     matrix_mdev->name,
						     &matrix);
	} else {
		ret = vfio_ap_verify_apid_reserved(matrix_dev,
						   matrix_mdev->name, apid);
	}

	if (ret)
		return ret;

	return 0;
}

/**
 * assign_adapter_store
 *
 * @dev: the matrix device
 * @attr: a mediated matrix device attribute
 * @buf: a buffer containing the adapter ID (APID) to be assigned
 * @count: the number of bytes in @buf
 *
 * Parses the APID from @buf and assigns it to the mediated matrix device. The
 * APID must be a valid value:
 *	* The APID value must not exceed the maximum allowable AP adapter ID
 *
 *	* If there are no AP domains assigned, then there must be at least
 *	  one AP queue device reserved by the VFIO AP device driver with an
 *	  APQN containing @apid.
 *
 *	* Else each APQN that can be derived from the intersection of @apid and
 *	  the IDs of the AP domains already assigned must identify an AP queue
 *	  that has been reserved by the VFIO AP device driver.
 *
 * Returns the number of bytes processed if the APID is valid; otherwise returns
 * an error.
 */
static ssize_t assign_adapter_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret;
	unsigned long apid;
	struct mdev_device *mdev = mdev_from_dev(dev);
	struct ap_matrix_mdev *matrix_mdev = mdev_get_drvdata(mdev);
	unsigned long max_apid = matrix_mdev->matrix.apm_max;

	ret = kstrtoul(buf, 0, &apid);
	if (ret || (apid > max_apid)) {
		pr_err("%s: %s: adapter id '%s' not a value from 0 to %02lu(%#04lx)",
		       VFIO_AP_MODULE_NAME, __func__, buf, max_apid, max_apid);

		return ret ? ret : -EINVAL;
	}

	ret = vfio_ap_validate_apid(mdev, matrix_mdev, apid);
	if (ret)
		return ret;

	/* Set the bit in the AP mask (APM) corresponding to the AP adapter
	 * number (APID). The bits in the mask, from most significant to least
	 * significant bit, correspond to APIDs 0-255.
	 */
	set_bit_inv(apid, matrix_mdev->matrix.apm);

	return count;
}
static DEVICE_ATTR_WO(assign_adapter);

/**
 * unassign_adapter_store
 *
 * @dev: the matrix device
 * @attr: a mediated matrix device attribute
 * @buf: a buffer containing the adapter ID (APID) to be assigned
 * @count: the number of bytes in @buf
 *
 * Parses the APID from @buf and unassigns it from the mediated matrix device.
 * The APID must be a valid value
 *
 * Returns the number of bytes processed if the APID is valid; otherwise returns
 * an error.
 */
static ssize_t unassign_adapter_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int ret;
	unsigned long apid;
	struct mdev_device *mdev = mdev_from_dev(dev);
	struct ap_matrix_mdev *matrix_mdev = mdev_get_drvdata(mdev);
	unsigned long max_apid = matrix_mdev->matrix.apm_max;

	ret = kstrtoul(buf, 0, &apid);
	if (ret || (apid > max_apid)) {
		pr_err("%s: %s: adapter id '%s' must be a value from 0 to %02lu(%#04lx)",
		       VFIO_AP_MODULE_NAME, __func__, buf, max_apid, max_apid);

		return ret ? ret : -EINVAL;
	}

	if (!test_bit_inv(apid, matrix_mdev->matrix.apm)) {
		pr_err("%s: %s: adapter id %02lu(%#04lx) not assigned",
		       VFIO_AP_MODULE_NAME, __func__, apid, apid);

		return -ENODEV;
	}

	clear_bit_inv((unsigned long)apid, matrix_mdev->matrix.apm);

	return count;
}
DEVICE_ATTR_WO(unassign_adapter);

/**
 * vfio_ap_validate_apqi
 *
 * @matrix_mdev: the mediated matrix device
 * @apqi: the APQI (domain ID) to validate
 *
 * Validates the value of @apqi:
 *	* If there are no AP adapters assigned, then there must be at least
 *	  one AP queue device reserved by the VFIO AP device driver with an
 *	  APQN containing @apqi.
 *
 *	* Else each APQN that can be derived from the cross product of @apqi and
 *	  the IDs of the AP adapters already assigned must identify an AP queue
 *	  that has been reserved by the VFIO AP device driver.
 *
 * Returns 0 if the value of @apqi is valid; otherwise, returns an error.
 */
static int vfio_ap_validate_apqi(struct mdev_device *mdev,
				 struct ap_matrix_mdev *matrix_mdev,
				 unsigned long apqi)
{
	int ret;
	unsigned long apmsz = matrix_mdev->matrix.apm_max + 1;
	struct device *dev = mdev_parent_dev(mdev);
	struct ap_matrix_dev *matrix_dev = to_ap_matrix_dev(dev);
	struct ap_matrix matrix = matrix_mdev->matrix;

	/* If there are any adapters assigned to the mediated device */
	if (find_first_bit_inv(matrix.apm, apmsz) < apmsz) {
		matrix.apm_max = matrix_mdev->matrix.apm_max;
		memcpy(matrix.apm, matrix_mdev->matrix.apm,
		       ARRAY_SIZE(matrix.apm) * sizeof(matrix.apm[0]));
		matrix.aqm_max = matrix_mdev->matrix.aqm_max;
		memset(matrix.aqm, 0,
		       ARRAY_SIZE(matrix.aqm) * sizeof(matrix.aqm[0]));
		set_bit_inv(apqi, matrix.aqm);
		ret = vfio_ap_verify_queues_reserved(matrix_dev,
						     matrix_mdev->name,
						     &matrix);
	} else {
		ret = vfio_ap_verify_apqi_reserved(matrix_dev,
						   matrix_mdev->name, apqi);
	}

	if (ret)
		return ret;

	return 0;
}

static ssize_t assign_domain_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;
	unsigned long apqi;
	struct mdev_device *mdev = mdev_from_dev(dev);
	struct ap_matrix_mdev *matrix_mdev = mdev_get_drvdata(mdev);
	unsigned long max_apqi = matrix_mdev->matrix.aqm_max;

	ret = kstrtoul(buf, 0, &apqi);
	if (ret || (apqi > max_apqi)) {
		pr_err("%s: %s: domain id '%s' not a value from 0 to %02lu(%#04lx)",
		       VFIO_AP_MODULE_NAME, __func__, buf, max_apqi, max_apqi);

		return ret ? ret : -EINVAL;
	}

	ret = vfio_ap_validate_apqi(mdev, matrix_mdev, apqi);
	if (ret)
		return ret;

	/* Set the bit in the AQM (bitmask) corresponding to the AP domain
	 * number (APQI). The bits in the mask, from most significant to least
	 * significant, correspond to numbers 0-255.
	 */
	set_bit_inv(apqi, matrix_mdev->matrix.aqm);

	return count;
}
DEVICE_ATTR_WO(assign_domain);

static ssize_t unassign_domain_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned long apqi;
	struct mdev_device *mdev = mdev_from_dev(dev);
	struct ap_matrix_mdev *matrix_mdev = mdev_get_drvdata(mdev);
	unsigned long max_apqi = matrix_mdev->matrix.aqm_max;

	ret = kstrtoul(buf, 0, &apqi);
	if (ret || (apqi > max_apqi)) {
		pr_err("%s: %s: domain id '%s' not a value from 0 to %02lu(%#04lx)",
		       VFIO_AP_MODULE_NAME, __func__, buf, max_apqi, max_apqi);

		return ret ? ret : -EINVAL;
	}

	if (!test_bit_inv(apqi, matrix_mdev->matrix.aqm)) {
		pr_err("%s: %s: domain %02lu(%#04lx) not assigned",
		       VFIO_AP_MODULE_NAME, __func__, apqi, apqi);
		return -ENODEV;
	}

	clear_bit_inv((unsigned long)apqi, matrix_mdev->matrix.aqm);

	return count;
}
DEVICE_ATTR_WO(unassign_domain);

static struct attribute *vfio_ap_mdev_attrs[] = {
	&dev_attr_assign_adapter.attr,
	&dev_attr_unassign_adapter.attr,
	&dev_attr_assign_domain.attr,
	&dev_attr_unassign_domain.attr,
	NULL,
};

static struct attribute_group vfio_ap_mdev_attr_group = {
	.attrs = vfio_ap_mdev_attrs
};

static const struct attribute_group *vfio_ap_mdev_attr_groups[] = {
	&vfio_ap_mdev_attr_group,
	NULL
};

static const struct mdev_parent_ops vfio_ap_matrix_ops = {
	.owner			= THIS_MODULE,
	.supported_type_groups	= vfio_ap_mdev_type_groups,
	.mdev_attr_groups	= vfio_ap_mdev_attr_groups,
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
