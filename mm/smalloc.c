/*
 * smalloc.c: Sealable Memory Allocator
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/string.h>


#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include "smalloc.h"

#define page_roundup(size) (((size) + !(size) - 1 + PAGE_SIZE) & PAGE_MASK)

#define pages_nr(size) (page_roundup(size) / PAGE_SIZE)

static struct smalloc_pool *global_pool;

struct smalloc_node *__smalloc_create_node(unsigned long words)
{
	struct smalloc_node *node;
	unsigned long size;

	/* Calculate the size to ask from vmalloc, page aligned. */
	size = page_roundup(NODE_HEADER_SIZE + words * sizeof(align_t));
	node = vmalloc(size);
	if (!node) {
		pr_err("No memory for allocating smalloc node.");
		return NULL;
	}
	/* Initialize the node.*/
	INIT_LIST_HEAD(&node->list);
	node->free = node->data;
	node->available_words = (size - NODE_HEADER_SIZE) / sizeof(align_t);
	return node;
}

static __always_inline
void *node_alloc(struct smalloc_node *node, unsigned long words)
{
	register align_t *old_free = node->free;

	node->available_words -= words;
	node->free += words;
	return old_free;
}

void *smalloc(unsigned long size, struct smalloc_pool *pool)
{
	struct list_head *pos;
	struct smalloc_node *node;
	void *ptr;
	unsigned long words;

	/* If no pool specified, use the global one. */
	if (!pool)
		pool = global_pool;

	mutex_lock(&pool->lock);

	/* If the pool is sealed, then return NULL. */
	if (pool->seal == SMALLOC_SEALED) {
		mutex_unlock(&pool->lock);
		return NULL;
	}

	/* Calculate minimum number of words required. */
	words = (size + sizeof(align_t) - 1) / sizeof(align_t);

	/* Look for slot that is large enough, in the existing pool.*/
	list_for_each(pos, &pool->list) {
		node = list_entry(pos, struct smalloc_node, list);
		if (node->available_words >= words) {
			ptr = node_alloc(node, words);
			mutex_unlock(&pool->lock);
			return ptr;
		}
	}

	/* No slot found, get a new chunk of virtual memory. */
	node = __smalloc_create_node(words);
	if (!node) {
		mutex_unlock(&pool->lock);
		return NULL;
	}

	list_add(&node->list, &pool->list);
	ptr = node_alloc(node, words);
	mutex_unlock(&pool->lock);
	return ptr;
}

static __always_inline
unsigned long get_node_size(struct smalloc_node *node)
{
	if (!node)
		return 0;
	return page_roundup((((void *)node->free) - (void *)node) +
			    node->available_words * sizeof(align_t));
}

static __always_inline
unsigned long get_node_pages_nr(struct smalloc_node *node)
{
	return pages_nr(get_node_size(node));
}
void smalloc_seal_set(enum seal_t seal, struct smalloc_pool *pool)
{
	struct list_head *pos;
	struct smalloc_node *node;

	if (!pool)
		pool = global_pool;
	mutex_lock(&pool->lock);
	if (pool->seal == seal) {
		mutex_unlock(&pool->lock);
		return;
	}
	list_for_each(pos, &pool->list) {
		node = list_entry(pos, struct smalloc_node, list);
		if (seal == SMALLOC_SEALED)
			set_memory_ro((unsigned long)node,
				      get_node_pages_nr(node));
		else if (seal == SMALLOC_UNSEALED)
			set_memory_rw((unsigned long)node,
				      get_node_pages_nr(node));
	}
	pool->seal = seal;
	mutex_unlock(&pool->lock);
}

int smalloc_initialize(struct smalloc_pool *pool)
{
	if (!pool)
		return -EINVAL;
	INIT_LIST_HEAD(&pool->list);
	pool->seal = SMALLOC_UNSEALED;
	mutex_init(&pool->lock);
	return 0;
}

struct smalloc_pool *smalloc_create(void)
{
	struct smalloc_pool *pool = vmalloc(sizeof(struct smalloc_pool));

	if (!pool) {
		pr_err("No memory for allocating pool.");
		return NULL;
	}
	smalloc_initialize(pool);
	return pool;
}

int smalloc_destroy(struct smalloc_pool *pool)
{
	struct list_head *pos, *q;
	struct smalloc_node *node;

	if (!pool)
		return -EINVAL;
	list_for_each_safe(pos, q, &pool->list) {
		node = list_entry(pos, struct smalloc_node, list);
		list_del(pos);
		vfree(node);
	}
	return 0;
}

static int __init smalloc_init(void)
{
	global_pool = smalloc_create();
	if (!global_pool) {
		pr_err("Module smalloc initialization failed: no memory.\n");
		return -ENOMEM;
	}
	pr_info("Module smalloc initialized successfully.\n");
	return 0;
}

static void __exit smalloc_exit(void)
{
	pr_info("Module smalloc un initialized successfully.\n");
}

module_init(smalloc_init);
module_exit(smalloc_exit);
MODULE_LICENSE("GPL");
