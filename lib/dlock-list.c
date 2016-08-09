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
#include <linux/slab.h>
#include <linux/cpumask.h>

/*
 * As all the locks in the dlock list are dynamically allocated, they need
 * to belong to their own special lock class to avoid warning and stack
 * trace in kernel log when lockdep is enabled. Statically allocated locks
 * don't have this problem.
 */
static struct lock_class_key dlock_list_key;

/*
 * Mapping CPU number to dlock list index.
 */
static DEFINE_PER_CPU_READ_MOSTLY(int, cpu2list);
static int nr_dlists;

/*
 * Initialize cpu2list mapping table & nr_dlists;
 *
 * All the sibling CPUs of a sibling group will map to the same dlock list so
 * as to reduce the number of dlock lists to be maintained while minimizing
 * cacheline contention.
 *
 * As the sibling masks are set up in the core initcall phase, this function
 * has to be done in the postcore phase to get the right data. An alloc can
 * be called before init. In this case, we just do a simple 1-1 mapping
 * between CPU and dlock list head. After init, multiple CPUs may map to the
 * same dlock list head.
 */
static int __init cpu2list_init(void)
{
	int idx, cpu;
	int nr_siblings;
	cpumask_var_t sibling_mask;
	static struct cpumask mask __initdata;

	/*
	 * Check # of sibling CPUs for CPU 0
	 */
	sibling_mask = topology_sibling_cpumask(0);
	if (!sibling_mask)
		goto done;
	nr_siblings = cpumask_weight(sibling_mask);
	if (nr_siblings == 1)
		goto done;

	cpumask_setall(&mask);
	idx = 0;
	for_each_possible_cpu(cpu) {
		int scpu;

		if (!cpumask_test_cpu(cpu, &mask))
			continue;
		sibling_mask = topology_sibling_cpumask(cpu);
		for_each_cpu(scpu, sibling_mask) {
			per_cpu(cpu2list, scpu) = idx;
			cpumask_clear_cpu(scpu, &mask);
		}
		idx++;
	}

	/*
	 * nr_dlists can only be set after cpu2list_map is properly initialized.
	 */
	smp_mb();
	nr_dlists = nr_cpu_ids/nr_siblings;

	WARN_ON(cpumask_weight(&mask) != 0);
	WARN_ON(idx > nr_dlists);
	pr_info("dlock-list: %d head entries per dlock list.\n", nr_dlists);
done:
	return 0;
}
postcore_initcall(cpu2list_init);

/**
 * alloc_dlock_list_heads - Initialize and allocate the list of head entries
 * @dlist: Pointer to the dlock_list_heads structure to be initialized
 * Return: 0 if successful, -ENOMEM if memory allocation error
 *
 * This function does not allocate the dlock_list_heads structure itself. The
 * callers will have to do their own memory allocation, if necessary. However,
 * this allows embedding the dlock_list_heads structure directly into other
 * structures.
 */
int alloc_dlock_list_heads(struct dlock_list_heads *dlist)
{
	int idx;

	dlist->nhead = nr_dlists ? nr_dlists : nr_cpu_ids;
	dlist->heads = kcalloc(dlist->nhead, sizeof(struct dlock_list_head),
			       GFP_KERNEL);

	if (!dlist->heads)
		return -ENOMEM;

	for (idx = 0; idx < dlist->nhead; idx++) {
		struct dlock_list_head *head = &dlist->heads[idx];

		INIT_LIST_HEAD(&head->list);
		head->lock = __SPIN_LOCK_UNLOCKED(&head->lock);
		lockdep_set_class(&head->lock, &dlock_list_key);
	}
	return 0;
}

/**
 * free_dlock_list_heads - Free all the heads entries of the dlock list
 * @dlist: Pointer of the dlock_list_heads structure to be freed
 *
 * This function doesn't free the dlock_list_heads structure itself. So
 * the caller will have to do it, if necessary.
 */
void free_dlock_list_heads(struct dlock_list_heads *dlist)
{
	kfree(dlist->heads);
	dlist->heads = NULL;
	dlist->nhead = 0;
}

/**
 * dlock_list_empty - Check if all the dlock lists are empty
 * @dlist: Pointer to the dlock_list_heads structure
 * Return: true if list is empty, false otherwise.
 *
 * This can be a pretty expensive function call. If this function is required
 * in a performance critical path, we may have to maintain a global count
 * of the list entries in the global dlock_list_heads structure instead.
 */
bool dlock_list_empty(struct dlock_list_heads *dlist)
{
	int idx;

	for (idx = 0; idx < dlist->nhead; idx++)
		if (!list_empty(&dlist->heads[idx].list))
			return false;
	return true;
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
		    struct dlock_list_heads *dlist)
{
	int cpu = smp_processor_id();
	int idx = (dlist->nhead < nr_cpu_ids) ? per_cpu(cpu2list, cpu) : cpu;
	struct dlock_list_head *head = &dlist->heads[idx];

	/*
	 * There is no need to disable preemption
	 */
	spin_lock(&head->lock);
	node->head = head;
	list_add(&node->list, &head->list);
	spin_unlock(&head->lock);
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
	struct dlock_list_head *head;
	bool retry;

	do {
		head = READ_ONCE(node->head);
		if (WARN_ONCE(!head,
			"dlock_list_del: node 0x%lx has no associated head\n",
			(unsigned long)node))
			return;

		spin_lock(&head->lock);
		if (likely(head == node->head)) {
			list_del_init(&node->list);
			node->head = NULL;
			retry = false;
		} else {
			/*
			 * The lock has somehow changed. Retry again if it is
			 * not NULL. Otherwise, just ignore the delete
			 * operation.
			 */
			retry = (node->head != NULL);
		}
		spin_unlock(&head->lock);
	} while (retry);
}

/**
 * __dlock_list_next_list: Find the first entry of the next available list
 * @dlist: Pointer to the dlock_list_heads structure
 * @iter : Pointer to the dlock list iterator structure
 * Return: true if the entry is found, false if all the lists exhausted
 *
 * The information about the next available list will be put into the iterator.
 */
struct dlock_list_node *__dlock_list_next_list(struct dlock_list_iter *iter)
{
	struct dlock_list_node *next;
	struct dlock_list_head *head;

restart:
	if (iter->entry) {
		spin_unlock(&iter->entry->lock);
		iter->entry = NULL;
	}

next_list:
	/*
	 * Try next list
	 */
	if (++iter->index >= iter->nhead)
		return NULL;	/* All the entries iterated */

	if (list_empty(&iter->head[iter->index].list))
		goto next_list;

	head = iter->entry = &iter->head[iter->index];
	spin_lock(&head->lock);
	/*
	 * There is a slight chance that the list may become empty just
	 * before the lock is acquired. So an additional check is
	 * needed to make sure that a valid node will be returned.
	 */
	if (list_empty(&head->list))
		goto restart;

	next = list_entry(head->list.next, struct dlock_list_node,
			  list);
	WARN_ON_ONCE(next->head != head);

	return next;
}
