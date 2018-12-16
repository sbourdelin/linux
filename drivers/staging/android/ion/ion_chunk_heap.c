// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/staging/android/ion/ion_chunk_heap.c
 *
 * Copyright (C) 2012 Google, Inc.
 */
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ion.h>
#include "ion.h"

static struct ion_chunk_heap_cfg chunk_heap_cfg[MAX_NUM_OF_CHUNK_HEAPS];
static unsigned int num_of_req_chunk_heaps;

struct ion_chunk_heap {
	struct ion_heap heap;
	struct gen_pool *pool;
	phys_addr_t base;
	unsigned long chunk_size;
	unsigned long size;
	unsigned long allocated;
};

static int ion_chunk_heap_allocate(struct ion_heap *heap,
				   struct ion_buffer *buffer,
				   unsigned long size,
				   unsigned long flags)
{
	struct ion_chunk_heap *chunk_heap =
		container_of(heap, struct ion_chunk_heap, heap);
	struct sg_table *table;
	struct scatterlist *sg;
	int ret, i;
	unsigned long num_chunks;
	unsigned long allocated_size;

	allocated_size = ALIGN(size, chunk_heap->chunk_size);
	num_chunks = allocated_size / chunk_heap->chunk_size;

	if (allocated_size > chunk_heap->size - chunk_heap->allocated)
		return -ENOMEM;

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	ret = sg_alloc_table(table, num_chunks, GFP_KERNEL);
	if (ret) {
		kfree(table);
		return ret;
	}

	sg = table->sgl;
	for (i = 0; i < num_chunks; i++) {
		unsigned long paddr = gen_pool_alloc(chunk_heap->pool,
						     chunk_heap->chunk_size);
		if (!paddr)
			goto err;
		sg_set_page(sg, pfn_to_page(PFN_DOWN(paddr)),
			    chunk_heap->chunk_size, 0);
		sg = sg_next(sg);
	}

	buffer->sg_table = table;
	chunk_heap->allocated += allocated_size;
	return 0;
err:
	sg = table->sgl;
	for (i -= 1; i >= 0; i--) {
		gen_pool_free(chunk_heap->pool, page_to_phys(sg_page(sg)),
			      sg->length);
		sg = sg_next(sg);
	}
	sg_free_table(table);
	kfree(table);
	return -ENOMEM;
}

static void ion_chunk_heap_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_chunk_heap *chunk_heap =
		container_of(heap, struct ion_chunk_heap, heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	int i;
	unsigned long allocated_size;

	allocated_size = ALIGN(buffer->size, chunk_heap->chunk_size);

	ion_heap_buffer_zero(buffer);

	for_each_sg(table->sgl, sg, table->nents, i) {
		gen_pool_free(chunk_heap->pool, page_to_phys(sg_page(sg)),
			      sg->length);
	}
	chunk_heap->allocated -= allocated_size;
	sg_free_table(table);
	kfree(table);
}

static struct ion_heap_ops chunk_heap_ops = {
	.allocate = ion_chunk_heap_allocate,
	.free = ion_chunk_heap_free,
	.map_user = ion_heap_map_user,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
};

static struct ion_heap *ion_chunk_heap_create(struct ion_chunk_heap_cfg *heap_cfg)
{
	struct ion_chunk_heap *chunk_heap;
	int ret;
	struct page *page;
	size_t size;

	page = pfn_to_page(PFN_DOWN(heap_cfg->base));
	size = heap_cfg->size;

	ret = ion_heap_pages_zero(page, size, pgprot_writecombine(PAGE_KERNEL));
	if (ret)
		return ERR_PTR(ret);

	chunk_heap = kzalloc(sizeof(*chunk_heap), GFP_KERNEL);
	if (!chunk_heap)
		return ERR_PTR(-ENOMEM);

	chunk_heap->chunk_size = heap_cfg->chunk_size;
	chunk_heap->pool = gen_pool_create(get_order(chunk_heap->chunk_size) +
					   PAGE_SHIFT, -1);
	if (!chunk_heap->pool) {
		ret = -ENOMEM;
		goto error_gen_pool_create;
	}
	chunk_heap->base = heap_cfg->base;
	chunk_heap->size = heap_cfg->size;
	chunk_heap->heap.name = heap_cfg->heap_name;
	chunk_heap->allocated = 0;

	gen_pool_add(chunk_heap->pool, chunk_heap->base, heap_cfg->size, -1);
	chunk_heap->heap.ops = &chunk_heap_ops;
	chunk_heap->heap.type = ION_HEAP_TYPE_CHUNK;
	chunk_heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;

	pr_info("%s: name %s base %pa size %zu\n", __func__,
		heap_cfg->heap_name,
		&heap_cfg->base,
		heap_cfg->size);

	return &chunk_heap->heap;

error_gen_pool_create:
	kfree(chunk_heap);
	return ERR_PTR(ret);
}

static int __init setup_heap(char *param)
{
	char *at_sign, *coma, *colon;
	size_t size_to_copy;
	struct ion_chunk_heap_cfg *cfg;

	do {
		cfg = &chunk_heap_cfg[num_of_req_chunk_heaps];

		/* heap name */
		colon = strchr(param, ':');
		if (!colon)
			return -EINVAL;

		size_to_copy = min_t(size_t, MAX_CHUNK_HEAP_NAME_SIZE - 1,
				     (colon - param));
		strncpy(cfg->heap_name,	param, size_to_copy);
		cfg->heap_name[size_to_copy] = '\0';

		/* heap size */
		cfg->size = memparse((colon + 1), &at_sign);
		if ((colon + 1) == at_sign)
			return -EINVAL;

		/* heap base addr */
		if (*at_sign == '@')
			cfg->base = memparse(at_sign + 1, &coma);
		else
			return -EINVAL;

		if (at_sign == coma)
			return -EINVAL;

		/* Chunk size */
		cfg->chunk_size = PAGE_SIZE;

		num_of_req_chunk_heaps++;

		/* if one more heap configuration exists */
		if (*coma == ',')
			param = coma + 1;
		else
			param = NULL;
	} while (num_of_req_chunk_heaps < MAX_NUM_OF_CHUNK_HEAPS && param);

	return 0;
}

__setup("ion_chunk_heap=", setup_heap);

int ion_add_chunk_heaps(struct ion_chunk_heap_cfg *cfg,
			unsigned int num_of_heaps)
{
	unsigned int i;
	struct ion_heap *heap;

	for (i = 0; i < num_of_heaps; i++) {
		heap = ion_chunk_heap_create(&cfg[i]);
		if (heap)
			ion_device_add_heap(heap);
	}
	return 0;
}

static int ion_add_chunk_heaps_from_boot_param(void)
{
	return ion_add_chunk_heaps(chunk_heap_cfg, num_of_req_chunk_heaps);
}

device_initcall(ion_add_chunk_heaps_from_boot_param);
