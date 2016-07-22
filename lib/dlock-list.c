/*
 * Distributed and locked list
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * (C) Copyright 2016 Hewlett-Packard Enterprise Development LP
 *
 * Authors: Waiman Long <waiman.long@hpe.com>
 */
#include <linux/dlock-list.h>
#include <linux/lockdep.h>

/*
 * As all the locks in the dlock list are dynamically allocated, they need
 * to belong to their own special lock class to avoid warning and stack
 * trace in kernel log when lockdep is enabled. Statically allocated locks
 * don't have this problem.
 */
static struct lock_class_key dlock_list_key;

/**
 * alloc_dlock_list_head - Initialize and allocate the per-cpu list head
 * @dlist: Pointer to the dlock_list_head structure to be initialized
 * Return: 0 if successful, -ENOMEM if memory allocation error
 *
 * This function does not allocate the dlock_list_head structure itself. The
 * callers will have to do their own memory allocation, if necessary. However,
 * this allows embedding the dlock_list_head structure directly into other
 * structures.
 */
int alloc_dlock_list_head(struct dlock_list_head *dlist)
{
	struct dlock_list_head dlist_tmp;
	int cpu;

	dlist_tmp.head = alloc_percpu(struct dlock_list_head_percpu);
	if (!dlist_tmp.head)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct dlock_list_head_percpu *head;

		head = per_cpu_ptr(dlist_tmp.head, cpu);
		INIT_LIST_HEAD(&head->list);
		head->lock = __SPIN_LOCK_UNLOCKED(&head->lock);
		lockdep_set_class(&head->lock, &dlock_list_key);
	}

	dlist->head = dlist_tmp.head;
	return 0;
}

/**
 * free_dlock_list_head - Free the per-cpu list head of dlock list
 * @dlist: Pointer of the dlock_list_head structure to be freed
 *
 * This function doesn't free the dlock_list_head structure itself. So
 * the caller will have to do it, if necessary.
 */
void free_dlock_list_head(struct dlock_list_head *dlist)
{
	free_percpu(dlist->head);
	dlist->head = NULL;
}

/**
 * dlock_list_add - Adds a node to the given dlock list
 * @node : The node to be added
 * @dlist: The dlock list where the node is to be added
 *
 * List selection is based on the CPU being used when the dlock_list_add()
 * function is called. However, deletion may be done by a different CPU.
 * So we still need to use a lock to protect the content of the list.
 */
void dlock_list_add(struct dlock_list_node *node,
		    struct dlock_list_head *dlist)
{
	struct dlock_list_head_percpu *head;

	/*
	 * Disable preemption to make sure that CPU won't gets changed.
	 */
	head = get_cpu_ptr(dlist->head);
	spin_lock(&head->lock);
	node->lockptr = &head->lock;
	list_add(&node->list, &head->list);
	spin_unlock(&head->lock);
	put_cpu_ptr(dlist->head);
}

/**
 * dlock_list_del - Delete a node from a dlock list
 * @node : The node to be deleted
 *
 * We need to check the lock pointer again after taking the lock to guard
 * against concurrent deletion of the same node. If the lock pointer changes
 * (becomes NULL or to a different one), we assume that the deletion was done
 * elsewhere. A warning will be printed if this happens as it is likely to be
 * a bug.
 */
void dlock_list_del(struct dlock_list_node *node)
{
	spinlock_t *lock;
	bool retry;

	do {
		lock = READ_ONCE(node->lockptr);
		if (WARN_ONCE(!lock,
			"dlock_list_del: node 0x%lx has no associated lock\n",
			(unsigned long)node))
			return;

		spin_lock(lock);
		if (likely(lock == node->lockptr)) {
			list_del_init(&node->list);
			node->lockptr = NULL;
			retry = false;
		} else {
			/*
			 * The lock has somehow changed. Retry again if it is
			 * not NULL. Otherwise, just ignore the delete
			 * operation.
			 */
			retry = (node->lockptr != NULL);
		}
		spin_unlock(lock);
	} while (retry);
}

/**
 * __dlock_list_next_cpu: Find the first entry of the next per-cpu list
 * @dlist: Pointer to the dlock_list_head structure
 * @iter : Pointer to the dlock list iterator structure
 * Return: true if the entry is found, false if all the lists exhausted
 *
 * The information about the next per-cpu list will be put into the iterator.
 */
struct dlock_list_node *__dlock_list_next_cpu(struct dlock_list_iter *iter)
{
	struct dlock_list_node *next;

	if (iter->pcpu_head) {
		spin_unlock(&iter->pcpu_head->lock);
		iter->pcpu_head = NULL;
	}

next_cpu:
	/*
	 * for_each_possible_cpu(cpu)
	 */
	iter->cpu = cpumask_next(iter->cpu, cpu_possible_mask);
	if (iter->cpu >= nr_cpu_ids)
		return NULL;	/* All the per-cpu lists iterated */

	iter->pcpu_head = per_cpu_ptr(iter->head->head, iter->cpu);
	if (list_empty(&iter->pcpu_head->list))
		goto next_cpu;

	spin_lock(&iter->pcpu_head->lock);
	/*
	 * There is a slight chance that the list may become empty just
	 * before the lock is acquired. So an additional check is
	 * needed to make sure that iter->curr points to a valid entry.
	 */
	if (list_empty(&iter->pcpu_head->list)) {
		spin_unlock(&iter->pcpu_head->lock);
		goto next_cpu;
	}
	next = list_entry(iter->pcpu_head->list.next, struct dlock_list_node,
			  list);
	WARN_ON_ONCE(next->lockptr != &iter->pcpu_head->lock);

	return next;
}
