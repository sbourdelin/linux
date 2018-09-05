/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvmem framework machine code bindings
 *
 * Copyright (C) 2018 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 */

#ifndef _LINUX_NVMEM_MACHINE_H
#define _LINUX_NVMEM_MACHINE_H

#include <linux/nvmem-provider.h>
#include <linux/list.h>

struct nvmem_cell_info {
	const char		*name;
	unsigned int		offset;
	unsigned int		bytes;
	unsigned int		bit_offset;
	unsigned int		nbits;
};

struct nvmem_cell_table {
	const char		*nvmem_name;
	struct nvmem_cell_info	*cells;
	size_t			ncells;
	struct list_head	node;
};

#if IS_ENABLED(CONFIG_NVMEM)

void nvmem_add_cell_table(struct nvmem_cell_table *table);
void nvmem_del_cell_table(struct nvmem_cell_table *table);

#else /* CONFIG_NVMEM */

static inline void nvmem_add_cell_table(struct nvmem_cell_table *table) {}
static inline void nvmem_del_cell_table(struct nvmem_cell_table *table) {}

#endif /* CONFIG_NVMEM */

#endif  /* ifndef _LINUX_NVMEM_MACHINE_H */
