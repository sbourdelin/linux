/*
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/cgroup_rdma.h>
#include <linux/parser.h>

#include "core_priv.h"

/**
 * resource table definition as to be seen by the user.
 * Need to add entries to it when more resources are
 * added/defined at IB verb/core layer.
 */
static char const *resource_tokens[] = {
	[RDMA_VERB_RESOURCE_UCTX] = "uctx",
	[RDMA_VERB_RESOURCE_AH] = "ah",
	[RDMA_VERB_RESOURCE_PD] = "pd",
	[RDMA_VERB_RESOURCE_CQ] = "cq",
	[RDMA_VERB_RESOURCE_MR] = "mr",
	[RDMA_VERB_RESOURCE_MW] = "mw",
	[RDMA_VERB_RESOURCE_SRQ] = "srq",
	[RDMA_VERB_RESOURCE_QP] = "qp",
	[RDMA_VERB_RESOURCE_FLOW] = "flow",
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
 * Returns 0 on success or otherwise failure code.
 */
int ib_device_register_rdmacg(struct ib_device *device)
{
	device->cg_device.name = device->name;
	device->cg_device.pool_info.resource_name_table = resource_tokens;
	device->cg_device.pool_info.table_len = ARRAY_SIZE(resource_tokens);
	return rdmacg_register_device(&device->cg_device);
}

/**
 * ib_device_unregister_rdmacg - unregister with rdma cgroup.
 * @device: device to unregister.
 *
 * Unregister with the rdma cgroup. Should be called after
 * all the resources are deallocated, and after a stage when any
 * other resource allocation of user application cannot be done
 * for this device to avoid any leak in accounting.
 */
void ib_device_unregister_rdmacg(struct ib_device *device)
{
	rdmacg_unregister_device(&device->cg_device);
}

int ib_rdmacg_try_charge(struct ib_rdmacg_object *cg_obj,
			 struct ib_device *device,
			 int resource_index, int num)
{
	return rdmacg_try_charge(&cg_obj->cg, &device->cg_device,
				 resource_index, num);
}
EXPORT_SYMBOL(ib_rdmacg_try_charge);

void ib_rdmacg_uncharge(struct ib_rdmacg_object *cg_obj,
			struct ib_device *device,
			int resource_index, int num)
{
	rdmacg_uncharge(cg_obj->cg, &device->cg_device,
			resource_index, num);
}
EXPORT_SYMBOL(ib_rdmacg_uncharge);

void ib_rdmacg_query_limit(struct ib_device *device, int *limits)
{
	rdmacg_query_limit(&device->cg_device, limits);
}
EXPORT_SYMBOL(ib_rdmacg_query_limit);
