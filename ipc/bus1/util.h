#ifndef __BUS1_UTIL_H
#define __BUS1_UTIL_H

/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

/**
 * Utilities
 *
 * Random utility functions that don't belong to a specific object. Some of
 * them are copies from internal kernel functions (which lack an export
 * annotation), some of them are variants of internal kernel functions, and
 * some of them are our own.
 */

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct dentry;

#if defined(CONFIG_DEBUG_FS)

struct dentry *
bus1_debugfs_create_atomic_x(const char *name,
			     umode_t mode,
			     struct dentry *parent,
			     atomic_t *value);

#else

static inline struct dentry *
bus1_debugfs_create_atomic_x(const char *name,
			     umode_t mode,
			     struct dentry *parent,
			     atomic_t *value)
{
	return ERR_PTR(-ENODEV);
}

#endif

#endif /* __BUS1_UTIL_H */
