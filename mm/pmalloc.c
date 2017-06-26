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


/**
 * pmalloc_data contains the data specific to a pmalloc pool,
 * in a format compatible with the design of gen_alloc.
 * Some of the fields are used for exposing the corresponding parameter
 * to userspace, through sysfs.
 */
struct pmalloc_data {
	struct gen_pool *pool;  /* Link back to the associated pool. */
	atomic_t protected;     /* Status of the pool: RO or RW. */
	atomic_t processed;     /* Is the pool already in sysfs? */
	struct device dev;      /* Device used to connect to sysfs. */
	struct device_attribute attr_protected; /* Sysfs attribute. */
	struct device_attribute attr_avail;     /* Sysfs attribute. */
	struct device_attribute attr_size;      /* Sysfs attribute. */
};

/**
 * Keeps track of the safe point, where operatioms according to the normal
 * device model are supported. Before this point, such operation are not
 * available.
 */
static atomic_t into_post_init;

static struct device pmalloc_dev;
static struct lock_class_key pmalloc_lock_key;
static struct class pmalloc_class = {
	.name = "pmalloc",
	.owner = THIS_MODULE,
};

static ssize_t __pmalloc_pool_show_protected(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_protected);
	if (atomic_read(&data->protected))
		return sprintf(buf, "protected\n");
	else
		return sprintf(buf, "unprotected\n");
}

static ssize_t __pmalloc_pool_show_avail(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_avail);
	return sprintf(buf, "%lu\n", gen_pool_avail(data->pool));
}

static ssize_t __pmalloc_pool_show_size(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct pmalloc_data *data;

	data = container_of(attr, struct pmalloc_data, attr_size);
	return sprintf(buf, "%lu\n", gen_pool_size(data->pool));
}

/**
 * Exposes the pool and its attributes through sysfs.
 */
static void __pmalloc_connect(struct pmalloc_data *data)
{
	device_add(&data->dev);
	device_create_file(&data->dev, &data->attr_protected);
	device_create_file(&data->dev, &data->attr_avail);
	device_create_file(&data->dev, &data->attr_size);
}

/**
 * Removes the pool and its attributes from sysfs.
 */
static void __pmalloc_disconnect(struct pmalloc_data *data)
{
	device_remove_file(&data->dev, &data->attr_protected);
	device_remove_file(&data->dev, &data->attr_avail);
	device_remove_file(&data->dev, &data->attr_size);
	device_del(&data->dev);
}

/**
 * Declares an attribute of the pool.
 */
#define __pmalloc_attr_init(data, attr_name) \
{ \
	data->attr_##attr_name.attr.name = #attr_name; \
	data->attr_##attr_name.attr.mode = VERIFY_OCTAL_PERMISSIONS(0444); \
	data->attr_##attr_name.show = __pmalloc_pool_show_##attr_name; \
}

struct gen_pool *pmalloc_create_pool(const char *name,
					 int min_alloc_order)
{
	struct gen_pool *pool;
	struct pmalloc_data *data;

	data = kzalloc(sizeof(struct pmalloc_data), GFP_KERNEL);
	if (!data)
		return NULL;
	if (min_alloc_order < 0)
		min_alloc_order = ilog2(sizeof(unsigned long));
	pool = devm_gen_pool_create(&pmalloc_dev, min_alloc_order,
				    -1, name);
	if (!pool) {
		kfree(data);
		return NULL;
	}
	atomic_set(&data->protected, false);
	device_initialize(&data->dev);
	dev_set_name(&data->dev, "%s", name);
	data->dev.class = &pmalloc_class;
	atomic_set(&data->processed, atomic_read(&into_post_init));
	data->pool = pool;
	__pmalloc_attr_init(data, protected);
	__pmalloc_attr_init(data, avail);
	__pmalloc_attr_init(data, size);
	if (atomic_read(&data->processed)) /* Check sysfs availability. */
		__pmalloc_connect(data);   /* After late init. */
	pool->data = data;
	return pool;
}


