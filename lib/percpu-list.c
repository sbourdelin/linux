/*
 * Per-cpu list
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
#include <linux/percpu-list.h>
#include <linux/lockdep.h>

/*
 * The per-cpu list lock needs its own class to avoid warning and stack
 * trace when lockdep is enabled.
 */
static struct lock_class_key percpu_list_key;

/*
 * Initialize the per-cpu list head
 */
int init_pcpu_list_head(struct pcpu_list_head **ppcpu_head)
{
	struct pcpu_list_head *pcpu_head = alloc_percpu(struct pcpu_list_head);
	int cpu;

	if (!pcpu_head)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct pcpu_list_head *head = per_cpu_ptr(pcpu_head, cpu);

		INIT_LIST_HEAD(&head->list);
		head->lock = __SPIN_LOCK_UNLOCKED(&head->lock);
		lockdep_set_class(&head->lock, &percpu_list_key);
	}

	*ppcpu_head = pcpu_head;
	return 0;
}

/*
 * List selection is based on the CPU being used when the pcpu_list_add()
 * function is called. However, deletion may be done by a different CPU.
 * So we still need to use a lock to protect the content of the list.
 */
void pcpu_list_add(struct pcpu_list_node *node, struct pcpu_list_head *head)
{
	struct pcpu_list_head *myhead;

	/*
	 * Disable preemption to make sure that CPU won't gets changed.
	 */
	preempt_disable();
	myhead = this_cpu_ptr(head);
	spin_lock(&myhead->lock);
	node->lockptr = &myhead->lock;
	list_add(&node->list, &myhead->list);
	spin_unlock(&myhead->lock);
	preempt_enable();
}

/*
 * Delete a node from a percpu list
 *
 * We need to check the lock pointer again after taking the lock to guard
 * against concurrent delete of the same node. If the lock pointer changes
 * (becomes NULL or to a different one), we assume that the deletion was done
 * elsewhere.
 */
void pcpu_list_del(struct pcpu_list_node *node)
{
	spinlock_t *lock = READ_ONCE(node->lockptr);

	if (unlikely(!lock)) {
		WARN(1, "pcpu_list_del: node 0x%lx has no associated lock\n",
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
