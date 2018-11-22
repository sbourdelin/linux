// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Roberto Sassu <roberto.sassu@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * File: readxattr.c
 *      Read extended attributes from regular files in the initial ram disk
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/xattr.h>
#include <linux/file.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/fs.h>

#include "internal.h"

#define SETXATTR_FILENAME ".setxattr"
#define FILENAME_XATTR_SEP ".xattr-"


struct readdir_callback {
	struct dir_context ctx;
	struct path *path;
};

LIST_HEAD(dir_list);

struct dir_path {
	struct list_head next;
	struct path path;
};

static int __init read_set_xattr(struct dir_context *__ctx, const char *name,
				 int namelen, loff_t offset, u64 ino,
				 unsigned int d_type)
{
	struct readdir_callback *ctx = container_of(__ctx, typeof(*ctx), ctx);
	struct path *dir = ctx->path, source_path, target_path;
	char filename[NAME_MAX + 1], *xattrname, *separator;
	struct dir_path *subdir;
	struct file *file;
	void *datap;
	loff_t size;
	int result;

	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 0;

	result = vfs_path_lookup(dir->dentry, dir->mnt, name, 0, &source_path);
	if (result)
		return 0;

	size = i_size_read(source_path.dentry->d_inode);
	if (size > XATTR_SIZE_MAX)
		goto out;

	if (source_path.dentry->d_inode->i_sb != dir->dentry->d_inode->i_sb)
		goto out;

	if (!S_ISREG(source_path.dentry->d_inode->i_mode) &&
	    !S_ISDIR(source_path.dentry->d_inode->i_mode))
		goto out;

	if (S_ISREG(source_path.dentry->d_inode->i_mode)) {
		separator = strstr(name, FILENAME_XATTR_SEP);
		if (!separator)
			goto out;

		xattrname = separator + sizeof(FILENAME_XATTR_SEP) - 1;
		if (strlen(xattrname) > XATTR_NAME_MAX)
			goto out;
	} else {
		subdir = kmalloc(sizeof(*subdir), GFP_KERNEL);
		if (subdir) {
			subdir->path.dentry = source_path.dentry;
			subdir->path.mnt = source_path.mnt;

			list_add(&subdir->next, &dir_list);
		}

		return 0;
	}

	file = dentry_open(&source_path, O_RDONLY, current_cred());
	if (IS_ERR(file))
		goto out;

	result = kernel_read_file(file, &datap, &size, 0, READING_XATTR);
	if (result)
		goto out_fput;

	if (separator != name) {
		snprintf(filename, sizeof(filename), "%.*s",
			 (int)(namelen - strlen(separator)), name);

		result = vfs_path_lookup(dir->dentry, dir->mnt, filename, 0,
					&target_path);
		if (result)
			goto out_vfree;

		inode_lock(target_path.dentry->d_inode);
	} else {
		target_path.dentry = dir->dentry;
		target_path.mnt = dir->mnt;
	}

	__vfs_setxattr_noperm(target_path.dentry, xattrname, datap, size, 0);

	if (separator != name) {
		inode_unlock(target_path.dentry->d_inode);
		path_put(&target_path);
	}
out_vfree:
	vfree(datap);
out_fput:
	fput(file);
out:
	path_put(&source_path);
	return 0;
}

void __init set_xattrs_initrd(void)
{
	struct readdir_callback buf = {
		.ctx.actor = read_set_xattr,
	};

	struct dir_path dir, *cur_dir;
	struct path path;
	struct file *file;
	int result;

	result = kern_path(SETXATTR_FILENAME, 0, &path);
	if (result)
		return;

	path_put(&path);

	result = kern_path("/", 0, &dir.path);
	if (result)
		return;

	list_add(&dir.next, &dir_list);

	while (!list_empty(&dir_list)) {
		cur_dir = list_first_entry(&dir_list, typeof(*cur_dir), next);

		file = dentry_open(&cur_dir->path, O_RDONLY, current_cred());
		if (file) {
			buf.path = &cur_dir->path;
			iterate_dir(file, &buf.ctx);
			fput(file);
		}

		path_put(&cur_dir->path);
		list_del(&cur_dir->next);

		if (cur_dir != &dir)
			kfree(cur_dir);
	}
}
EXPORT_SYMBOL_GPL(set_xattrs_initrd);
