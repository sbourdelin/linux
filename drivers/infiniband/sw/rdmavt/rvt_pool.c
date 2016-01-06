/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rvt_loc.h"

/* info about object pools
   note that mr, fmr and mw share a single index space
   so that one can map an lkey to the correct type of object */
struct rvt_type_info rvt_type_info[RVT_NUM_TYPES] = {
	[RVT_TYPE_UC] = {
		.name		= "uc",
		.size		= sizeof(struct rvt_ucontext),
	},
	[RVT_TYPE_PD] = {
		.name		= "pd",
		.size		= sizeof(struct rvt_pd),
	},
	[RVT_TYPE_AH] = {
		.name		= "ah",
		.size		= sizeof(struct rvt_ah),
		.flags		= RVT_POOL_ATOMIC,
	},
	[RVT_TYPE_SRQ] = {
		.name		= "srq",
		.size		= sizeof(struct rvt_srq),
		.flags		= RVT_POOL_INDEX,
		.min_index	= RVT_MIN_SRQ_INDEX,
		.max_index	= RVT_MAX_SRQ_INDEX,
	},
	[RVT_TYPE_QP] = {
		.name		= "qp",
		.size		= sizeof(struct rvt_qp),
		.cleanup	= rvt_qp_cleanup,
		.flags		= RVT_POOL_INDEX,
		.min_index	= RVT_MIN_QP_INDEX,
		.max_index	= RVT_MAX_QP_INDEX,
	},
	[RVT_TYPE_CQ] = {
		.name		= "cq",
		.size		= sizeof(struct rvt_cq),
		.cleanup	= rvt_cq_cleanup,
	},
	[RVT_TYPE_MR] = {
		.name		= "mr",
		.size		= sizeof(struct rvt_mem),
		.cleanup	= rvt_mem_cleanup,
		.flags		= RVT_POOL_INDEX,
		.max_index	= RVT_MAX_MR_INDEX,
		.min_index	= RVT_MIN_MR_INDEX,
	},
	[RVT_TYPE_FMR] = {
		.name		= "fmr",
		.size		= sizeof(struct rvt_mem),
		.cleanup	= rvt_mem_cleanup,
		.flags		= RVT_POOL_INDEX,
		.max_index	= RVT_MAX_FMR_INDEX,
		.min_index	= RVT_MIN_FMR_INDEX,
	},
	[RVT_TYPE_MW] = {
		.name		= "mw",
		.size		= sizeof(struct rvt_mem),
		.flags		= RVT_POOL_INDEX,
		.max_index	= RVT_MAX_MW_INDEX,
		.min_index	= RVT_MIN_MW_INDEX,
	},
	[RVT_TYPE_MC_GRP] = {
		.name		= "mc_grp",
		.size		= sizeof(struct rvt_mc_grp),
		.cleanup	= rvt_mc_cleanup,
		.flags		= RVT_POOL_KEY,
		.key_offset	= offsetof(struct rvt_mc_grp, mgid),
		.key_size	= sizeof(union ib_gid),
	},
	[RVT_TYPE_MC_ELEM] = {
		.name		= "mc_elem",
		.size		= sizeof(struct rvt_mc_elem),
		.flags		= RVT_POOL_ATOMIC,
	},
};

static inline char *pool_name(struct rvt_pool *pool)
{
	return rvt_type_info[pool->type].name;
}

static inline struct kmem_cache *pool_cache(struct rvt_pool *pool)
{
	return rvt_type_info[pool->type].cache;
}

static inline enum rvt_elem_type rvt_type(void *arg)
{
	struct rvt_pool_entry *elem = arg;

	return elem->pool->type;
}

int __init rvt_cache_init(void)
{
	int err;
	int i;
	size_t size;
	struct rvt_type_info *type;

	for (i = 0; i < RVT_NUM_TYPES; i++) {
		type = &rvt_type_info[i];
		size = ALIGN(type->size, RVT_POOL_ALIGN);
		type->cache = kmem_cache_create(type->name, size,
				RVT_POOL_ALIGN,
				RVT_POOL_CACHE_FLAGS, NULL);
		if (!type->cache) {
			pr_err("Unable to init kmem cache for %s\n",
				type->name);
			err = -ENOMEM;
			goto err1;
		}
	}

	return 0;

err1:
	while (--i >= 0) {
		kmem_cache_destroy(type->cache);
		type->cache = NULL;
	}

	return err;
}

