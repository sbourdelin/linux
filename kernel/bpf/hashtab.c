/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */
#include <linux/bpf.h>
#include <linux/jhash.h>
#include <linux/filter.h>
#include <linux/vmalloc.h>
#include <linux/percpu_ida.h>

/* each htab element is struct htab_elem + key + value */
struct htab_elem {
	u32 tag;
	union {
		/* won't be used after being removed from hash */
		struct {
			u32 hash;
			struct hlist_node hash_node;
		};

		/* set after being deleted from hash */
		struct {
			struct bpf_htab *htab;
			struct rcu_head rcu;
		};
	};
	char key[0] __aligned(8);
};

struct bpf_htab {
	struct bpf_map map;
	struct hlist_head *buckets;
	u32 n_buckets;	/* number of hash buckets */
	u32 elem_size;	/* size of each element in bytes */

	struct list_head page_list;
	struct htab_elem **elems;
	struct percpu_ida elems_pool;
};

static size_t order_to_size(unsigned int order)
{
	return (size_t)PAGE_SIZE << order;
}

/* Called from syscall, and the code is borrowed from blk_mq */
static int htab_pre_alloc_elems(struct bpf_htab *htab)
{
	const unsigned max_order = 4;
	unsigned elem_size = htab->elem_size, i;
	unsigned nr_entries = htab->map.max_entries;
	size_t left = nr_entries * elem_size;

	htab->elems = kzalloc(nr_entries * sizeof(struct htab_elem *),
			      GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
	if (!htab->elems)
		goto fail;

	INIT_LIST_HEAD(&htab->page_list);

	for (i = 0; i < nr_entries; ) {
		int this_order = max_order;
		struct page *page;
		int j, to_do;
		void *p;

		while (left < order_to_size(this_order - 1) && this_order)
			this_order--;

		do {
			page = alloc_pages(GFP_KERNEL | __GFP_NOWARN |
					   __GFP_NORETRY | __GFP_ZERO,
					   this_order);
			if (page)
				break;
			if (!this_order--)
				break;
			if (order_to_size(this_order) < elem_size)
				break;
		} while (1);

		if (!page)
			goto fail;

		page->private = this_order;
		list_add_tail(&page->lru, &htab->page_list);

		p = page_address(page);

		to_do = min_t(unsigned,
			      order_to_size(this_order) / elem_size,
			      nr_entries - i);
		left -= to_do * elem_size;

		for (j = 0; j < to_do; j++) {
			htab->elems[i] = p;
			p += elem_size;
			i++;
		}
	}
	return 0;

fail:
	kfree(htab->elems);
	return -ENOMEM;
}

static void htab_destroy_elems(struct bpf_htab *htab)
{
	struct page *page;

	while (!list_empty(&htab->page_list)) {
		page = list_first_entry(&htab->page_list, struct page, lru);
		list_del_init(&page->lru);
		__free_pages(page, page->private);
	}

	kfree(htab->elems);
}

static int htab_init_elems_allocator(struct bpf_htab *htab)
{
	int ret = htab_pre_alloc_elems(htab);

	if (ret)
		return ret;

	ret = percpu_ida_init(&htab->elems_pool, htab->map.max_entries);
	if (ret)
		htab_destroy_elems(htab);
	return ret;
}

static void htab_deinit_elems_allocator(struct bpf_htab *htab)
{
	htab_destroy_elems(htab);
	percpu_ida_destroy(&htab->elems_pool);
}

static struct htab_elem *htab_alloc_elem(struct bpf_htab *htab)
{
	int tag = percpu_ida_alloc(&htab->elems_pool, TASK_RUNNING);
	struct htab_elem *elem;

	if (tag < 0)
		return NULL;

	elem = htab->elems[tag];
	elem->tag = tag;
	return elem;
}

static void htab_free_elem(struct bpf_htab *htab, struct htab_elem *elem)
{
	percpu_ida_free(&htab->elems_pool, elem->tag);
}

static void htab_free_elem_cb(struct rcu_head *head)
{
	struct htab_elem *elem = container_of(head, struct htab_elem, rcu);

	htab_free_elem(elem->htab, elem);
}

static void htab_free_elem_rcu(struct bpf_htab *htab,
			       struct htab_elem *elem)
{
	hlist_del_rcu_lock(&elem->hash_node);
	elem->htab = htab;
	call_rcu(&elem->rcu, htab_free_elem_cb);
}

/* Called from syscall */
static struct bpf_map *htab_map_alloc(union bpf_attr *attr)
{
	struct bpf_htab *htab;
	int err, i;

	htab = kzalloc(sizeof(*htab), GFP_USER);
	if (!htab)
		return ERR_PTR(-ENOMEM);

	/* mandatory map attributes */
	htab->map.key_size = attr->key_size;
	htab->map.value_size = attr->value_size;
	htab->map.max_entries = attr->max_entries;

	/* check sanity of attributes.
	 * value_size == 0 may be allowed in the future to use map as a set
	 */
	err = -EINVAL;
	if (htab->map.max_entries == 0 || htab->map.key_size == 0 ||
	    htab->map.value_size == 0)
		goto free_htab;

	/* hash table size must be power of 2 */
	htab->n_buckets = roundup_pow_of_two(htab->map.max_entries);

	err = -E2BIG;
	if (htab->map.key_size > MAX_BPF_STACK)
		/* eBPF programs initialize keys on stack, so they cannot be
		 * larger than max stack size
		 */
		goto free_htab;

	if (htab->map.value_size >= (1 << (KMALLOC_SHIFT_MAX - 1)) -
	    MAX_BPF_STACK - sizeof(struct htab_elem))
		/* if value_size is bigger, the user space won't be able to
		 * access the elements via bpf syscall. This check also makes
		 * sure that the elem_size doesn't overflow and it's
		 * kmalloc-able later in htab_map_update_elem()
		 */
		goto free_htab;

	htab->elem_size = round_up(sizeof(struct htab_elem) +
				   round_up(htab->map.key_size, 8) +
				   htab->map.value_size,
				   cache_line_size());

	/* prevent zero size kmalloc and check for u32 overflow */
	if (htab->n_buckets == 0 ||
	    htab->n_buckets > U32_MAX / sizeof(struct hlist_head))
		goto free_htab;

	if ((u64) htab->n_buckets * sizeof(struct hlist_head) +
	    (u64) htab->elem_size * htab->map.max_entries >=
	    U32_MAX - PAGE_SIZE)
		/* make sure page count doesn't overflow */
		goto free_htab;

	htab->map.pages = round_up(htab->n_buckets * sizeof(struct hlist_head) +
				   htab->elem_size * htab->map.max_entries,
				   PAGE_SIZE) >> PAGE_SHIFT;

	err = -ENOMEM;
	htab->buckets = kmalloc_array(htab->n_buckets, sizeof(struct hlist_head),
				      GFP_USER | __GFP_NOWARN);

	if (!htab->buckets) {
		htab->buckets = vmalloc(htab->n_buckets * sizeof(struct hlist_head));
		if (!htab->buckets)
			goto free_htab;
	}

	for (i = 0; i < htab->n_buckets; i++)
		INIT_HLIST_HEAD(&htab->buckets[i]);

	err = htab_init_elems_allocator(htab);
	if (err)
		goto free_buckets;

	return &htab->map;

free_buckets:
	kvfree(htab->buckets);
free_htab:
	kfree(htab);
	return ERR_PTR(err);
}

static inline u32 htab_map_hash(const void *key, u32 key_len)
{
	return jhash(key, key_len, 0);
}

static inline struct hlist_head *select_bucket(struct bpf_htab *htab, u32 hash)
{
	return &htab->buckets[hash & (htab->n_buckets - 1)];
}

static struct htab_elem *lookup_elem_raw(struct hlist_head *head, u32 hash,
					 void *key, u32 key_size)
{
	struct htab_elem *l;

	hlist_for_each_entry_rcu(l, head, hash_node)
		if (l->hash == hash && !memcmp(&l->key, key, key_size))
			return l;

	return NULL;
}

/* Called from syscall or from eBPF program */
static void *htab_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct hlist_head h;
	struct htab_elem *l;
	u32 hash, key_size;

	/* Must be called with rcu_read_lock. */
	WARN_ON_ONCE(!rcu_read_lock_held());

	key_size = map->key_size;

	hash = htab_map_hash(key, key_size);

	head = select_bucket(htab, hash);
	head = hlist_get_head_lock(head, &h);

	l = lookup_elem_raw(head, hash, key, key_size);

	if (l)
		return l->key + round_up(map->key_size, 8);

	return NULL;
}

