/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include "util.h"

#if defined(CONFIG_DEBUG_FS)

static int bus1_debugfs_atomic_t_get(void *data, u64 *val)
{
	*val = atomic_read((atomic_t *)data);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(bus1_debugfs_atomic_x_ro,
			 bus1_debugfs_atomic_t_get,
			 NULL,
			 "%llx\n");

/**
 * bus1_debugfs_create_atomic_x() - create debugfs file for hex atomic_t
 * @name:	file name to use
 * @mode:	permissions for the file
 * @parent:	parent directory
 * @value:	variable to read from, or write to
 *
 * This is almost equivalent to debugfs_create_atomic_t() but prints/reads the
 * data as hexadecimal value. So far, only read-only attributes are supported.
 *
 * Return: Pointer to new dentry, NULL/ERR_PTR if disabled or on failure.
 */
struct dentry *bus1_debugfs_create_atomic_x(const char *name,
					    umode_t mode,
					    struct dentry *parent,
					    atomic_t *value)
{
	return debugfs_create_file_unsafe(name, mode, parent, value,
					  &bus1_debugfs_atomic_x_ro);
}

#endif /* defined(CONFIG_DEBUG_FS) */
