#ifndef _CGROUP_RDMA_H
#define _CGROUP_RDMA_H

#include <linux/cgroup.h>

#ifdef CONFIG_CGROUP_RDMA

struct rdma_cgroup {
	struct cgroup_subsys_state	css;

	spinlock_t rpool_list_lock;	/* protects resource pool list */
	struct list_head rpool_head;	/* head to keep track of all resource
					 * pools that belongs to this cgroup.
					 */
};

struct rdmacg_pool_info {
	const char **resource_name_table;
	int table_len;
};

struct rdmacg_device {
	struct rdmacg_pool_info pool_info;
	struct list_head	rdmacg_list;
	struct list_head	rpool_head;
	/* protects resource pool list */
	spinlock_t		rpool_lock;
	char			*name;
};

/* APIs for RDMA/IB stack to publish when a device wants to
 * participate in resource accounting
 */
int rdmacg_register_device(struct rdmacg_device *device);
void rdmacg_unregister_device(struct rdmacg_device *device);

/* APIs for RDMA/IB stack to charge/uncharge pool specific resources */
int rdmacg_try_charge(struct rdma_cgroup **rdmacg,
		      struct rdmacg_device *device,
		      int resource_index,
		      int num);
void rdmacg_uncharge(struct rdma_cgroup *cg,
		     struct rdmacg_device *device,
		     int resource_index,
		     int num);
void rdmacg_query_limit(struct rdmacg_device *device,
			int *limits);

#endif	/* CONFIG_CGROUP_RDMA */
#endif	/* _CGROUP_RDMA_H */
