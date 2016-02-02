/*
 * linux/include/asm-generic/mmzone.h
 *
 * Author: Ganapatrao Kulkarni <gkulkarni@cavium.com>
 * Copyright (C) 2016 Cavium Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _ASM_GENERIC_MMZONE_H
#define _ASM_GENERIC_MMZONE_H

#if defined(CONFIG_NUMA) || defined(CONFIG_NEED_MULTIPLE_NODES)

#ifndef NODE_DATA
extern struct pglist_data *node_data[];
#define NODE_DATA(nid)          (node_data[(nid)])
#endif

#endif
#endif /* _ASM_GENERIC_MMZONE_H */
