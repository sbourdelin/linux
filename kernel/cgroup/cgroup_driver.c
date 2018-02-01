/* SPDX-License-Identifier: GPL-2.0 */

#include "cgroup-internal.h"

#include <linux/hashtable.h>
#include <linux/mm.h>

/*
 * General data structure returned by cgroup_driver_init() and used as a
 * hashtable key to lookup driver-specific data.
 */
struct cgroup_driver {
	/* Functions this driver uses to manage its data */
	struct cgroup_driver_funcs *funcs;

	/*
	 * List of driver-specific data structures that need to be cleaned up
	 * if driver is unloaded.
	 */
	struct list_head datalist;
};

/**
 * cgroup_driver_init - initialize cgroups driver-specific data management
 * @funcs: driver-specific data management functions
 *
 * Drivers that wish to store driver-specific data alongside individual
 * cgroups should call this and provide a function table of driver-specific
 * data operations.
 *
 * RETURNS:
 * An instance of 'struct cgroup_driver' that will be used to help manage
 * data storage for the invoking driver.  If an error occurs, a negative
 * error code will be returned.  If CONFIG_CGROUP_DRIVER is not set, NULL
 * will be returned.
 */
struct cgroup_driver *
cgroup_driver_init(struct cgroup_driver_funcs *funcs)
{
	struct cgroup_driver *drv;

	drv = kzalloc(sizeof *drv, GFP_KERNEL);
	if (!drv)
		return ERR_PTR(-ENOMEM);

	drv->funcs = funcs;
	INIT_LIST_HEAD(&drv->datalist);

	return drv;
}
EXPORT_SYMBOL(cgroup_driver_init);

/**
 * cgroup_driver_release - release all driver-specific data for a driver
 * @drv: driver to release data for
 *
 * Drivers storing their own data alongside cgroups should call this function
 * when unloaded to ensure all driver-specific data is released.
 */
void
cgroup_driver_release(struct cgroup_driver *drv)
{
	struct cgroup_driver_data *data, *tmp;

	mutex_lock(&cgroup_mutex);
	list_for_each_entry_safe(data, tmp, &drv->datalist, drivernode) {
		hlist_del(&data->cgroupnode);
		list_del(&data->drivernode);
		if (drv->funcs && drv->funcs->free_data)
			drv->funcs->free_data(data);
		else
			kvfree(data);
	}
	mutex_unlock(&cgroup_mutex);

	kfree(drv);
}
EXPORT_SYMBOL(cgroup_driver_release);

/**
 * cgroup_driver_get_data - retrieve/allocate driver-specific data for a cgroup
 * @drv: driver wishing to fetch data
 * @cgrp: cgroup to fetch data for
 * @is_new: will be set to true if a new structure is allocated
 *
 * Fetches the driver-specific data structure associated with a cgroup, if one
 * has previously been set.  If no driver data has been associated with this
 * cgroup, a new driver-specific data structure is allocated and returned.
 *
 * RETURNS:
 * The driver data previously associated with this cgroup, or a fresh data
 * structure allocated via drv->funcs->alloc_data() if no data has previously
 * been associated.  On error, a negative error code is returned.
 */
struct cgroup_driver_data *
cgroup_driver_get_data(struct cgroup_driver *drv,
		       struct cgroup *cgrp,
		       bool *is_new)
{
	struct cgroup_driver_data *data;

	/* We only support driver-specific data on the cgroup-v2 hierarchy */
	if (!cgroup_on_dfl(cgrp))
		return ERR_PTR(-EINVAL);

	mutex_lock(&cgroup_mutex);

	if (is_new)
		*is_new = false;
	hash_for_each_possible(cgrp->driver_data, data, cgroupnode,
			       (unsigned long)drv)
		if (data->drv == drv)
			goto out;

	/* First time for this cgroup; alloc and store new data */
	data = drv->funcs->alloc_data(drv);
	if (!IS_ERR(data)) {
		data->drv = drv;
		hash_add(cgrp->driver_data, &data->cgroupnode,
			 (unsigned long)drv);
		list_add(&data->drivernode, &drv->datalist);
		if (is_new)
			*is_new = true;
	}

out:
	mutex_unlock(&cgroup_mutex);
	return data;
}
EXPORT_SYMBOL(cgroup_driver_get_data);

/**
 * cgroup_for_driver_process - return the cgroup for a process
 * @pid: process to lookup cgroup for
 *
 * Returns the cgroup from the v2 hierarchy that a process belongs to.
 * This function is intended to be called from drivers and will obtain
 * the necessary cgroup locks.
 *
 * RETURNS:
 * Process' cgroup in the default (v2) hierarchy
 */
struct cgroup *
cgroup_for_driver_process(struct pid *pid)
{
	struct task_struct *task = pid_task(pid, PIDTYPE_PID);

	mutex_lock(&cgroup_mutex);
	spin_lock_irq(&css_set_lock);
	task_cgroup_from_root(task, &cgrp_dfl_root);
	spin_unlock_irq(&css_set_lock);
	mutex_unlock(&cgroup_mutex);
}
EXPORT_SYMBOL(cgroup_for_driver_process);
