/*
 * Copyright(c) 2016 - 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#ifndef __DAX_H__
#define __DAX_H__
struct dax_inode;
struct dax_inode *alloc_dax_inode(void *private, const char *host);
void put_dax_inode(struct dax_inode *dax_inode);
bool dax_inode_alive(struct dax_inode *dax_inode);
void kill_dax_inode(struct dax_inode *dax_inode);
struct dax_inode *inode_to_dax_inode(struct inode *inode);
struct inode *dax_inode_to_inode(struct dax_inode *dax_inode);
void *dax_inode_get_private(struct dax_inode *dax_inode);
int dax_inode_register(struct dax_inode *dax_inode,
		const struct file_operations *fops, struct module *owner,
		struct kobject *parent);
void dax_inode_unregister(struct dax_inode *dax_inode);
#endif /* __DAX_H__ */