/* Called from syscall */
static int htab_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct hlist_head h;
	struct htab_elem *l, *next_l;
	u32 hash, key_size;
	int i;

	WARN_ON_ONCE(!rcu_read_lock_held());

	key_size = map->key_size;

	hash = htab_map_hash(key, key_size);

	head = select_bucket(htab, hash);
	head = hlist_get_head_lock(head, &h);

	/* lookup the key */
	l = lookup_elem_raw(head, hash, key, key_size);

	if (!l) {
		i = 0;
		goto find_first_elem;
	}

	/* key was found, get next key in the same bucket */
	next_l = hlist_entry_safe(rcu_dereference_raw(hlist_next_rcu(&l->hash_node)),
				  struct htab_elem, hash_node);

	if (next_l) {
		/* if next elem in this hash list is non-zero, just return it */
		memcpy(next_key, next_l->key, key_size);
		return 0;
	}

	/* no more elements in this hash list, go to the next bucket */
	i = hash & (htab->n_buckets - 1);
	i++;

find_first_elem:
	/* iterate over buckets */
	for (; i < htab->n_buckets; i++) {
		head = select_bucket(htab, i);
		head = hlist_get_head_lock(head, &h);

		/* pick first element in the bucket */
		next_l = hlist_entry_safe(rcu_dereference_raw(hlist_first_rcu(head)),
					  struct htab_elem, hash_node);
		if (next_l) {
			/* if it's not empty, just return it */
			memcpy(next_key, next_l->key, key_size);
			return 0;
		}
	}

	/* itereated over all buckets and all elements */
	return -ENOENT;
}

