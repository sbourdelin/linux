/*
 * pmalloc.c: Protectable Memory Allocator
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/printk.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/rculist.h>
#include <linux/set_memory.h>
#include <asm/cacheflush.h>
#include <asm/page.h>

/**
 * pmalloc_data contains the data specific to a pmalloc pool,
 * in a format compatible with the design of gen_alloc.
 * Some of the fields are used for exposing the corresponding parameter
 * to userspace, through sysfs.
 */
struct pmalloc_data {
	struct gen_pool *pool;  /* Link back to the associated pool. */
	bool protected;     /* Status of the pool: RO or RW. */
	struct kobj_attribute attr_protected; /* Sysfs attribute. */
	struct kobj_attribute attr_avail;     /* Sysfs attribute. */
	struct kobj_attribute attr_size;      /* Sysfs attribute. */
	struct kobj_attribute attr_chunks;    /* Sysfs attribute. */
	struct kobject *pool_kobject;
	struct list_head node; /* list of pools */
};

static LIST_HEAD(pmalloc_final_list);
static LIST_HEAD(pmalloc_tmp_list);
static struct list_head *pmalloc_list = &pmalloc_tmp_list;
static DEFINE_MUTEX(pmalloc_mutex);
static struct kobject *pmalloc_kobject;

static ssize_t pmalloc_pool_show_protected(struct kobject *dev,
					   struct kobj_attribute *attr,
					   char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_protected);
	if (data->protected)
		return sprintf(buf, "protected\n");
	else
		return sprintf(buf, "unprotected\n");
}

static ssize_t pmalloc_pool_show_avail(struct kobject *dev,
				       struct kobj_attribute *attr,
				       char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_avail);
	return sprintf(buf, "%lu\n", gen_pool_avail(data->pool));
}

static ssize_t pmalloc_pool_show_size(struct kobject *dev,
				      struct kobj_attribute *attr,
				      char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_size);
	return sprintf(buf, "%lu\n", gen_pool_size(data->pool));
}

static void pool_chunk_number(struct gen_pool *pool,
			      struct gen_pool_chunk *chunk, void *data)
{
	unsigned long *counter = data;

	(*counter)++;
}

static ssize_t pmalloc_pool_show_chunks(struct kobject *dev,
					struct kobj_attribute *attr,
					char *buf)
{
	struct pmalloc_data *data;
	unsigned long chunks_num = 0;

	data = container_of(attr, struct pmalloc_data, attr_chunks);
	gen_pool_for_each_chunk(data->pool, pool_chunk_number, &chunks_num);
	return sprintf(buf, "%lu\n", chunks_num);
}

/**
 * Exposes the pool and its attributes through sysfs.
 */
static struct kobject *pmalloc_connect(struct pmalloc_data *data)
{
	const struct attribute *attrs[] = {
		&data->attr_protected.attr,
		&data->attr_avail.attr,
		&data->attr_size.attr,
		&data->attr_chunks.attr,
		NULL
	};
	struct kobject *kobj;

	kobj = kobject_create_and_add(data->pool->name, pmalloc_kobject);
	if (unlikely(!kobj))
		return NULL;

	if (unlikely(sysfs_create_files(kobj, attrs) < 0)) {
		kobject_put(kobj);
		kobj = NULL;
	}
	return kobj;
}

/**
 * Removes the pool and its attributes from sysfs.
 */
static void pmalloc_disconnect(struct pmalloc_data *data,
			       struct kobject *kobj)
{
	const struct attribute *attrs[] = {
		&data->attr_protected.attr,
		&data->attr_avail.attr,
		&data->attr_size.attr,
		&data->attr_chunks.attr,
		NULL
	};

	sysfs_remove_files(kobj, attrs);
	kobject_put(kobj);
}

/**
 * Declares an attribute of the pool.
 */

