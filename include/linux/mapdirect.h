/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
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
#ifndef __MAPDIRECT_H__
#define __MAPDIRECT_H__
#include <linux/err.h>

struct inode;
struct work_struct;
struct vm_area_struct;
struct map_direct_state;

#if IS_ENABLED(CONFIG_FS_DAX)
struct map_direct_state *map_direct_register(int fd, struct vm_area_struct *vma);
bool test_map_direct_valid(struct map_direct_state *mds);
void generic_map_direct_open(struct vm_area_struct *vma);
void generic_map_direct_close(struct vm_area_struct *vma);
#else
static inline struct map_direct_state *map_direct_register(int fd,
		struct vm_area_struct *vma)
{
	return ERR_PTR(-EOPNOTSUPP);
}
static inline bool test_map_direct_valid(struct map_direct_state *mds)
{
	return false;
}
#define generic_map_direct_open NULL
#define generic_map_direct_close NULL
#endif
#endif /* __MAPDIRECT_H__ */
