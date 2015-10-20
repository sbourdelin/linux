/*
 * Industrial I/O configfs bits
 *
 * Copyright (c) 2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/slab.h>

#include <linux/iio/iio.h>

static struct config_item_type iio_triggers_group_type = {
	.ct_owner = THIS_MODULE,
};

struct config_group iio_triggers_group = {
	.cg_item = {
		.ci_namebuf = "triggers",
		.ci_type = &iio_triggers_group_type,
	},
};
EXPORT_SYMBOL(iio_triggers_group);

static struct config_group *iio_root_default_groups[] = {
	&iio_triggers_group,
	NULL
};

static struct config_item_type iio_root_group_type = {
	.ct_owner       = THIS_MODULE,
};

static struct configfs_subsystem iio_configfs_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "iio",
			.ci_type = &iio_root_group_type,
		},
		.default_groups = iio_root_default_groups,
	},
	.su_mutex = __MUTEX_INITIALIZER(iio_configfs_subsys.su_mutex),
};

static int __init iio_configfs_init(void)
{
	config_group_init(&iio_triggers_group);
	config_group_init(&iio_configfs_subsys.su_group);

	return configfs_register_subsystem(&iio_configfs_subsys);
}
module_init(iio_configfs_init);

static void __exit iio_configfs_exit(void)
{
	configfs_unregister_subsystem(&iio_configfs_subsys);
}
module_exit(iio_configfs_exit);

MODULE_AUTHOR("Daniel Baluta <daniel.baluta@intel.com>");
MODULE_DESCRIPTION("Industrial I/O configfs support");
MODULE_LICENSE("GPL v2");
