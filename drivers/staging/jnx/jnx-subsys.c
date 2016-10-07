/*
 * Juniper Generic APIs for providing chassis and card information
 *
 * Copyright (C) 2012, 2013, 2014 Juniper Networks. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>

#include <linux/jnx/jnx-subsys.h>
#include <linux/jnx/jnx-board-core.h>

#include "jnx-subsys-private.h"

#define DRIVER_VERSION  "0.01.0"
#define DRIVER_AUTHOR   "Thomas Kavanagh"
#define DRIVER_DESC     "JNX Subsystem"

static struct platform_device *jnx_platform_device;
static struct platform_device *jnx_local_card_device;

static struct jnx_chassis_info chassis_info;

/*
 * Linked list to hold info on all inserted boards.
 */
struct jnx_board_entry {
	struct platform_device *pdev;
	struct device *dev;
	struct list_head list;
};

static LIST_HEAD(jnx_board_list);
static DEFINE_SPINLOCK(jnx_board_list_lock);

/*
 * Chassis Attributes
 *
 * platform - identifies the product upon which we are running
 * chassis_no - the chassis number, used mainly in multi-chassis systems
 * multichassis - indicates whether or not this chassis is part of a
 *                multichassis system
 */
static ssize_t jnx_show_platform(struct device *dev,
				 struct device_attribute *da,
				 char *buf)
{
	return sprintf(buf, "%u\n", chassis_info.platform);
}

static ssize_t jnx_show_chassis_no(struct device *dev,
				   struct device_attribute *da,
				   char *buf)
{
	return sprintf(buf, "%u\n", chassis_info.chassis_no);
}

static ssize_t jnx_show_multichassis(struct device *dev,
				     struct device_attribute *da,
				     char *buf)
{
	return sprintf(buf, "%u\n", chassis_info.multichassis);
}

/* Determine mastership status */
bool jnx_is_master(void)
{
	struct jnx_chassis_info *chinfo = &chassis_info;
	bool is_master = true;

	/* mastership_get() can be NULL when connector running on fpc */
	if (chinfo->mastership_get)
		is_master = chinfo->mastership_get(chinfo->master_data);

	return is_master;
}
EXPORT_SYMBOL(jnx_is_master);

/* mastership status notifier list and register/unregister functions */
static BLOCKING_NOTIFIER_HEAD(mastership_notifier_list);

int register_mastership_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mastership_notifier_list,
						nb);
}
EXPORT_SYMBOL(register_mastership_notifier);

int unregister_mastership_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mastership_notifier_list,
						  nb);
}
EXPORT_SYMBOL(unregister_mastership_notifier);

static ssize_t jnx_get_master(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	struct jnx_chassis_info *chinfo = &chassis_info;

	return sprintf(buf, "%d\n",
		       chinfo->get_master(chinfo->master_data));
}

static ssize_t jnx_mastership_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct jnx_chassis_info *chinfo = &chassis_info;

	return sprintf(buf, "%d\n",
		       chinfo->mastership_get(chinfo->master_data));
}

static ssize_t jnx_mastership_set(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct jnx_chassis_info *chinfo = &chassis_info;
	int val, err;
	bool mstrship_before_set, mstrship_after_set;
	char object[JNX_BRD_I2C_NAME_LEN + 8];
	char subobject[24];
	char arg0[13];   /* New mastership state */
	char *envp[4];

	err = kstrtoint(buf, 0, &val);
	if (err)
		return err;

	envp[0] = object;
	envp[1] = subobject;
	envp[2] = arg0;
	envp[3] = NULL;

	mstrship_before_set = chinfo->mastership_get(chinfo->master_data);
	chinfo->mastership_set(chinfo->master_data, val);
	mstrship_after_set = chinfo->mastership_get(chinfo->master_data);

	/*
	 * Notifier callback should only get called for the valid combinations
	 * once hw switchover has completed successfully. Calling it for the
	 * rest of the combinations is either harmful or redundant.
	 */
	if (mstrship_before_set != mstrship_after_set) {
		/* udev notification of mastership change */
		sprintf(object, "OBJECT=chassis");
		sprintf(subobject, "SUBOBJECT=mastership");
		sprintf(arg0, "ARG0=%s", mstrship_after_set ?
					 "master" : "standby");
		kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
		/* Notifier callback */
		blocking_notifier_call_chain(&mastership_notifier_list,
					     val, NULL);
	}

	return count;
}

static ssize_t jnx_mastership_ping(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct jnx_chassis_info *chinfo = &chassis_info;

	chinfo->mastership_ping(chinfo->master_data);

	return count;
}

