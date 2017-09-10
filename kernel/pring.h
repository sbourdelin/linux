#ifndef _LINUX_PRING_H_
#define _LINUX_PRING_H_

/*
 * futex specific priority-sorted ring
 *
 * based on include/linux/plist.h
 *
 * Simple ASCII art explanation:
 *
 * fl:futx_list
 * pl:prio_list
 * nl:node_list
 *
 * +------+
 * |      v
 * |     |fl|        HEAD
 * |      ^
 * |      |
 * |      v
 * |  +--------+
 * |  +->|pl|<-+
 * |     |10|   (prio)
 * |     |  |
 * |  +->|nl|<-+
 * |  +--------+
 * |      ^
 * |      |
 * |      v
 * |  +------------------------------------+
 * |  +->|pl|<->|pl|<--------------->|pl|<-+
 * |     |10|   |21|   |21|   |21|   |40|   (prio)
 * |     |  |   |  |   |  |   |  |   |  |
 * |  +->|nl|<->|nl|<->|nl|<->|nl|<->|nl|<-+
 * |  +------------------------------------+
 * |      ^
 * +------+
 */

#include <linux/kernel.h>
#include <linux/list.h>

struct pring_node {
	int			prio;
	struct list_head	prio_list;
	struct list_head	node_list;
};

static void pring_init(struct pring_node *node, int prio)
{
	node->prio = prio;
	INIT_LIST_HEAD(&node->prio_list);
	INIT_LIST_HEAD(&node->node_list);
}

static inline bool pring_is_singular(struct pring_node *node)
{
	return list_empty(&node->node_list);
}

static void pring_del(struct pring_node *node)
{
	if (WARN_ON(list_empty(&node->node_list)))
		return;
	if (!list_empty(&node->prio_list)) {
		struct pring_node *next;

		next = list_next_entry(node, node_list);
		/* add the next node into prio_list */
		if (list_empty(&next->prio_list))
			list_add(&next->prio_list, &node->prio_list);
		list_del_init(&node->prio_list);
	}
	list_del_init(&node->node_list);
}

static struct pring_node *
pring_add(struct pring_node *node, struct pring_node *top)
{
	struct pring_node *iter, *prev = NULL;
	struct list_head *node_next = &top->node_list;

	WARN_ON(!list_empty(&node->node_list));
	WARN_ON(!list_empty(&node->prio_list));

	iter = top;

	do {
		if (node->prio < iter->prio) {
			node_next = &iter->node_list;
			break;
		}

		prev = iter;
		iter = list_entry(iter->prio_list.next,
				struct pring_node, prio_list);
	} while (iter != top);

	if (!prev || prev->prio != node->prio)
		list_add_tail(&node->prio_list, &iter->prio_list);
	list_add_tail(&node->node_list, node_next);

	return prev ? top : node;
}

#define pring_next_entry(pos, member) \
	container_of(list_next_entry(&(pos)->member, node_list), \
			typeof(*(pos)), member)

#endif
