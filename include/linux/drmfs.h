/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Swati Dhingra <swati.dhingra@intel.com>
 *	Sourab Gupta <sourab.gupta@intel.com>
 *	Akash Goel <akash.goel@intel.com>
 */

#ifndef _DRMFS_H_
#define _DRMFS_H_

#include <linux/fs.h>
#include <linux/seq_file.h>

#include <linux/types.h>

struct file_operations;

#ifdef CONFIG_DRMFS

struct dentry *drmfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);

struct dentry *drmfs_create_dir(const char *name, struct dentry *parent);

void drmfs_remove(struct dentry *dentry);
void drmfs_remove_recursive(struct dentry *dentry);

bool drmfs_initialized(void);
int drmfs_init(void);
int drmfs_fini(void);

#endif /* CONFIG_DRMFS */

#endif