static ssize_t jnx_mastership_alive_cnt_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct jnx_chassis_info *chinfo = &chassis_info;

	return sprintf(buf, "%d\n",
		       chinfo->mastership_count_get(chinfo->master_data));
}

static ssize_t jnx_mastership_alive_cnt_set(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct jnx_chassis_info *chinfo = &chassis_info;
	int val, err;

	err = kstrtoint(buf, 0, &val);
	if (err)
		return err;

	err = chinfo->mastership_count_set(chinfo->master_data, val);
	if (err)
		return err;

	return count;
}

static DEVICE_ATTR(platform, S_IRUGO, jnx_show_platform, NULL);
static DEVICE_ATTR(chassis_no, S_IRUGO, jnx_show_chassis_no, NULL);
static DEVICE_ATTR(multichassis, S_IRUGO, jnx_show_multichassis, NULL);
static DEVICE_ATTR(master, S_IRUGO, jnx_get_master, NULL);
static DEVICE_ATTR(mastership, S_IRUGO | S_IWUSR, jnx_mastership_show,
		   jnx_mastership_set);
static DEVICE_ATTR(mastership_alive, S_IWUSR, NULL, jnx_mastership_ping);
static DEVICE_ATTR(mastership_alive_cnt, S_IRUGO | S_IWUSR,
		   jnx_mastership_alive_cnt_show, jnx_mastership_alive_cnt_set);

static struct attribute *jnx_chassis_attrs[] = {
	&dev_attr_platform.attr,
	&dev_attr_chassis_no.attr,
	&dev_attr_multichassis.attr,
	&dev_attr_master.attr,			/* 3 */
	&dev_attr_mastership.attr,		/* 4 */
	&dev_attr_mastership_alive.attr,	/* 5 */
	&dev_attr_mastership_alive_cnt.attr,	/* 6 */
	NULL
};

static umode_t jnx_chassis_is_visible(struct kobject *kobj,
				      struct attribute *attr, int index)
{
	struct jnx_chassis_info *chinfo = &chassis_info;

	if (index == 3 && !chinfo->get_master)
		return 0;
	if (index == 4 && (!chinfo->mastership_get || !chinfo->mastership_set))
		return 0;
	if (index == 5 && !chinfo->mastership_ping)
		return 0;
	if (index == 6 && (!chinfo->mastership_count_get ||
			   !chinfo->mastership_count_set))
		return 0;

	return attr->mode;
}

static const struct attribute_group jnx_chassis_group = {
	.name = "chassis",
	.attrs = jnx_chassis_attrs,
	.is_visible = jnx_chassis_is_visible,
};

/*
 * Card attributes
 *
 * slot - slot number for the given board
 * type - what type of board is inserted: RE, FPC, FAN, etc
 */
static ssize_t jnx_show_slot(struct device *dev,
			     struct device_attribute *da,
			     char *buf)
{
	struct jnx_card_info *cinfo = dev_get_platdata(dev);

	return sprintf(buf, "%d\n", cinfo->slot);
}

static ssize_t jnx_show_type(struct device *dev,
			     struct device_attribute *da,
			     char *buf)
{
	struct jnx_card_info *cinfo = dev_get_platdata(dev);

	return sprintf(buf, "%u\n", cinfo->type);
}

static ssize_t jnx_show_assembly_id(struct device *dev,
				    struct device_attribute *da,
				    char *buf)
{
	struct jnx_card_info *cinfo = dev_get_platdata(dev);

	return sprintf(buf, "0x%04x\n", cinfo->assembly_id);
}

static ssize_t jnx_show_warmboot(struct device *dev,
				 struct device_attribute *da,
				 char *buf)
{
	return sprintf(buf, "%u\n", jnx_warmboot());
}

static DEVICE_ATTR(slot, S_IRUGO, jnx_show_slot, NULL);
static DEVICE_ATTR(type, S_IRUGO, jnx_show_type, NULL);
static DEVICE_ATTR(assembly_id, S_IRUGO, jnx_show_assembly_id, NULL);
static DEVICE_ATTR(warmboot, S_IRUGO, jnx_show_warmboot, NULL);

/* Card attributes  */
static struct attribute *jnx_card_attrs[] = {
	&dev_attr_slot.attr,
	&dev_attr_type.attr,
	&dev_attr_assembly_id.attr,
	NULL
};

static const struct attribute_group jnx_card_group = {
	.attrs	= jnx_card_attrs,
};

static const struct attribute_group *jnx_card_groups[] = {
	&jnx_card_group,
	NULL
};

/* With addtional card attributes for 'local' */
static struct attribute *jnx_card_local_attrs[] = {
	&dev_attr_warmboot.attr,
	NULL
};

