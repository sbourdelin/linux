#ifndef __PERF_NAMESPACES_H
#define __PERF_NAMESPACES_H

#include "../perf.h"
#include <linux/list.h>

struct namespaces_event;

struct namespaces {
	struct list_head list;
	u64 end_time;
	u32 uts_ns_inum;
	u32 ipc_ns_inum;
	u32 mnt_ns_inum;
	u32 pid_ns_inum;
	u32 net_ns_inum;
	u32 cgroup_ns_inum;
	u32 user_ns_inum;
};

struct namespaces *namespaces__new(struct namespaces_event *event);
void namespaces__free(struct namespaces *namespaces);

#endif  /* __PERF_NAMESPACES_H */
