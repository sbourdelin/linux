#include <linux/kernel.h>
#include <linux/parser.h>
#include <linux/cgroup_rdma.h>

#include "core_priv.h"

/**
 * resource table definition as to be seen by the user.
 * Need to add entries to it when more resources are
 * added/defined at IB verb/core layer.
 */
static match_table_t resource_tokens = {
	{RDMA_VERB_RESOURCE_UCTX, "uctx=%d"},
	{RDMA_VERB_RESOURCE_AH, "ah=%d"},
	{RDMA_VERB_RESOURCE_PD, "pd=%d"},
	{RDMA_VERB_RESOURCE_CQ, "cq=%d"},
	{RDMA_VERB_RESOURCE_MR, "mr=%d"},
	{RDMA_VERB_RESOURCE_MW, "mw=%d"},
	{RDMA_VERB_RESOURCE_SRQ, "srq=%d"},
	{RDMA_VERB_RESOURCE_QP, "qp=%d"},
	{RDMA_VERB_RESOURCE_FLOW, "flow=%d"},
	{-1, NULL}
};

/**
 * setup table pointers for RDMA cgroup to access.
 */
static struct rdmacg_pool_info verbs_token_info = {
	.resource_table = resource_tokens,
	.resource_count =
		(sizeof(resource_tokens) / sizeof(struct match_token)) - 1,
};

static struct rdmacg_pool_info*
	rdmacg_get_resource_pool_tokens(struct rdmacg_device *device)
{
	return &verbs_token_info;
}

static struct rdmacg_resource_pool_ops verbs_pool_ops = {
	.get_resource_pool_tokens = &rdmacg_get_resource_pool_tokens,
};

/**
 * ib_device_register_rdmacg - register with rdma cgroup.
 * @device: device to register to participate in resource
 *          accounting by rdma cgroup.
 *
 * Register with the rdma cgroup. Should be called before
 * exposing rdma device to user space applications to avoid
 * resource accounting leak.
 * HCA drivers should set resource pool ops first if they wish
 * to support hw specific resource accounting before IB core
 * registers with rdma cgroup.
 */
void ib_device_register_rdmacg(struct ib_device *device)
{
	rdmacg_set_rpool_ops(&device->cg_device,
			     RDMACG_RESOURCE_POOL_VERB,
			     &verbs_pool_ops);
	rdmacg_register_device(&device->cg_device, device->name);
}

/**
 * ib_device_unregister_rdmacg - unregister with rdma cgroup.
 * @device: device to unregister.
 *
 * Unregister with the rdma cgroup. Should be called after
 * all the resources are deallocated, and after a stage when any
 * other resource allocation of user application cannot be done
 * for this device to avoid any leak in accounting.
 * HCA drivers should clear resource pool ops after ib stack
 * unregisters with rdma cgroup.
 */
void ib_device_unregister_rdmacg(struct ib_device *device)
{
	rdmacg_unregister_device(&device->cg_device);
	rdmacg_clear_rpool_ops(&device->cg_device,
			       RDMACG_RESOURCE_POOL_VERB);
}

int ib_rdmacg_try_charge(struct ib_rdmacg_object *cg_obj,
			 struct ib_device *device,
			 enum rdmacg_resource_pool_type type,
			 int resource_index, int num)
{
	return rdmacg_try_charge(&cg_obj->cg, &device->cg_device,
				 type, resource_index, num);
}
EXPORT_SYMBOL(ib_rdmacg_try_charge);

void ib_rdmacg_uncharge(struct ib_rdmacg_object *cg_obj,
			struct ib_device *device,
			enum rdmacg_resource_pool_type type,
			int resource_index, int num)
{
	rdmacg_uncharge(cg_obj->cg, &device->cg_device,
			type, resource_index, num);
}
EXPORT_SYMBOL(ib_rdmacg_uncharge);

int ib_rdmacg_query_limit(struct ib_device *device,
			  enum rdmacg_resource_pool_type type,
			  int *limits, int max_count)
{
	return rdmacg_query_limit(&device->cg_device, type, limits, max_count);
}
EXPORT_SYMBOL(ib_rdmacg_query_limit);