struct gen_pool *pmalloc_get_pool(const char *name)
{
	return gen_pool_get(&pmalloc_dev, name);
}


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
		if (set_tag)
			__SetPagePmalloc(page);
		else
			__ClearPagePmalloc(page);
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

	if (!size || !pool ||
	    atomic_read(&((struct pmalloc_data *)pool->data)->protected))
		return NULL;
	retval = (void *)gen_pool_alloc(pool, size);
	if (retval)
		return retval;
	chunk_size = roundup(size, PAGE_SIZE);
	chunk = vmalloc(chunk_size);
	if (!chunk)
		return NULL;
	__pmalloc_tag_pages(chunk, size, PMALLOC_TAG_PAGE);
	BUG_ON(gen_pool_add_virt(pool, (unsigned long)chunk,
				(phys_addr_t)NULL, chunk_size, -1));
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
	if (!pool)
		return -EINVAL;
	BUG_ON(atomic_read(&((struct pmalloc_data *)pool->data)->protected)
	       == protection);
	atomic_set(&((struct pmalloc_data *)pool->data)->protected, protection);
	gen_pool_for_each_chunk(pool, __page_protection, &protection);
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
	return atomic_read(&(((struct pmalloc_data *)pool->data)->protected));
}


void devm_gen_pool_release(struct device *dev, void *res);
int devm_gen_pool_match(struct device *dev, void *res, void *data);

int pmalloc_destroy_pool(struct gen_pool *pool)
{
	struct gen_pool **p;
	struct pmalloc_data *data;

	data = (struct pmalloc_data *)pool->data;
	p = devres_find(&pmalloc_dev, devm_gen_pool_release,
			devm_gen_pool_match, (void *)pool->name);
	if (!p)
		return -EINVAL;
	__pmalloc_pool_protection(pool, false);
	gen_pool_for_each_chunk(pool, __page_untag, NULL);
	devm_gen_pool_release(&pmalloc_dev, p);
	__pmalloc_disconnect(data);
	kfree(data);
	return 0;
}

static const char msg[] = "Not a valid Pmalloc object.";
const char *__pmalloc_check_object(const void *ptr, unsigned long n)
{
	unsigned long p;

	p = (unsigned long)ptr;
	n = p + n - 1;
	for (; (PAGE_MASK & p) <= (PAGE_MASK & n); p += PAGE_SIZE) {
		struct page *page;

		if (!is_vmalloc_addr((void *)p))
			return msg;
		page = vmalloc_to_page((void *)p);
		if (!(page && PagePmalloc(page)))
			return msg;
	}
	return NULL;
}
EXPORT_SYMBOL(__pmalloc_check_object);


/**
 * Early init function, the main purpose is to create the device used
 * in conjunction with genalloc, to track the pools as resources.
 * It cannot register the device because it is called very early in the
 * boot sequence and the sysfs is not yet fully initialized.
 */
int __init pmalloc_init(void)
{
	device_initialize(&pmalloc_dev);
	dev_set_name(&pmalloc_dev, "%s", "pmalloc");
	atomic_set(&into_post_init, false);
	return 0;
}

static void __pmalloc_late_add(struct device *dev, void *pool_ptr, void *d)
{
	struct pmalloc_data *data;

	data = (*(struct gen_pool **)pool_ptr)->data;
	if (!atomic_read(&data->processed)) {
		atomic_set(&data->processed, true);
		__pmalloc_connect(data);
	}
}


/**
 * When the sysfs is ready for recieving registrations, connect all the
 * pools previously created. Also enable further pools to be connected
 * right away.
 */
static int __init pmalloc_late_init(void)
{
	int retval;

	atomic_set(&into_post_init, true);
	retval = __class_register(&pmalloc_class, &pmalloc_lock_key);
	devres_for_each_res(&pmalloc_dev, devm_gen_pool_release,
			    NULL, NULL, __pmalloc_late_add, NULL);
	return retval;
}
late_initcall(pmalloc_late_init);