#define pmalloc_attr_init(data, attr_name) \
do { \
	sysfs_attr_init(&data->attr_##attr_name.attr); \
	data->attr_##attr_name.attr.name = #attr_name; \
	data->attr_##attr_name.attr.mode = VERIFY_OCTAL_PERMISSIONS(0444); \
	data->attr_##attr_name.show = pmalloc_pool_show_##attr_name; \
} while (0)

struct gen_pool *pmalloc_create_pool(const char *name, int min_alloc_order)
{
	struct gen_pool *pool;
	const char *pool_name;
	struct pmalloc_data *data;

	if (!name) {
		WARN_ON(1);
		return NULL;
	}

	if (min_alloc_order < 0)
		min_alloc_order = ilog2(sizeof(unsigned long));

	pool = gen_pool_create(min_alloc_order, NUMA_NO_NODE);
	if (unlikely(!pool))
		return NULL;

	mutex_lock(&pmalloc_mutex);
	list_for_each_entry(data, pmalloc_list, node)
		if (!strcmp(name, data->pool->name))
			goto same_name_err;

	pool_name = kstrdup(name, GFP_KERNEL);
	if (unlikely(!pool_name))
		goto name_alloc_err;

	data = kzalloc(sizeof(struct pmalloc_data), GFP_KERNEL);
	if (unlikely(!data))
		goto data_alloc_err;

	data->protected = false;
	data->pool = pool;
	pmalloc_attr_init(data, protected);
	pmalloc_attr_init(data, avail);
	pmalloc_attr_init(data, size);
	pmalloc_attr_init(data, chunks);
	pool->data = data;
	pool->name = pool_name;

	list_add(&data->node, pmalloc_list);
	if (pmalloc_list == &pmalloc_final_list)
		data->pool_kobject = pmalloc_connect(data);
	mutex_unlock(&pmalloc_mutex);
	return pool;

data_alloc_err:
	kfree(pool_name);
name_alloc_err:
same_name_err:
	mutex_unlock(&pmalloc_mutex);
	gen_pool_destroy(pool);
	return NULL;
}

static inline int check_alloc_params(struct gen_pool *pool, size_t req_size)
{
	struct pmalloc_data *data;
	unsigned int order;

	if (unlikely(!req_size || !pool))
		return -1;

	order = (unsigned int)pool->min_alloc_order;
	data = pool->data;

	if (data == NULL)
		return -1;

	if (unlikely(data->protected)) {
		WARN_ON(1);
		return -1;
	}
	return 0;
}


static inline bool chunk_tagging(void *chunk, bool tag)
{
	struct vm_struct *area;
	struct page *page;

	if (!is_vmalloc_addr(chunk))
		return false;

	page = vmalloc_to_page(chunk);
	if (unlikely(!page))
		return false;

	area = page->area;
	if (tag)
		area->flags |= VM_PMALLOC;
	else
		area->flags &= ~VM_PMALLOC;
	return true;
}


static inline bool tag_chunk(void *chunk)
{
	return chunk_tagging(chunk, true);
}


static inline bool untag_chunk(void *chunk)
{
	return chunk_tagging(chunk, false);
}

enum {
	INVALID_PMALLOC_OBJECT = -1,
	NOT_PMALLOC_OBJECT = 0,
	VALID_PMALLOC_OBJECT = 1,
};

int is_pmalloc_object(const void *ptr, const unsigned long n)
{
	struct vm_struct *area;
	struct page *page;
	unsigned long area_start;
	unsigned long area_end;
	unsigned long object_start;
	unsigned long object_end;


	/* is_pmalloc_object gets called pretty late, so chances are high
	 * that the object is indeed of vmalloc type
	 */
	if (unlikely(!is_vmalloc_addr(ptr)))
		return NOT_PMALLOC_OBJECT;

	page = vmalloc_to_page(ptr);
	if (unlikely(!page))
		return NOT_PMALLOC_OBJECT;

	area = page->area;

	if (likely(!(area->flags & VM_PMALLOC)))
		return NOT_PMALLOC_OBJECT;

	area_start = (unsigned long)area->addr;
	area_end = area_start + area->nr_pages * PAGE_SIZE - 1;
	object_start = (unsigned long)ptr;
	object_end = object_start + n - 1;

	if (likely((area_start <= object_start) &&
		   (object_end <= area_end)))
		return VALID_PMALLOC_OBJECT;
	else
		return INVALID_PMALLOC_OBJECT;
}


bool pmalloc_prealloc(struct gen_pool *pool, size_t size)
{
	void *chunk;
	size_t chunk_size;
	bool add_error;
	unsigned int order;

	if (check_alloc_params(pool, size))
		return false;

	order = (unsigned int)pool->min_alloc_order;

	/* Expand pool */
	chunk_size = roundup(size, PAGE_SIZE);
	chunk = vmalloc(chunk_size);
	if (unlikely(chunk == NULL))
		return false;

	/* Locking is already done inside gen_pool_add */
	add_error = gen_pool_add(pool, (unsigned long)chunk, chunk_size,
				 NUMA_NO_NODE);
	if (unlikely(add_error != 0))
		goto abort;

	return true;
abort:
	vfree(chunk);
	return false;

}

void *pmalloc(struct gen_pool *pool, size_t size, gfp_t gfp)
{
	void *chunk;
	size_t chunk_size;
	bool add_error;
	unsigned long retval;
	unsigned int order;

	if (check_alloc_params(pool, size))
		return NULL;

	order = (unsigned int)pool->min_alloc_order;

retry_alloc_from_pool:
	retval = gen_pool_alloc(pool, size);
	if (retval)
		goto return_allocation;

	if (unlikely((gfp & __GFP_ATOMIC))) {
		if (unlikely((gfp & __GFP_NOFAIL)))
			goto retry_alloc_from_pool;
		else
			return NULL;
	}

	/* Expand pool */
	chunk_size = roundup(size, PAGE_SIZE);
	chunk = vmalloc(chunk_size);
	if (unlikely(!chunk)) {
		if (unlikely((gfp & __GFP_NOFAIL)))
			goto retry_alloc_from_pool;
		else
			return NULL;
	}
	if (unlikely(!tag_chunk(chunk)))
		goto free;

	/* Locking is already done inside gen_pool_add */
	add_error = gen_pool_add(pool, (unsigned long)chunk, chunk_size,
				 NUMA_NO_NODE);
	if (unlikely(add_error))
		goto abort;

	retval = gen_pool_alloc(pool, size);
	if (retval) {
return_allocation:
		*(size_t *)retval = size;
		if (gfp & __GFP_ZERO)
			memset((void *)retval, 0, size);
		return (void *)retval;
	}
	/* Here there is no test for __GFP_NO_FAIL because, in case of
	 * concurrent allocation, one thread might add a chunk to the
	 * pool and this memory could be allocated by another thread,
	 * before the first thread gets a chance to use it.
	 * As long as vmalloc succeeds, it's ok to retry.
	 */
	goto retry_alloc_from_pool;
abort:
	untag_chunk(chunk);
free:
	vfree(chunk);
	return NULL;
}

static void pmalloc_chunk_set_protection(struct gen_pool *pool,

					 struct gen_pool_chunk *chunk,
					 void *data)
{
	const bool *flag = data;
	size_t chunk_size = chunk->end_addr + 1 - chunk->start_addr;
	unsigned long pages = chunk_size / PAGE_SIZE;

	BUG_ON(chunk_size & (PAGE_SIZE - 1));

	if (*flag)
		set_memory_ro(chunk->start_addr, pages);
	else
		set_memory_rw(chunk->start_addr, pages);
}

static int pmalloc_pool_set_protection(struct gen_pool *pool, bool protection)
{
	struct pmalloc_data *data;
	struct gen_pool_chunk *chunk;

	if (unlikely(!pool))
		return -EINVAL;

	data = pool->data;

	if (unlikely(!data))
		return -EINVAL;

	if (unlikely(data->protected == protection)) {
		WARN_ON(1);
		return 0;
	}

	data->protected = protection;
	list_for_each_entry(chunk, &(pool)->chunks, next_chunk)
		pmalloc_chunk_set_protection(pool, chunk, &protection);
	return 0;
}

int pmalloc_protect_pool(struct gen_pool *pool)
{
	return pmalloc_pool_set_protection(pool, true);
}


static void pmalloc_chunk_free(struct gen_pool *pool,
			       struct gen_pool_chunk *chunk, void *data)
{
	untag_chunk(chunk);
	gen_pool_flush_chunk(pool, chunk);
	vfree_atomic((void *)chunk->start_addr);
}


int pmalloc_destroy_pool(struct gen_pool *pool)
{
	struct pmalloc_data *data;

	if (unlikely(pool == NULL))
		return -EINVAL;

	data = pool->data;

	if (unlikely(data == NULL))
		return -EINVAL;

	mutex_lock(&pmalloc_mutex);
	list_del(&data->node);
	mutex_unlock(&pmalloc_mutex);

	if (likely(data->pool_kobject))
		pmalloc_disconnect(data, data->pool_kobject);

	pmalloc_pool_set_protection(pool, false);
	gen_pool_for_each_chunk(pool, pmalloc_chunk_free, NULL);
	gen_pool_destroy(pool);
	kfree(data);
	return 0;
}

/**
 * When the sysfs is ready to receive registrations, connect all the
 * pools previously created. Also enable further pools to be connected
 * right away.
 */
static int __init pmalloc_late_init(void)
{
	struct pmalloc_data *data, *n;

	pmalloc_kobject = kobject_create_and_add("pmalloc", kernel_kobj);

	mutex_lock(&pmalloc_mutex);
	pmalloc_list = &pmalloc_final_list;

	if (likely(pmalloc_kobject != NULL)) {
		list_for_each_entry_safe(data, n, &pmalloc_tmp_list, node) {
			list_move(&data->node, &pmalloc_final_list);
			pmalloc_connect(data);
		}
	}
	mutex_unlock(&pmalloc_mutex);
	return 0;
}
late_initcall(pmalloc_late_init);
