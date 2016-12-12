#ifndef __PERF_NAMESPACES_H
#define __PERF_NAMESPACES_H

#include "../perf.h"
#include <linux/list.h>

struct namespaces_event;

struct namespaces {
	struct list_head list;
	u64 end_time;
	u64 dev_num;
	u64 inode_num[NAMESPACES_MAX];
};

struct namespaces *namespaces__new(struct namespaces_event *event);
void namespaces__free(struct namespaces *namespaces);

#endif  /* __PERF_NAMESPACES_H */
