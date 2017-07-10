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
#include <asm/set_memory.h>
#include <asm/page.h>

#include <linux/debugfs.h>
#include <linux/kallsyms.h>

static LIST_HEAD(tmp_list);

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
	struct mutex mutex;
};

static LIST_HEAD(pmalloc_final_list);
static LIST_HEAD(pmalloc_tmp_list);
static struct list_head *pmalloc_list = &pmalloc_tmp_list;
static DEFINE_MUTEX(pmalloc_mutex);
static struct kobject *pmalloc_kobject;

static const unsigned long pmalloc_signature = (unsigned long)&pmalloc_mutex;

static ssize_t __pmalloc_pool_show_protected(struct kobject *dev,
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

static ssize_t __pmalloc_pool_show_avail(struct kobject *dev,
					 struct kobj_attribute *attr,
					 char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_avail);
	return sprintf(buf, "%lu\n", gen_pool_avail(data->pool));
}

static ssize_t __pmalloc_pool_show_size(struct kobject *dev,
					struct kobj_attribute *attr,
					char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_size);
	return sprintf(buf, "%lu\n", gen_pool_size(data->pool));
}

static void __pool_chunk_number(struct gen_pool *pool,
				struct gen_pool_chunk *chunk, void *data)
{
	if (!data)
		return;
	*(unsigned long *)data += 1;
}

static ssize_t __pmalloc_pool_show_chunks(struct kobject *dev,
					  struct kobj_attribute *attr,
					  char *buf)
{
	struct pmalloc_data *data;
	unsigned long chunks_num = 0;

	data = container_of(attr, struct pmalloc_data, attr_chunks);
	gen_pool_for_each_chunk(data->pool, __pool_chunk_number, &chunks_num);
	return sprintf(buf, "%lu\n", chunks_num);
}

/**
 * Exposes the pool and its attributes through sysfs.
 */
static void __pmalloc_connect(struct pmalloc_data *data)
{
	data->pool_kobject = kobject_create_and_add(data->pool->name,
						    pmalloc_kobject);
	sysfs_create_file(data->pool_kobject, &data->attr_protected.attr);
	sysfs_create_file(data->pool_kobject, &data->attr_avail.attr);
	sysfs_create_file(data->pool_kobject, &data->attr_size.attr);
	sysfs_create_file(data->pool_kobject, &data->attr_chunks.attr);
}

/**
 * Removes the pool and its attributes from sysfs.
 */
static void __pmalloc_disconnect(struct pmalloc_data *data)
{
	sysfs_remove_file(data->pool_kobject, &data->attr_protected.attr);
	sysfs_remove_file(data->pool_kobject, &data->attr_avail.attr);
	sysfs_remove_file(data->pool_kobject, &data->attr_size.attr);
	sysfs_remove_file(data->pool_kobject, &data->attr_chunks.attr);
	kobject_put(data->pool_kobject);
}

/**
 * Declares an attribute of the pool.
 */


