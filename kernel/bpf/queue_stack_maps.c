// SPDX-License-Identifier: GPL-2.0
/*
 * queue_stack_maps.c: BPF queue and stack maps
 *
 * Copyright (c) 2018 Politecnico di Torino
 */
#include <linux/bpf.h>
#include <linux/list.h>
#include <linux/slab.h>
#include "percpu_freelist.h"

#define QUEUE_STACK_CREATE_FLAG_MASK \
	(BPF_F_NO_PREALLOC | BPF_F_NUMA_NODE | BPF_F_RDONLY | BPF_F_WRONLY)

enum queue_type {
	QUEUE,
	STACK,
};

struct bpf_queue {
	struct bpf_map map;
	struct list_head head;
	struct pcpu_freelist freelist;
	void *nodes;
	enum queue_type type;
	raw_spinlock_t lock;
	atomic_t count;
	u32 node_size;
};

struct queue_node {
	struct pcpu_freelist_node fnode;
	struct bpf_queue *queue;
	struct list_head list;
	struct rcu_head rcu;
	char element[0] __aligned(8);
};

static bool queue_map_is_prealloc(struct bpf_queue *queue)
{
	return !(queue->map.map_flags & BPF_F_NO_PREALLOC);
}

/* Called from syscall */
static int queue_map_alloc_check(union bpf_attr *attr)
{
	/* check sanity of attributes */
	if (attr->max_entries == 0 || attr->key_size != 0 ||
	    attr->value_size == 0 ||
	    attr->map_flags & ~QUEUE_STACK_CREATE_FLAG_MASK)
		return -EINVAL;

	if (attr->value_size > KMALLOC_MAX_SIZE)
		/* if value_size is bigger, the user space won't be able to
		 * access the elements.
		 */
		return -E2BIG;

	return 0;
}

static int prealloc_init(struct bpf_queue *queue)
{
	u32 node_size = sizeof(struct queue_node) +
			round_up(queue->map.value_size, 8);
	u32 num_entries = queue->map.max_entries;
	int err;

	queue->nodes = bpf_map_area_alloc(node_size * num_entries,
					  queue->map.numa_node);
	if (!queue->nodes)
		return -ENOMEM;

	err = pcpu_freelist_init(&queue->freelist);
	if (err)
		goto free_nodes;

	pcpu_freelist_populate(&queue->freelist,
			       queue->nodes +
			       offsetof(struct queue_node, fnode),
			       node_size, num_entries);

	return 0;

free_nodes:
	bpf_map_area_free(queue->nodes);
	return err;
}

static void prealloc_destroy(struct bpf_queue *queue)
{
	bpf_map_area_free(queue->nodes);
	pcpu_freelist_destroy(&queue->freelist);
}

static struct bpf_map *queue_map_alloc(union bpf_attr *attr)
{
	struct bpf_queue *queue;
	u64 cost = sizeof(*queue);
	int ret;

	queue = kzalloc(sizeof(*queue), GFP_USER);
	if (!queue)
		return ERR_PTR(-ENOMEM);

	bpf_map_init_from_attr(&queue->map, attr);

	queue->node_size = sizeof(struct queue_node) +
			   round_up(attr->value_size, 8);
	cost += (u64) attr->max_entries * queue->node_size;
	if (cost >= U32_MAX - PAGE_SIZE) {
		ret = -E2BIG;
		goto free_queue;
	}

	queue->map.pages = round_up(cost, PAGE_SIZE) >> PAGE_SHIFT;

	ret = bpf_map_precharge_memlock(queue->map.pages);
	if (ret)
		goto free_queue;

	INIT_LIST_HEAD(&queue->head);

	raw_spin_lock_init(&queue->lock);

	if (queue->map.map_type == BPF_MAP_TYPE_QUEUE)
		queue->type = QUEUE;
	else if (queue->map.map_type == BPF_MAP_TYPE_STACK)
		queue->type = STACK;

	if (queue_map_is_prealloc(queue))
		ret = prealloc_init(queue);
		if (ret)
			goto free_queue;

	return &queue->map;

free_queue:
	kfree(queue);
	return ERR_PTR(ret);
}

/* Called when map->refcnt goes to zero, either from workqueue or from syscall */
static void queue_map_free(struct bpf_map *map)
{
	struct bpf_queue *queue = container_of(map, struct bpf_queue, map);
	struct queue_node *l;

	/* at this point bpf_prog->aux->refcnt == 0 and this map->refcnt == 0,
	 * so the programs (can be more than one that used this map) were
	 * disconnected from events. Wait for outstanding critical sections in
	 * these programs to complete
	 */
	synchronize_rcu();

	/* some of queue_elem_free_rcu() callbacks for elements of this map may
	 * not have executed. Wait for them.
	 */
	rcu_barrier();
	if (!queue_map_is_prealloc(queue))
		list_for_each_entry(l, &queue->head, list) {
			list_del(&l->list);
			kfree(l);
		}
	else
		prealloc_destroy(queue);
	kfree(queue);
}

