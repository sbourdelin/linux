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

int iio_sw_trigger_type_configfs_register(struct iio_sw_trigger_type *tt)
{
#ifdef CONFIG_IIO_CONFIGFS
	config_group_init_type_name(&tt->group, tt->name,
				    &iio_trigger_type_group_type);

	return configfs_register_group(&iio_triggers_group, &tt->group);
#else
	return 0;
#endif
}
EXPORT_SYMBOL_GPL(iio_sw_trigger_type_configfs_register);

void iio_sw_trigger_type_configfs_unregister(struct iio_sw_trigger_type *tt)
{
#ifdef CONFIG_IIO_CONFIGFS
	configfs_unregister_group(&tt->group);
#endif
}
EXPORT_SYMBOL_GPL(iio_sw_trigger_type_configfs_unregister);