void __exit rvt_cache_exit(void)
{
	int i;
	struct rvt_type_info *type;

	for (i = 0; i < RVT_NUM_TYPES; i++) {
		type = &rvt_type_info[i];
		kmem_cache_destroy(type->cache);
		type->cache = NULL;
	}
}

static int rvt_pool_init_index(struct rvt_pool *pool, u32 max, u32 min)
{
	int err = 0;
	size_t size;

	if ((max - min + 1) < pool->max_elem) {
		pr_warn("not enough indices for max_elem\n");
		err = -EINVAL;
		goto out;
	}

	pool->max_index = max;
	pool->min_index = min;

	size = BITS_TO_LONGS(max - min + 1) * sizeof(long);
	pool->table = kmalloc(size, GFP_KERNEL);
	if (!pool->table) {
		pr_warn("no memory for bit table\n");
		err = -ENOMEM;
		goto out;
	}

	pool->table_size = size;
	bitmap_zero(pool->table, max - min + 1);

out:
	return err;
}

int rvt_pool_init(
	struct rvt_dev		*rvt,
	struct rvt_pool		*pool,
	enum rvt_elem_type	type,
	unsigned		max_elem)
{
	int			err = 0;
	size_t			size = rvt_type_info[type].size;

	memset(pool, 0, sizeof(*pool));

	pool->rvt		= rvt;
	pool->type		= type;
	pool->max_elem		= max_elem;
	pool->elem_size		= ALIGN(size, RVT_POOL_ALIGN);
	pool->flags		= rvt_type_info[type].flags;
	pool->tree		= RB_ROOT;
	pool->cleanup		= rvt_type_info[type].cleanup;

	atomic_set(&pool->num_elem, 0);

	kref_init(&pool->ref_cnt);

	spin_lock_init(&pool->pool_lock);

	if (rvt_type_info[type].flags & RVT_POOL_INDEX) {
		err = rvt_pool_init_index(pool,
					  rvt_type_info[type].max_index,
					  rvt_type_info[type].min_index);
		if (err)
			goto out;
	}

	if (rvt_type_info[type].flags & RVT_POOL_KEY) {
		pool->key_offset = rvt_type_info[type].key_offset;
		pool->key_size = rvt_type_info[type].key_size;
	}

	pool->state = rvt_pool_valid;

out:
	return err;
}

static void rvt_pool_release(struct kref *kref)
{
	struct rvt_pool *pool = container_of(kref, struct rvt_pool, ref_cnt);

	pool->state = rvt_pool_invalid;
	kfree(pool->table);
}

void rvt_pool_put(struct rvt_pool *pool)
{
	kref_put(&pool->ref_cnt, rvt_pool_release);
}


int rvt_pool_cleanup(struct rvt_pool *pool)
{
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);
	pool->state = rvt_pool_invalid;
	spin_unlock_irqrestore(&pool->pool_lock, flags);

	if (atomic_read(&pool->num_elem) > 0)
		pr_warn("%s pool destroyed with unfree'd elem\n",
			pool_name(pool));

	rvt_pool_put(pool);

	return 0;
}

static u32 alloc_index(struct rvt_pool *pool)
{
	u32 index;
	u32 range = pool->max_index - pool->min_index + 1;

	index = find_next_zero_bit(pool->table, range, pool->last);
	if (index >= range)
		index = find_first_zero_bit(pool->table, range);

	set_bit(index, pool->table);
	pool->last = index;
	return index + pool->min_index;
}

static void insert_index(struct rvt_pool *pool, struct rvt_pool_entry *new)
{
	struct rb_node **link = &pool->tree.rb_node;
	struct rb_node *parent = NULL;
	struct rvt_pool_entry *elem;

	while (*link) {
		parent = *link;
		elem = rb_entry(parent, struct rvt_pool_entry, node);

		if (elem->index == new->index) {
			pr_warn("element already exists!\n");
			goto out;
		}

		if (elem->index > new->index)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &pool->tree);
out:
	return;
}

static void insert_key(struct rvt_pool *pool, struct rvt_pool_entry *new)
{
	struct rb_node **link = &pool->tree.rb_node;
	struct rb_node *parent = NULL;
	struct rvt_pool_entry *elem;
	int cmp;

	while (*link) {
		parent = *link;
		elem = rb_entry(parent, struct rvt_pool_entry, node);

		cmp = memcmp((u8 *)elem + pool->key_offset,
			     (u8 *)new + pool->key_offset, pool->key_size);

		if (cmp == 0) {
			pr_warn("key already exists!\n");
			goto out;
		}

		if (cmp > 0)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &pool->tree);
out:
	return;
}

