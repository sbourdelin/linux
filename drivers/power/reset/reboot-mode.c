/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include "reboot-mode.h"

#define PREFIX "mode-"

struct mode_info {
	const char *mode;
	unsigned int magic;
	struct list_head list;
};

static int get_reboot_mode_magic(struct reboot_mode_driver *reboot,
				 const char *cmd)
{
	const char *normal = "normal";
	int magic = 0;
	struct mode_info *info;

	if (!cmd)
		cmd = normal;

	list_for_each_entry(info, &reboot->head, list) {
		if (!strcmp(info->mode, cmd)) {
			magic = info->magic;
			break;
		}
	}

	return magic;
}

static int reboot_mode_notify(struct notifier_block *this,
			      unsigned long mode, void *cmd)
{
	struct reboot_mode_driver *reboot;
	int magic;

	reboot = container_of(this, struct reboot_mode_driver, reboot_notifier);
	magic = get_reboot_mode_magic(reboot, cmd);
	if (magic)
		reboot->write(reboot, magic);

	return NOTIFY_DONE;
}

int reboot_mode_register(struct reboot_mode_driver *reboot)
{
	struct mode_info *info;
	struct property *prop;
	struct device_node *np = reboot->dev->of_node;
	size_t len = strlen(PREFIX);
	int ret;

	INIT_LIST_HEAD(&reboot->head);

	for_each_property_of_node(np, prop) {
		if (len > strlen(prop->name) || strncmp(prop->name, PREFIX, len))
			continue;

		info = devm_kzalloc(reboot->dev, sizeof(*info), GFP_KERNEL);
		if (!info) {
			ret = -ENOMEM;
			goto error;
		}

		info->mode = kstrdup_const(prop->name + len, GFP_KERNEL);
		if (of_property_read_u32(np, prop->name, &info->magic)) {
			dev_err(reboot->dev, "reboot mode %s without magic number\n",
				info->mode);
			devm_kfree(reboot->dev, info);
			continue;
		}
		list_add_tail(&info->list, &reboot->head);
	}

	reboot->reboot_notifier.notifier_call = reboot_mode_notify;
	ret = register_reboot_notifier(&reboot->reboot_notifier);
	if (ret)
		dev_err(reboot->dev, "can't register reboot notifier\n");

	return ret;

error:
	list_for_each_entry(info, &reboot->head, list)
		kfree_const(info->mode);

	return ret;
}

int reboot_mode_unregister(struct reboot_mode_driver *reboot)
{
	struct mode_info *info;

	unregister_reboot_notifier(&reboot->reboot_notifier);

	list_for_each_entry(info, &reboot->head, list)
		kfree_const(info->mode);

	return 0;
}

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com");
MODULE_DESCRIPTION("System reboot mode driver");
MODULE_LICENSE("GPL v2");
