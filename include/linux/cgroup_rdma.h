#ifndef _CGROUP_RDMA_H
#define _CGROUP_RDMA_H

#include <linux/cgroup.h>

/*
 * This file is subject to the terms and conditions of version 2 of the GNU
 * General Public License.  See the file COPYING in the main directory of the
 * Linux distribution for more details.
 */

enum rdmacg_resource_pool_type {
	RDMACG_RESOURCE_POOL_VERB,
	RDMACG_RESOURCE_POOL_HW,
	RDMACG_RESOURCE_POOL_TYPE_MAX,
};

struct rdma_cgroup {
	struct cgroup_subsys_state	css;

	spinlock_t	cg_list_lock;	/* protects cgroup resource pool list */
	struct list_head rpool_head;	/* head to keep track of all resource
					 * pools that belongs to this cgroup.
					 */
};

#ifdef CONFIG_CGROUP_RDMA
#define RDMACG_MAX_RESOURCE_INDEX (64)

struct match_token;
struct rdmacg_device;

struct rdmacg_pool_info {
	struct match_token *resource_table;
	int resource_count;
};

struct rdmacg_resource_pool_ops {
	struct rdmacg_pool_info*
		(*get_resource_pool_tokens)(struct rdmacg_device *);
};

struct rdmacg_device {
	struct rdmacg_resource_pool_ops
				*rpool_ops[RDMACG_RESOURCE_POOL_TYPE_MAX];
	struct list_head        rdmacg_list;
	char                    *name;
};

/* APIs for RDMA/IB stack to publish when a device wants to
 * participate in resource accounting
 */
void rdmacg_register_device(struct rdmacg_device *device, char *dev_name);
void rdmacg_unregister_device(struct rdmacg_device *device);

/* APIs for RDMA/IB stack to charge/uncharge pool specific resources */
int rdmacg_try_charge(struct rdma_cgroup **rdmacg,
		      struct rdmacg_device *device,
		      enum rdmacg_resource_pool_type type,
		      int resource_index,
		      int num);
void rdmacg_uncharge(struct rdma_cgroup *cg,
		     struct rdmacg_device *device,
		     enum rdmacg_resource_pool_type type,
		     int resource_index,
		     int num);

void rdmacg_set_rpool_ops(struct rdmacg_device *device,
			  enum rdmacg_resource_pool_type pool_type,
			  struct rdmacg_resource_pool_ops *ops);
void rdmacg_clear_rpool_ops(struct rdmacg_device *device,
			    enum rdmacg_resource_pool_type pool_type);
int rdmacg_query_limit(struct rdmacg_device *device,
		       enum rdmacg_resource_pool_type type,
		       int *limits, int max_count);

#endif	/* CONFIG_CGROUP_RDMA */
#endif	/* _CGROUP_RDMA_H */