static void queue_elem_free_rcu(struct rcu_head *head)
{
	struct queue_node *l = container_of(head, struct queue_node, rcu);
	struct bpf_queue *queue = l->queue;

	/* must increment bpf_prog_active to avoid kprobe+bpf triggering while
	 * we're calling kfree, otherwise deadlock is possible if kprobes
	 * are placed somewhere inside of slub
	 */
	preempt_disable();
	__this_cpu_inc(bpf_prog_active);
	if (queue_map_is_prealloc(queue))
		pcpu_freelist_push(&queue->freelist, &l->fnode);
	else
		kfree(l);
	__this_cpu_dec(bpf_prog_active);
	preempt_enable();
}

static void *__queue_map_lookup(struct bpf_map *map, bool delete)
{
	struct bpf_queue *queue = container_of(map, struct bpf_queue, map);
	unsigned long flags;
	struct queue_node *node;

	raw_spin_lock_irqsave(&queue->lock, flags);

	if (list_empty(&queue->head)) {
		raw_spin_unlock_irqrestore(&queue->lock, flags);
		return NULL;
	}

	node = list_first_entry(&queue->head, struct queue_node, list);

	if (delete) {
		if (!queue_map_is_prealloc(queue))
			atomic_dec(&queue->count);

		list_del(&node->list);
		call_rcu(&node->rcu, queue_elem_free_rcu);
	}

	raw_spin_unlock_irqrestore(&queue->lock, flags);
	return &node->element;
}

/* Called from syscall or from eBPF program */
static void *queue_map_lookup_elem(struct bpf_map *map, void *key)
{
	return __queue_map_lookup(map, false);
}

/* Called from syscall or from eBPF program */
static void *queue_map_lookup_and_delete_elem(struct bpf_map *map, void *key)
{
	return __queue_map_lookup(map, true);
}

static struct queue_node *queue_map_delete_oldest_node(struct bpf_queue *queue)
{
	struct queue_node *node = NULL;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&queue->lock, irq_flags);

	if (list_empty(&queue->head))
		goto out;

	switch (queue->type) {
	case QUEUE:
		node = list_first_entry(&queue->head, struct queue_node, list);
		break;
	case STACK:
		node = list_last_entry(&queue->head, struct queue_node, list);
		break;
	default:
		goto out;
	}

	list_del(&node->list);
out:
	raw_spin_unlock_irqrestore(&queue->lock, irq_flags);
	return node;
}

/* Called from syscall or from eBPF program */
static int queue_map_update_elem(struct bpf_map *map, void *key, void *value,
				 u64 flags)
{
	struct bpf_queue *queue = container_of(map, struct bpf_queue, map);
	unsigned long irq_flags;
	struct queue_node *new;
	/* BPF_EXIST is used to force making room for a new element in case the
	 * map is full
	 */
	bool replace = (flags & BPF_EXIST);

	/* Check supported flags for queue and stack maps */
	if (flags & BPF_NOEXIST || flags > BPF_EXIST)
		return -EINVAL;

again:
	if (!queue_map_is_prealloc(queue)) {
		if (atomic_inc_return(&queue->count) > queue->map.max_entries) {
			atomic_dec(&queue->count);
			if (!replace)
				return -E2BIG;
			new = queue_map_delete_oldest_node(queue);
			/* It is possible that in the meanwhile the queue/stack
			 * became empty and there was not an 'oldest' element
			 * to delete.  In that case, try again
			 */
			if (!new)
				goto again;
		} else {
			new = kmalloc_node(queue->node_size,
					   GFP_ATOMIC | __GFP_NOWARN,
					   queue->map.numa_node);
			if (!new) {
				atomic_dec(&queue->count);
				return -ENOMEM;
			}
		}
	} else {
		struct pcpu_freelist_node *l;

		l = pcpu_freelist_pop(&queue->freelist);
		if (!l) {
			if (!replace)
				return -E2BIG;
			new = queue_map_delete_oldest_node(queue);
			if (!new)
			/* TODO: This should goto again, but this causes an
			 * infinite loop when the elements are not being
			 * returned to the free list by the
			 * queue_elem_free_rcu() callback
			 */
				return -ENOMEM;
		} else
			new = container_of(l, struct queue_node, fnode);
	}

	memcpy(new->element, value, queue->map.value_size);
	new->queue = queue;

	raw_spin_lock_irqsave(&queue->lock, irq_flags);
	switch (queue->type) {
	case QUEUE:
		list_add_tail(&new->list, &queue->head);
		break;

	case STACK:
		list_add(&new->list, &queue->head);
		break;
	}
	raw_spin_unlock_irqrestore(&queue->lock, irq_flags);

	return 0;
}

/* Called from syscall or from eBPF program */
static int queue_map_delete_elem(struct bpf_map *map, void *key)
{
	return -EINVAL;
}

/* Called from syscall */
static int queue_map_get_next_key(struct bpf_map *map, void *key,
				  void *next_key)
{
	return -EINVAL;
}

const struct bpf_map_ops queue_map_ops = {
	.map_alloc_check = queue_map_alloc_check,
	.map_alloc = queue_map_alloc,
	.map_free = queue_map_free,
	.map_lookup_elem = queue_map_lookup_elem,
	.map_lookup_and_delete_elem = queue_map_lookup_and_delete_elem,
	.map_update_elem = queue_map_update_elem,
	.map_delete_elem = queue_map_delete_elem,
	.map_get_next_key = queue_map_get_next_key,
};

