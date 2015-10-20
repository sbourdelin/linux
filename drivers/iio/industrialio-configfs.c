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
#include <linux/iio/sw_trigger.h>

static struct config_group *trigger_make_group(struct config_group *group,
					       const char *name)
{
	struct iio_sw_trigger *t;

	t = iio_sw_trigger_create(group->cg_item.ci_name, name);
	if (IS_ERR(t))
		return ERR_CAST(t);

	config_item_set_name(&t->group.cg_item, "%s", name);

	return &t->group;
}

static void trigger_drop_group(struct config_group *group,
			       struct config_item *item)
{
	struct iio_sw_trigger *t = to_iio_sw_trigger(item);

	iio_sw_trigger_destroy(t);
	config_item_put(item);
}

static struct configfs_group_operations trigger_ops = {
	.make_group	= &trigger_make_group,
	.drop_item	= &trigger_drop_group,
};

struct config_item_type iio_trigger_type_group_type = {
	.ct_group_ops = &trigger_ops,
	.ct_owner       = THIS_MODULE,
};
EXPORT_SYMBOL(iio_trigger_type_group_type);

struct config_item_type iio_triggers_group_type = {
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