#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define do_lock_dep(data, attr_name) \
	(data->attr_##attr_name.attr.ignore_lockdep = 1)
#else
#define do_lock_dep(data, attr_name) do {} while (0)
#endif

#define __pmalloc_attr_init(data, attr_name) \
do { \
	data->attr_##attr_name.attr.name = #attr_name; \
	data->attr_##attr_name.attr.mode = VERIFY_OCTAL_PERMISSIONS(0444); \
	data->attr_##attr_name.show = __pmalloc_pool_show_##attr_name; \
	do_lock_dep(data, attr_name); \
} while (0)

struct gen_pool *pmalloc_create_pool(const char *name, int min_alloc_order)
{
	struct gen_pool *pool;
	const char *pool_name;
	struct pmalloc_data *data;

	if (!name)
		return NULL;
	pool_name = kstrdup(name, GFP_KERNEL);
	if (!pool_name)
		return NULL;
	data = kzalloc(sizeof(struct pmalloc_data), GFP_KERNEL);
	if (!data)
		return NULL;
	if (min_alloc_order < 0)
		min_alloc_order = ilog2(sizeof(unsigned long));
	pool = gen_pool_create(min_alloc_order, NUMA_NO_NODE);
	if (!pool) {
		kfree(pool_name);
		kfree(data);
		return NULL;
	}
	data->protected = false;
	data->pool = pool;
	mutex_init(&data->mutex);
	__pmalloc_attr_init(data, protected);
	__pmalloc_attr_init(data, avail);
	__pmalloc_attr_init(data, size);
	__pmalloc_attr_init(data, chunks);
	pool->data = data;
	pool->name = pool_name;
	mutex_lock(&pmalloc_mutex);
	list_add(&data->node, &pmalloc_tmp_list);
	if (pmalloc_list == &pmalloc_final_list)
		__pmalloc_connect(data);
	mutex_unlock(&pmalloc_mutex);
	return pool;
}


bool is_pmalloc_page(struct page *page)
{
	return page && page_private(page) &&
		page->private == pmalloc_signature;
}
EXPORT_SYMBOL(is_pmalloc_page);

/**
 * To support hardened usercopy, tag/untag pages supplied by pmalloc.
 * Pages are tagged when added to a pool and untagged when removed
 * from said pool.
 */
#define PMALLOC_TAG_PAGE true
#define PMALLOC_UNTAG_PAGE false
static inline
int __pmalloc_tag_pages(void *base, const size_t size, const bool set_tag)
{
	void *end = base + size - 1;

	do {
		struct page *page;

		if (!is_vmalloc_addr(base))
			return -EINVAL;
		page = vmalloc_to_page(base);
		if (set_tag) {
			BUG_ON(page_private(page) || page->private);
			set_page_private(page, 1);
			page->private = pmalloc_signature;
		} else {
			BUG_ON(!(page_private(page) &&
				 page->private == pmalloc_signature));
			set_page_private(page, 0);
			page->private = 0;
		}
		base += PAGE_SIZE;
	} while ((PAGE_MASK & (unsigned long)base) <=
		 (PAGE_MASK & (unsigned long)end));
	return 0;
}


static void __page_untag(struct gen_pool *pool,
			 struct gen_pool_chunk *chunk, void *data)
{
	__pmalloc_tag_pages((void *)chunk->start_addr,
			    chunk->end_addr - chunk->start_addr + 1,
			    PMALLOC_UNTAG_PAGE);
}

void *pmalloc(struct gen_pool *pool, size_t size)
{
	void *retval, *chunk;
	size_t chunk_size;

	if (!size || !pool || ((struct pmalloc_data *)pool->data)->protected)
		return NULL;
	retval = (void *)gen_pool_alloc(pool, size);
	if (retval)
		return retval;
	chunk_size = roundup(size, PAGE_SIZE);
	chunk = vmalloc(chunk_size);
	if (!chunk)
		return NULL;
	__pmalloc_tag_pages(chunk, size, PMALLOC_TAG_PAGE);
	/* Locking is already done inside gen_pool_add_virt */
	BUG_ON(gen_pool_add_virt(pool, (unsigned long)chunk,
				(phys_addr_t)NULL, chunk_size, NUMA_NO_NODE));
	return (void *)gen_pool_alloc(pool, size);
}

static void __page_protection(struct gen_pool *pool,
			      struct gen_pool_chunk *chunk, void *data)
{
	unsigned long pages;

	if (!data)
		return;
	pages = roundup(chunk->end_addr - chunk->start_addr + 1,
			PAGE_SIZE) / PAGE_SIZE;
	if (*(bool *)data)
		set_memory_ro(chunk->start_addr, pages);
	else
		set_memory_rw(chunk->start_addr, pages);
}

static int __pmalloc_pool_protection(struct gen_pool *pool, bool protection)
{
	struct pmalloc_data *data;
	struct gen_pool_chunk *chunk;

	if (!pool)
		return -EINVAL;
	data = (struct pmalloc_data *)pool->data;
	mutex_lock(&data->mutex);
	BUG_ON(data->protected == protection);
	data->protected = protection;
	list_for_each_entry(chunk, &(pool)->chunks, next_chunk)
		__page_protection(pool, chunk, &protection);
	mutex_unlock(&data->mutex);
	return 0;
}

int pmalloc_protect_pool(struct gen_pool *pool)
{
	return __pmalloc_pool_protection(pool, true);
}


bool pmalloc_pool_protected(struct gen_pool *pool)
{
	if (!pool)
		return true;
	return ((struct pmalloc_data *)pool->data)->protected;
}


int pmalloc_destroy_pool(struct gen_pool *pool)
{
	struct pmalloc_data *data;

	if (!pool)
		return -EINVAL;
	data = (struct pmalloc_data *)pool->data;
	mutex_lock(&data->mutex);
	list_del(&data->node);
	mutex_unlock(&data->mutex);
	gen_pool_for_each_chunk(pool, __page_untag, NULL);
	__pmalloc_disconnect(data);
	__pmalloc_pool_protection(pool, false);
	gen_pool_destroy(pool);
	kfree(data);
	return 0;
}

static const char msg[] = "Not a valid Pmalloc object.";
const char *pmalloc_check_range(const void *ptr, unsigned long n)
{
	unsigned long p;

	p = (unsigned long)ptr;
	n = p + n - 1;
	for (; (PAGE_MASK & p) <= (PAGE_MASK & n); p += PAGE_SIZE) {
		struct page *page;

		if (!is_vmalloc_addr((void *)p))
			return msg;
		page = vmalloc_to_page((void *)p);
		if (!(page && page_private(page) &&
		      page->private == pmalloc_signature))
			return msg;
	}
	return NULL;
}
EXPORT_SYMBOL(pmalloc_check_range);


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
	list_for_each_entry_safe(data, n, &pmalloc_tmp_list, node) {
		list_move(&data->node, &pmalloc_final_list);
		__pmalloc_connect(data);
	}
	mutex_unlock(&pmalloc_mutex);
	return 0;
}
late_initcall(pmalloc_late_init);