static const struct attribute_group jnx_local_card_group = {
	.attrs	= jnx_card_local_attrs
};

static const struct attribute_group *jnx_local_card_groups[] = {
	&jnx_card_group,
	&jnx_local_card_group,
	NULL
};

static struct attribute *jnx_attrs[] = {
	NULL
};

static const struct attribute_group jnx_group = {
	.name = "card",
	.attrs = jnx_attrs,
};

static const struct attribute_group *jnx_groups[] = {
	&jnx_group,
	NULL
};

static int jnx_platform_uevent(const char *dir, const char *obj,
			       const char *subobj, int event)
{
	struct kobject *kobj = &jnx_platform_device->dev.kobj;
	char objpath[64];
	char object[JNX_BRD_I2C_NAME_LEN + 8];
	char subobject[20];
	char *envp[4];
	const char *devpath;
	int ret;

	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath)
		return -ENOENT;

	envp[0] = objpath;
	envp[1] = object;
	envp[2] = subobject;
	envp[3] = NULL;

	if (dir)
		snprintf(objpath, sizeof(objpath), "OBJPATH=%s/%s", devpath,
			 dir);
	else
		snprintf(objpath, sizeof(objpath), "OBJPATH=%s", devpath);
	snprintf(object, sizeof(object), "OBJECT=%s", obj);

	if (subobj)
		snprintf(subobject, sizeof(subobject), "SUBOBJECT=%s", subobj);
	else
		sprintf(subobject, "SUBOBJECT=");

	ret = kobject_uevent_env(kobj, event, envp);
	kfree(devpath);
	return ret;
}

static int jnx_subsystem_init_pdev(void)
{
	int err;

	if (jnx_platform_device)
		return 0;	/* already initialized */

	jnx_platform_device = platform_device_alloc("jnx", -1);
	if (!jnx_platform_device)
		return -ENOMEM;

	jnx_platform_device->dev.groups = jnx_groups;
	err = platform_device_add(jnx_platform_device);
	if (err)
		goto err_free_device;

	return 0;

err_free_device:
	platform_device_put(jnx_platform_device);

	return err;
}

int jnx_register_chassis(struct jnx_chassis_info *chinfo)
{
	int ret;
	char object[JNX_BRD_I2C_NAME_LEN + 8];
	char subobject[24];
	char arg0[13];
	char *envp[4];

	if (!jnx_platform_device) {
		ret = jnx_subsystem_init_pdev();
		if (ret)
			return ret;
	}

	envp[0] = object;
	envp[1] = subobject;
	envp[2] = arg0;
	envp[3] = NULL;

	chassis_info = *chinfo;

	ret = sysfs_create_group(&jnx_platform_device->dev.kobj,
				 &jnx_chassis_group);
	if (ret < 0)
		return ret;

	jnx_platform_uevent(NULL, "chassis", NULL, KOBJ_ADD);
	if (chinfo->mastership_get) {
		/* notify udev of mastership sysfs attr creation */
		sprintf(arg0, "ARG0=%s",
			chinfo->mastership_get(chinfo->master_data) ? "master" :
			"standby");
		sprintf(object, "OBJECT=chassis");
		sprintf(subobject, "SUBOBJECT=mastership");
		kobject_uevent_env(&jnx_platform_device->dev.kobj,
				   KOBJ_ADD, envp);
	}

	return 0;
}
EXPORT_SYMBOL(jnx_register_chassis);

void jnx_unregister_chassis(void)
{
	if (jnx_platform_device) {
		sysfs_remove_group(&jnx_platform_device->dev.kobj,
				   &jnx_chassis_group);
		jnx_platform_uevent(NULL, "chassis", NULL, KOBJ_REMOVE);
	}
}
EXPORT_SYMBOL(jnx_unregister_chassis);

static
struct platform_device *jnx_create_card_device(char *name,
					       struct jnx_card_info *cinfo,
					       int id)
{
	struct platform_device *pdev;
	int err;

	pdev = platform_device_alloc(name, id);
	if (!pdev)
		return ERR_PTR(-ENOMEM);

	err = platform_device_add_data(pdev, cinfo, sizeof(*cinfo));
	if (err)
		goto pdev_failure;

	if (jnx_platform_device)
		pdev->dev.parent = &jnx_platform_device->dev;

	if (id != -1)
		pdev->dev.groups = jnx_card_groups;
	else
		pdev->dev.groups = jnx_local_card_groups;

	err = platform_device_add(pdev);
	if (err)
		goto pdev_failure;

	return pdev;

pdev_failure:
	platform_device_put(pdev);
	return ERR_PTR(err);
}