/* Called from syscall or from eBPF program */
static int htab_map_update_elem(struct bpf_map *map, void *key, void *value,
				u64 map_flags)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct htab_elem *l_new, *l_old;
	struct hlist_head *head;
	struct hlist_head h;
	unsigned long flags;
	u32 key_size;
	int ret;

	if (map_flags > BPF_EXIST)
		/* unknown flags */
		return -EINVAL;

	WARN_ON_ONCE(!rcu_read_lock_held());

	/* allocate new element outside of lock */
	l_new = htab_alloc_elem(htab);
	if (!l_new)
		return -E2BIG;

	key_size = map->key_size;

	memcpy(l_new->key, key, key_size);
	memcpy(l_new->key + round_up(key_size, 8), value, map->value_size);

	l_new->hash = htab_map_hash(l_new->key, key_size);
	head = select_bucket(htab, l_new->hash);

	/* bpf_map_update_elem() can be called in_irq() */
	raw_local_irq_save(flags);
	bit_spin_lock(HLIST_LOCK_BIT, (unsigned long *)&head->first);

	l_old = lookup_elem_raw(hlist_get_head_lock(head, &h), l_new->hash,
			key, key_size);

	if (l_old && map_flags == BPF_NOEXIST) {
		/* elem already exists */
		ret = -EEXIST;
		goto err;
	}

	if (!l_old && map_flags == BPF_EXIST) {
		/* elem doesn't exist, cannot update it */
		ret = -ENOENT;
		goto err;
	}

	/* add new element to the head of the list, so that concurrent
	 * search will find it before old elem
	 */
	hlist_add_head_rcu_lock(&l_new->hash_node, head);
	if (l_old)
		htab_free_elem_rcu(htab, l_old);
	bit_spin_unlock(HLIST_LOCK_BIT, (unsigned long *)&head->first);
	raw_local_irq_restore(flags);

	return 0;
err:
	bit_spin_unlock(HLIST_LOCK_BIT, (unsigned long *)&head->first);
	raw_local_irq_restore(flags);
	htab_free_elem(htab, l_new);
	return ret;
}

/* Called from syscall or from eBPF program */
static int htab_map_delete_elem(struct bpf_map *map, void *key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct hlist_head h;
	struct htab_elem *l;
	unsigned long flags;
	u32 hash, key_size;
	int ret = -ENOENT;

	WARN_ON_ONCE(!rcu_read_lock_held());

	key_size = map->key_size;

	hash = htab_map_hash(key, key_size);
	head = select_bucket(htab, hash);

	raw_local_irq_save(flags);
	bit_spin_lock(HLIST_LOCK_BIT, (unsigned long *)&head->first);

	l = lookup_elem_raw(hlist_get_head_lock(head, &h), hash, key, key_size);
	if (l) {
		htab_free_elem_rcu(htab, l);
		ret = 0;
	}

	bit_spin_unlock(HLIST_LOCK_BIT, (unsigned long *)&head->first);
	raw_local_irq_restore(flags);
	return ret;
}

static void delete_all_elements(struct bpf_htab *htab)
{
	int i;

	for (i = 0; i < htab->n_buckets; i++) {
		struct hlist_head *head = select_bucket(htab, i);
		struct hlist_head h;
		struct hlist_node *n;
		struct htab_elem *l;

		head = hlist_get_head_lock(head, &h);

		hlist_for_each_entry_safe(l, n, head, hash_node)
			hlist_del_rcu(&l->hash_node);
	}
}

/* Called when map->refcnt goes to zero, either from workqueue or from syscall */
static void htab_map_free(struct bpf_map *map)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);

	/* at this point bpf_prog->aux->refcnt == 0 and this map->refcnt == 0,
	 * so the programs (can be more than one that used this map) were
	 * disconnected from events. Wait for outstanding critical sections in
	 * these programs to complete
	 */
	synchronize_rcu();

	/* some of kfree_rcu() callbacks for elements of this map may not have
	 * executed. It's ok. Proceed to free residual elements and map itself
	 */
	delete_all_elements(htab);
	htab_deinit_elems_allocator(htab);
	kvfree(htab->buckets);
	kfree(htab);
}

static const struct bpf_map_ops htab_ops = {
	.map_alloc = htab_map_alloc,
	.map_free = htab_map_free,
	.map_get_next_key = htab_map_get_next_key,
	.map_lookup_elem = htab_map_lookup_elem,
	.map_update_elem = htab_map_update_elem,
	.map_delete_elem = htab_map_delete_elem,
};

static struct bpf_map_type_list htab_type __read_mostly = {
	.ops = &htab_ops,
	.type = BPF_MAP_TYPE_HASH,
};

static int __init register_htab_map(void)
{
	bpf_register_map_type(&htab_type);
	return 0;
}
late_initcall(register_htab_map);
