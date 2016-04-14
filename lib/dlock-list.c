/*
 * Distributed/locked list
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
 * The dlock list lock needs its own class to avoid warning and stack
 * trace when lockdep is enabled.
 */
static struct lock_class_key dlock_list_key;

/*
 * Initialize the per-cpu list head
 */
int init_dlock_list_head(struct dlock_list_head __percpu **pdlock_head)
{
	struct dlock_list_head *dlock_head;
	int cpu;

	dlock_head = alloc_percpu(struct dlock_list_head);
	if (!dlock_head)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct dlock_list_head *head = per_cpu_ptr(dlock_head, cpu);

		INIT_LIST_HEAD(&head->list);
		head->lock = __SPIN_LOCK_UNLOCKED(&head->lock);
		lockdep_set_class(&head->lock, &dlock_list_key);
	}

	*pdlock_head = dlock_head;
	return 0;
}

/*
 * List selection is based on the CPU being used when the dlock_list_add()
 * function is called. However, deletion may be done by a different CPU.
 * So we still need to use a lock to protect the content of the list.
 */
void dlock_list_add(struct dlock_list_node *node,
		    struct dlock_list_head __percpu *head)
{
	struct dlock_list_head *myhead;

	/*
	 * Disable preemption to make sure that CPU won't gets changed.
	 */
	myhead = get_cpu_ptr(head);
	spin_lock(&myhead->lock);
	node->lockptr = &myhead->lock;
	list_add(&node->list, &myhead->list);
	spin_unlock(&myhead->lock);
	put_cpu_ptr(head);
}

/*
 * Delete a node from a dlock list
 *
 * We need to check the lock pointer again after taking the lock to guard
 * against concurrent delete of the same node. If the lock pointer changes
 * (becomes NULL or to a different one), we assume that the deletion was done
 * elsewhere.
 */
void dlock_list_del(struct dlock_list_node *node)
{
	spinlock_t *lock = READ_ONCE(node->lockptr);

	if (unlikely(!lock)) {
		WARN(1, "dlock_list_del: node 0x%lx has no associated lock\n",
			(unsigned long)node);
		return;
	}

	spin_lock(lock);
	if (likely(lock == node->lockptr)) {
		list_del_init(&node->list);
		node->lockptr = NULL;
	} else {
		/*
		 * This path should never be executed.
		 */
		WARN_ON(1);
	}
	spin_unlock(lock);
}
