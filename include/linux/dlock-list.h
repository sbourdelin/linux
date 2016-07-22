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
#ifndef __LINUX_DLOCK_LIST_H
#define __LINUX_DLOCK_LIST_H

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/percpu.h>

/*
 * include/linux/dlock-list.h
 *
 * A distributed (per-cpu) set of lists each of which is protected by its
 * own spinlock, but acts like a single consolidated list to the callers.
 *
 * The dlock_list_head_percpu structure contains the spinlock, the other
 * dlock_list_node structures only contains a pointer to the spinlock in
 * dlock_list_head_percpu.
 */
struct dlock_list_head_percpu {
	struct list_head list;
	spinlock_t lock;
};

struct dlock_list_head {
	struct dlock_list_head_percpu __percpu *head;
};

/*
 * dlock list node data structure
 */
struct dlock_list_node {
	struct list_head list;
	spinlock_t *lockptr;
};

/*
 * dlock list iteration state
 *
 * This is an opaque data structure that may change. Users of this structure
 * should not access the structure members directly other than using the
 * helper functions and macros provided in this header file.
 */
struct dlock_list_iter {
	int cpu;
	struct dlock_list_head *head;
	struct dlock_list_head_percpu *pcpu_head;
};

#define DLOCK_LIST_ITER_INIT(dlist)		\
	{					\
		.cpu = -1,			\
		.head = dlist,			\
	}

#define DEFINE_DLOCK_LIST_ITER(s, dlist)	\
	struct dlock_list_iter s = DLOCK_LIST_ITER_INIT(dlist)

static inline void init_dlock_list_iter(struct dlock_list_iter *iter,
					struct dlock_list_head *head)
{
	*iter = (struct dlock_list_iter)DLOCK_LIST_ITER_INIT(head);
}

#define DLOCK_LIST_NODE_INIT(name)		\
	{					\
		.list = LIST_HEAD_INIT(name)	\
	}

static inline void init_dlock_list_node(struct dlock_list_node *node)
{
	*node = (struct dlock_list_node)DLOCK_LIST_NODE_INIT(node->list);
}

/**
 * dlock_list_empty - Check if all the dlock lists are empty
 * @dlist: Pointer to the dlock_list_head structure
 * Return: true if list is empty, false otherwise.
 *
 * This can be a pretty expensive function call. If this function is required
 * in a performance critical path, we may have to maintain a global count
 * of the list entries in the global dlock_list_head structure instead.
 */
static inline bool dlock_list_empty(struct dlock_list_head *dlist)
{
	int cpu;

	for_each_possible_cpu(cpu)
		if (!list_empty(&per_cpu_ptr(dlist->head, cpu)->list))
			return false;
	return true;
}

/**
 * dlock_list_unlock - unlock the spinlock that protects the percpu list
 * @iter: Pointer to the dlock list iterator structure
 */
static inline void dlock_list_unlock(struct dlock_list_iter *iter)
{
	spin_unlock(&iter->pcpu_head->lock);
}

/**
 * dlock_list_relock - lock the spinlock that protects the percpu list
 * @iter: Pointer to the dlock list iterator structure
 */
static inline void dlock_list_relock(struct dlock_list_iter *iter)
{
	spin_lock(&iter->pcpu_head->lock);
}

/*
 * Allocation and freeing of dlock list
 */
extern int  alloc_dlock_list_head(struct dlock_list_head *dlist);
extern void free_dlock_list_head(struct dlock_list_head *dlist);

/*
 * The dlock list addition and deletion functions here are not irq-safe.
 * Special irq-safe variants will have to be added if we need them.
 */
extern void dlock_list_add(struct dlock_list_node *node,
			   struct dlock_list_head *dlist);
extern void dlock_list_del(struct dlock_list_node *node);

/*
 * Find the first entry of the next per-cpu list.
 */
extern struct dlock_list_node *
__dlock_list_next_cpu(struct dlock_list_iter *iter);

/**
 * __dlock_list_next_entry - Iterate to the next entry of the dlock list
 * @curr : Pointer to the current dlock_list_node structure
 * @iter : Pointer to the dlock list iterator structure
 * Return: Pointer to the next entry or NULL if all the entries are iterated
 *
 * The iterator has to be properly initialized before calling this function.
 */
static inline struct dlock_list_node *
__dlock_list_next_entry(struct dlock_list_node *curr,
			struct dlock_list_iter *iter)
{
	/*
	 * Find next entry
	 */
	if (curr)
		curr = list_next_entry(curr, list);

	if (!curr || (&curr->list == &iter->pcpu_head->list)) {
		/*
		 * The current per-cpu list has been exhausted, try the next
		 * per-cpu list.
		 */
		curr = __dlock_list_next_cpu(iter);
	}

	return curr;	/* Continue the iteration */
}

/**
 * dlock_list_first_entry - get the first element from a list
 * @iter  : The dlock list iterator.
 * @type  : The type of the struct this is embedded in.
 * @member: The name of the dlock_list_node within the struct.
 * Return : Pointer to the next entry or NULL if all the entries are iterated.
 */
#define dlock_list_first_entry(iter, type, member)			\
	({								\
		struct dlock_list_node *_n;				\
		_n = __dlock_list_next_entry(NULL, iter);		\
		_n ? list_entry(_n, type, member) : NULL;		\
	})

/**
 * dlock_list_next_entry - iterate to the next entry of the list
 * @pos   : The type * to cursor
 * @iter  : The dlock list iterator.
 * @member: The name of the dlock_list_node within the struct.
 * Return : Pointer to the next entry or NULL if all the entries are iterated.
 *
 * Note that pos can't be NULL.
 */
#define dlock_list_next_entry(pos, iter, member)			\
	({								\
		struct dlock_list_node *_n;				\
		_n = __dlock_list_next_entry(&(pos)->member, iter);	\
		_n ? list_entry(_n, typeof(*(pos)), member) : NULL;	\
	})

/**
 * dlist_for_each_entry - iterate over the dlock list
 * @pos   : Type * to use as a loop cursor
 * @iter  : The dlock list iterator
 * @member: The name of the dlock_list_node within the struct
 *
 * This iteration macro isn't safe with respect to list entry removal, but
 * it can correctly iterate newly added entries right after the current one.
 * This iteration function is designed to be used in a while loop.
 */
#define dlist_for_each_entry(pos, iter, member)				\
	for (pos = dlock_list_first_entry(iter, typeof(*(pos)), member);\
	     pos != NULL;						\
	     pos = dlock_list_next_entry(pos, iter, member))

/**
 * dlist_for_each_entry_safe - iterate over the dlock list & safe over removal
 * @pos   : Type * to use as a loop cursor
 * @n	  : Another type * to use as temporary storage
 * @iter  : The dlock list iterator
 * @member: The name of the dlock_list_node within the struct
 *
 * This iteration macro is safe with respect to list entry removal.
 * However, it cannot correctly iterate newly added entries right after the
 * current one.
 */
#define dlist_for_each_entry_safe(pos, n, iter, member)			\
	for (pos = dlock_list_first_entry(iter, typeof(*(pos)), member);\
	    ({								\
		bool _b = (pos != NULL);				\
		if (_b)							\
			n = dlock_list_next_entry(pos, iter, member);	\
		_b;							\
	    });								\
	    pos = n)

#endif /* __LINUX_DLOCK_LIST_H */
