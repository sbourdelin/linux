/*
 *  tracefs.h - a pseudo file system for activating tracing
 *
 * Based on debugfs by: 2004 Greg Kroah-Hartman <greg@kroah.com>
 *
 *  Copyright (C) 2014 Red Hat Inc, author: Steven Rostedt <srostedt@redhat.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License version
 *	2 as published by the Free Software Foundation.
 *
 * tracefs is the file system that is used by the tracing infrastructure.
 *
 */

#ifndef _TRACEFS_H_
#define _TRACEFS_H_

#include <linux/fs.h>
#include <linux/seq_file.h>

#include <linux/types.h>

struct file_operations;

#ifdef CONFIG_TRACING

/* instance types */
#define INSTANCE_DIR	0	/* created inside instances dir */
#define INSTANCE_MNT	1	/* created with newinstance mount option */

struct dentry *tracefs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);

struct dentry *tracefs_create_dir(const char *name, struct dentry *parent);

void tracefs_remove(struct dentry *dentry);
void tracefs_remove_recursive(struct dentry *dentry);

struct dentry *
tracefs_create_instance_dir(const char *name, struct dentry *parent,
			    int (*mkdir)(int instance_type, void *data),
			    int (*rmdir)(int instance_type, void *data));

bool tracefs_initialized(void);

#endif /* CONFIG_TRACING */

#endif
