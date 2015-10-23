/*
 * The Industrial I/O core, software trigger functions
 *
 * Copyright (c) 2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/iio/sw_trigger.h>
#include <linux/configfs.h>

static struct config_group *iio_triggers_group;
static struct config_item_type iio_triggers_group_type = {
	.ct_owner = THIS_MODULE,
};

static LIST_HEAD(iio_trigger_types_list);
static DEFINE_MUTEX(iio_trigger_types_lock);

static
struct iio_sw_trigger_type *__iio_find_sw_trigger_type(const char *name,
						       unsigned len)
{
	struct iio_sw_trigger_type *t = NULL, *iter;

	list_for_each_entry(iter, &iio_trigger_types_list, list)
		if (!strcmp(iter->name, name)) {
			t = iter;
			break;
		}

	return t;
}

int iio_register_sw_trigger_type(struct iio_sw_trigger_type *t)
{
	struct iio_sw_trigger_type *iter;
	int ret = 0;

	mutex_lock(&iio_trigger_types_lock);
	iter = __iio_find_sw_trigger_type(t->name, strlen(t->name));
	if (iter)
		ret = -EBUSY;
	else
		list_add_tail(&t->list, &iio_trigger_types_list);
	mutex_unlock(&iio_trigger_types_lock);

	if (!ret)
		iio_sw_trigger_type_configfs_register(t);

	return ret;
}
EXPORT_SYMBOL(iio_register_sw_trigger_type);

void iio_unregister_sw_trigger_type(struct iio_sw_trigger_type *t)
{
	struct iio_sw_trigger_type *iter;

	mutex_lock(&iio_trigger_types_lock);
	iter = __iio_find_sw_trigger_type(t->name, strlen(t->name));
	if (iter)
		list_del(&t->list);
	mutex_unlock(&iio_trigger_types_lock);

	iio_sw_trigger_type_configfs_unregister(t);
}
EXPORT_SYMBOL(iio_unregister_sw_trigger_type);

static
struct iio_sw_trigger_type *iio_get_sw_trigger_type(const char *name)
{
	struct iio_sw_trigger_type *t;

	mutex_lock(&iio_trigger_types_lock);
	t = __iio_find_sw_trigger_type(name, strlen(name));
	if (t && !try_module_get(t->owner))
		t = NULL;
	mutex_unlock(&iio_trigger_types_lock);

	return t;
}

struct iio_sw_trigger *iio_sw_trigger_create(const char *type, const char *name)
{
	struct iio_sw_trigger *t;
	struct iio_sw_trigger_type *tt;

	tt = iio_get_sw_trigger_type(type);
	if (!tt) {
		pr_err("Invalid trigger type: %s\n", type);
		return ERR_PTR(-EINVAL);
	}
	t = tt->ops->probe(name);
	if (IS_ERR(t))
		goto out_module_put;

	t->trigger_type = tt;

	return t;
out_module_put:
	module_put(tt->owner);
	return t;
}
EXPORT_SYMBOL(iio_sw_trigger_create);

void iio_sw_trigger_destroy(struct iio_sw_trigger *t)
{
	struct iio_sw_trigger_type *tt = t->trigger_type;

	tt->ops->remove(t);
	module_put(tt->owner);
}
EXPORT_SYMBOL(iio_sw_trigger_destroy);

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

static struct config_item_type iio_trigger_type_group_type = {
	.ct_group_ops = &trigger_ops,
	.ct_owner       = THIS_MODULE,
};

int iio_sw_trigger_type_configfs_register(struct iio_sw_trigger_type *tt)
{
	int ret;

	tt->group = configfs_alloc_group(tt->name,
					 &iio_trigger_type_group_type);
	if (IS_ERR(tt->group))
		return PTR_ERR(tt->group);

	ret = configfs_register_group(iio_triggers_group, tt->group);
	if (ret)
		configfs_free_group(tt->group);
	return ret;
}
EXPORT_SYMBOL_GPL(iio_sw_trigger_type_configfs_register);

void iio_sw_trigger_type_configfs_unregister(struct iio_sw_trigger_type *tt)
{
	configfs_unregister_group(tt->group);
	configfs_free_group(tt->group);
}
EXPORT_SYMBOL_GPL(iio_sw_trigger_type_configfs_unregister);

static int __init iio_sw_trigger_init(void)
{
	int ret;

	iio_triggers_group = configfs_alloc_group("triggers",
						  &iio_triggers_group_type);
	if (IS_ERR(iio_triggers_group))
		return PTR_ERR(iio_triggers_group);

	ret = configfs_register_group(&iio_configfs_subsys.su_group,
				      iio_triggers_group);
	if (ret)
		configfs_free_group(iio_triggers_group);

	return ret;
}
module_init(iio_sw_trigger_init);

static void __exit iio_sw_trigger_exit(void)
{
	configfs_unregister_group(iio_triggers_group);
	configfs_free_group(iio_triggers_group);
}
module_exit(iio_sw_trigger_exit);

MODULE_AUTHOR("Daniel Baluta <daniel.baluta@intel.com>");
MODULE_DESCRIPTION("Industrial I/O software triggers support");
MODULE_LICENSE("GPL v2");