int jnx_sysfs_create_link(struct device *dev, const char *link)
{
	int ret = 0;

	if (jnx_platform_device) {
		ret = sysfs_add_link_to_group(&jnx_platform_device->dev.kobj,
					      "card", &dev->kobj, link);
		if (!ret)
			ret = jnx_platform_uevent("card", link, NULL, KOBJ_ADD);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(jnx_sysfs_create_link);

void jnx_sysfs_delete_link(struct device *dev, const char *link)
{
	if (jnx_platform_device) {
		sysfs_remove_link_from_group(&jnx_platform_device->dev.kobj,
					     "card", link);
		jnx_platform_uevent("card", link, NULL, KOBJ_REMOVE);
	}
}
EXPORT_SYMBOL_GPL(jnx_sysfs_delete_link);

/*
 * Register local card. This is the card we are running on.
 * Typically this would be the RE or a PMB.
 * Create a card device similar to other card devices.
 * Also create the 'local' link in the 'card' directory.
 */
int jnx_register_local_card(struct jnx_card_info *cinfo)
{
	char name[JNX_BRD_I2C_NAME_LEN];

	if (jnx_local_card_device)
		return -EEXIST;

	snprintf(name, sizeof(name), "jnx-%04x-local", cinfo->assembly_id);
	jnx_local_card_device = jnx_create_card_device(name, cinfo, -1);
	if (IS_ERR(jnx_local_card_device))
		return PTR_ERR(jnx_local_card_device);

	jnx_sysfs_create_link(&jnx_local_card_device->dev, "local");

	return 0;
}
EXPORT_SYMBOL(jnx_register_local_card);

void jnx_unregister_local_card(void)
{
	if (jnx_local_card_device) {
		jnx_sysfs_delete_link(&jnx_local_card_device->dev, "local");
		platform_device_unregister(jnx_local_card_device);
		jnx_local_card_device = NULL;
	}
}
EXPORT_SYMBOL(jnx_unregister_local_card);

int jnx_register_board(struct device *dev, struct device *ideeprom,
		       struct jnx_card_info *cinfo, int id)
{
	char name[JNX_BRD_I2C_NAME_LEN];
	struct platform_device *pdev;
	struct jnx_board_entry *entry;
	int err;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	snprintf(name, sizeof(name), "jnx-%04x", cinfo->assembly_id);
	pdev = jnx_create_card_device(name, cinfo, id);
	if (IS_ERR(pdev)) {
		kfree(entry);
		return PTR_ERR(pdev);
	}

	if (ideeprom) {
		err = sysfs_create_link(&pdev->dev.kobj, &ideeprom->kobj, "id");
		if (err)
			dev_err(&pdev->dev,
				"Failed to create link to ID eeprom\n");
	}

	if (cinfo->adap) {
		err = sysfs_create_link(&pdev->dev.kobj,
					&cinfo->adap->dev.kobj, "i2c-adapter");
		if (err)
			dev_err(&pdev->dev,
				"Failed to create link to i2c adapter\n");
	}
	entry->pdev = pdev;
	entry->dev = dev;

	spin_lock(&jnx_board_list_lock);
	list_add_tail(&entry->list, &jnx_board_list);
	spin_unlock(&jnx_board_list_lock);

	return 0;
}
EXPORT_SYMBOL(jnx_register_board);

static struct jnx_board_entry *jnx_find_board_entry_by_dev(struct device *dev)
{
	struct jnx_board_entry *brd_entry;

	spin_lock(&jnx_board_list_lock);

	list_for_each_entry(brd_entry, &jnx_board_list, list) {
		/*
		 * Device is either the device stored in brd_entry,
		 * or its parent (if there is a channel enable mux
		 * on the board).
		 */
		if (brd_entry->dev == dev || brd_entry->dev->parent == dev)
			goto found;
	}

	brd_entry = NULL;

found:
	spin_unlock(&jnx_board_list_lock);

	return brd_entry;
}

int jnx_unregister_board(struct device *dev)
{
	struct jnx_board_entry *entry;

	entry = jnx_find_board_entry_by_dev(dev);
	if (!entry)
		return -ENODEV;

	if (!list_empty(&jnx_board_list)) {
		spin_lock(&jnx_board_list_lock);
		list_del(&entry->list);
		spin_unlock(&jnx_board_list_lock);
	}

	sysfs_remove_link(&entry->pdev->dev.kobj, "id");
	sysfs_remove_link(&entry->pdev->dev.kobj, "i2c-adapter");
	platform_device_unregister(entry->pdev);

	kfree(entry);

	return 0;
}
EXPORT_SYMBOL(jnx_unregister_board);

subsys_initcall(jnx_subsystem_init_pdev);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
