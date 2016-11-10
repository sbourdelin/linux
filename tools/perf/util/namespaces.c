#include "namespaces.h"
#include "util.h"
#include "event.h"
#include <stdlib.h>
#include <stdio.h>
#include <linux/atomic.h>

struct namespaces *namespaces__new(struct namespaces_event *event)
{
	struct namespaces *namespaces = zalloc(sizeof(*namespaces));

	if (!namespaces)
		return NULL;

	namespaces->end_time = -1;

	if (event) {
		namespaces->uts_ns_inum = event->uts_ns_inum;
		namespaces->ipc_ns_inum = event->ipc_ns_inum;
		namespaces->mnt_ns_inum = event->mnt_ns_inum;
		namespaces->pid_ns_inum = event->pid_ns_inum;
		namespaces->net_ns_inum = event->net_ns_inum;
		namespaces->cgroup_ns_inum = event->cgroup_ns_inum;
		namespaces->user_ns_inum = event->user_ns_inum;
	}

	return namespaces;
}

void namespaces__free(struct namespaces *namespaces)
{
	free(namespaces);
}