void rvt_add_key(void *arg, void *key)
{
	struct rvt_pool_entry *elem = arg;
	struct rvt_pool *pool = elem->pool;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);
	memcpy((u8 *)elem + pool->key_offset, key, pool->key_size);
	insert_key(pool, elem);
	spin_unlock_irqrestore(&pool->pool_lock, flags);
}

void rvt_drop_key(void *arg)
{
	struct rvt_pool_entry *elem = arg;
	struct rvt_pool *pool = elem->pool;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);
	rb_erase(&elem->node, &pool->tree);
	spin_unlock_irqrestore(&pool->pool_lock, flags);
}

void rvt_add_index(void *arg)
{
	struct rvt_pool_entry *elem = arg;
	struct rvt_pool *pool = elem->pool;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);
	elem->index = alloc_index(pool);
	insert_index(pool, elem);
	spin_unlock_irqrestore(&pool->pool_lock, flags);
}

void rvt_drop_index(void *arg)
{
	struct rvt_pool_entry *elem = arg;
	struct rvt_pool *pool = elem->pool;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);
	clear_bit(elem->index - pool->min_index, pool->table);
	rb_erase(&elem->node, &pool->tree);
	spin_unlock_irqrestore(&pool->pool_lock, flags);
}

void *rvt_alloc(struct rvt_pool *pool)
{
	struct rvt_pool_entry *elem;
	unsigned long flags;

	might_sleep_if(!(pool->flags & RVT_POOL_ATOMIC));

	spin_lock_irqsave(&pool->pool_lock, flags);
	if (pool->state != rvt_pool_valid) {
		spin_unlock_irqrestore(&pool->pool_lock, flags);
		return NULL;
	}
	kref_get(&pool->ref_cnt);
	spin_unlock_irqrestore(&pool->pool_lock, flags);

	kref_get(&pool->rvt->ref_cnt);

	if (atomic_inc_return(&pool->num_elem) > pool->max_elem) {
		atomic_dec(&pool->num_elem);
		rvt_dev_put(pool->rvt);
		rvt_pool_put(pool);
		return NULL;
	}

	elem = kmem_cache_zalloc(pool_cache(pool),
				 (pool->flags & RVT_POOL_ATOMIC) ?
				 GFP_ATOMIC : GFP_KERNEL);

	elem->pool = pool;
	kref_init(&elem->ref_cnt);

	return elem;
}

void rvt_elem_release(struct kref *kref)
{
	struct rvt_pool_entry *elem =
		container_of(kref, struct rvt_pool_entry, ref_cnt);
	struct rvt_pool *pool = elem->pool;

	if (pool->cleanup)
		pool->cleanup(elem);

	kmem_cache_free(pool_cache(pool), elem);
	atomic_dec(&pool->num_elem);
	rvt_dev_put(pool->rvt);
	rvt_pool_put(pool);
}

void *rvt_pool_get_index(struct rvt_pool *pool, u32 index)
{
	struct rb_node *node = NULL;
	struct rvt_pool_entry *elem = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);

	if (pool->state != rvt_pool_valid)
		goto out;

	node = pool->tree.rb_node;

	while (node) {
		elem = rb_entry(node, struct rvt_pool_entry, node);

		if (elem->index > index)
			node = node->rb_left;
		else if (elem->index < index)
			node = node->rb_right;
		else
			break;
	}

	if (node)
		kref_get(&elem->ref_cnt);

out:
	spin_unlock_irqrestore(&pool->pool_lock, flags);
	return node ? (void *)elem : NULL;
}

void *rvt_pool_get_key(struct rvt_pool *pool, void *key)
{
	struct rb_node *node = NULL;
	struct rvt_pool_entry *elem = NULL;
	int cmp;
	unsigned long flags;

	spin_lock_irqsave(&pool->pool_lock, flags);

	if (pool->state != rvt_pool_valid)
		goto out;

	node = pool->tree.rb_node;

	while (node) {
		elem = rb_entry(node, struct rvt_pool_entry, node);

		cmp = memcmp((u8 *)elem + pool->key_offset,
			     key, pool->key_size);

		if (cmp > 0)
			node = node->rb_left;
		else if (cmp < 0)
			node = node->rb_right;
		else
			break;
	}

	if (node)
		kref_get(&elem->ref_cnt);

out:
	spin_unlock_irqrestore(&pool->pool_lock, flags);
	return node ? ((void *)elem) : NULL;
}
