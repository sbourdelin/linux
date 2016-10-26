/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include "main.h"
#include "tests.h"
#include "user.h"

static int bus1_fop_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int bus1_fop_release(struct inode *inode, struct file *file)
{
	return 0;
}

const struct file_operations bus1_fops = {
	.owner			= THIS_MODULE,
	.open			= bus1_fop_open,
	.release		= bus1_fop_release,
	.llseek			= noop_llseek,
};

static struct miscdevice bus1_misc = {
	.fops			= &bus1_fops,
	.minor			= MISC_DYNAMIC_MINOR,
	.name			= KBUILD_MODNAME,
	.mode			= S_IRUGO | S_IWUGO,
};

struct dentry *bus1_debugdir;

static int __init bus1_modinit(void)
{
	int r;

	r = bus1_tests_run();
	if (r < 0)
		return r;

	bus1_debugdir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (!bus1_debugdir)
		pr_err("cannot create debugfs root\n");

	r = misc_register(&bus1_misc);
	if (r < 0)
		goto error;

	pr_info("loaded\n");
	return 0;

error:
	debugfs_remove(bus1_debugdir);
	bus1_user_modexit();
	return r;
}

static void __exit bus1_modexit(void)
{
	misc_deregister(&bus1_misc);
	debugfs_remove(bus1_debugdir);
	bus1_user_modexit();
	pr_info("unloaded\n");
}

module_init(bus1_modinit);
module_exit(bus1_modexit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bus based interprocess communication");
