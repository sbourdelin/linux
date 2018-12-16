/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ION_H
#define _LINUX_ION_H

#define MAX_NUM_OF_CHUNK_HEAPS 32
#define MAX_CHUNK_HEAP_NAME_SIZE 32

struct ion_chunk_heap_cfg {
	char heap_name[MAX_CHUNK_HEAP_NAME_SIZE];
	phys_addr_t base;
	size_t size;
	size_t chunk_size;
};

int ion_add_chunk_heaps(struct ion_chunk_heap_cfg *cfg,
			unsigned int  num_of_heaps);

#endif /* _LINUX_ION_H */
