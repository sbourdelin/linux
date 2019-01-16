// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2017-2018 Mellanox Technologies. All rights reserved.
 */

#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/restrack.h>
#include <linux/mutex.h>
#include <linux/sched/task.h>
#include <linux/pid_namespace.h>
#include <linux/rwsem.h>

#include "cma_priv.h"

/**
 * FIXME: Replace with official xa_array_cyclic(), once it is merged
 * http://git.infradead.org/users/willy/linux-dax.git/commit/8bcca93a53949d20877e82c2aaae0fd7c385b90d
 */
struct rt_xa_limit {
	u32 min;
	u32 max;
};
static int rt_xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
			      const struct rt_xa_limit *limit, u32 *next,
			      gfp_t gfp)
{
	int err;

	if (*next < limit->max)
		*id = *next;
	else
		*id = limit->min;

	xa_lock(xa);
	err = __xa_alloc(xa, id, limit->max, entry, gfp);

	if (err && *next > limit->min) {
		*id = limit->min;
		err = __xa_alloc(xa, id, limit->max, entry, gfp);
	}

	if (!err)
		*next = *id + 1;
	xa_unlock(xa);
	return err;
}

/**
 * struct rdma_restrack_root - main resource tracking management
 * entity, per-device
 */
struct rdma_restrack_root {
	/*
	 * @rwsem: Read/write lock to protect erase of entry.
	 * Lists and insertions are protected by XArray internal lock.
	 */
	struct rw_semaphore	rwsem;
	/**
	 * @xa: Array of XArray structure to hold restrack entries.
	 */
	struct xarray xa;
	/**
	 * @range: Allocation ID range between min and max
	 */
	struct rt_xa_limit range;
	/**
	 * @next_id: Next ID to support cyclic allocation
	 */
	u32 next_id;
};

/**
 * rdma_restrack_init() - initialize and allocate resource tracking
 * @dev:  IB device
 *
 * Return: 0 on success
 */
int rdma_restrack_init(struct ib_device *dev)
{
	struct rdma_restrack_root *rt;
	int i;

	dev->res = kcalloc(RDMA_RESTRACK_MAX, sizeof(*rt), GFP_KERNEL);
	if (!dev->res)
		return -ENOMEM;

	rt = dev->res;

	for (i = 0 ; i < RDMA_RESTRACK_MAX; i++) {
		init_rwsem(&rt[i].rwsem);
		xa_init_flags(&rt[i].xa, XA_FLAGS_ALLOC);
		rt[i].range.max = U32_MAX;
	}

	return 0;
}

static const char *type2str(enum rdma_restrack_type type)
{
	static const char * const names[RDMA_RESTRACK_MAX] = {
		[RDMA_RESTRACK_PD] = "PD",
		[RDMA_RESTRACK_CQ] = "CQ",
		[RDMA_RESTRACK_QP] = "QP",
		[RDMA_RESTRACK_CM_ID] = "CM_ID",
		[RDMA_RESTRACK_MR] = "MR",
		[RDMA_RESTRACK_CTX] = "CTX",
	};

	return names[type];
};

/**
 * rdma_dev_to_xa() - translate from device to XArray DB
 * @dev: IB device to work
 * @type: resource track type
 *
 * Return: XArray DB to use for xa_for_each() iterations
 */
struct xarray *rdma_dev_to_xa(struct ib_device *dev,
			      enum rdma_restrack_type type)
{
	return &dev->res[type].xa;

}
EXPORT_SYMBOL(rdma_dev_to_xa);

/**
 * rdma_rt_read_lock() - Lock XArray for read, needed while iterating
 *                       with xa_for_each()
 * @dev: IB device to work
 * @type: resource track type
 */
void rdma_rt_read_lock(struct ib_device *dev, enum rdma_restrack_type type)
{
	down_read(&dev->res[type].rwsem);
}
EXPORT_SYMBOL(rdma_rt_read_lock);

/**
 * rdma_rt_read_unlock() - Unlock XArray for read, needed while iterating
 *                         with xa_for_each()
 * @dev: IB device to work
 * @type: resource track type
 */
void rdma_rt_read_unlock(struct ib_device *dev, enum rdma_restrack_type type)
{
	up_read(&dev->res[type].rwsem);
}
EXPORT_SYMBOL(rdma_rt_read_unlock);

/**
 * rdma_restrack_clean() - clean resource tracking
 * @dev:  IB device
 */
void rdma_restrack_clean(struct ib_device *dev)
{
	struct rdma_restrack_root *rt = dev->res;
	struct rdma_restrack_entry *e;
	char buf[TASK_COMM_LEN];
	bool found = false;
	const char *owner;
	int i;

	for (i = 0 ; i < RDMA_RESTRACK_MAX; i++) {
		struct xarray *xa = rdma_dev_to_xa(dev, i);

		if (!xa_empty(xa)) {
			unsigned long index = 0;

			if (!found) {
				pr_err("restrack: %s", CUT_HERE);
				dev_err(&dev->dev, "BUG: RESTRACK detected leak of resources\n");
			}
			xa_for_each(xa, e, index, ULONG_MAX, XA_PRESENT) {
				if (rdma_is_kernel_res(e)) {
					owner = e->kern_name;
				} else {
					/*
					 * There is no need to call get_task_struct here,
					 * because we can be here only if there are more
					 * get_task_struct() call than put_task_struct().
					 */
					get_task_comm(buf, e->task);
					owner = buf;
				}

				pr_err("restrack: %s %s object allocated by %s is not freed\n",
				       rdma_is_kernel_res(e) ? "Kernel" :
							       "User",
				       type2str(e->type), owner);
			}
			found = true;
		}
		xa_destroy(xa);
	}
	if (found)
		pr_err("restrack: %s", CUT_HERE);

	kfree(rt);
}

/**
 * rdma_restrack_count() - the current usage of specific object
 * @dev:  IB device
 * @type: actual type of object to operate
 * @ns:   PID namespace
 */
int rdma_restrack_count(struct ib_device *dev, enum rdma_restrack_type type,
			struct pid_namespace *ns)
{
	struct xarray *xa = rdma_dev_to_xa(dev, type);
	struct rdma_restrack_entry *e;
	unsigned long index = 0;
	u32 cnt = 0;

	rdma_rt_read_lock(dev, type);
	xa_for_each(xa, e, index, ULONG_MAX, RES_VISIBLE) {
		if (ns == &init_pid_ns ||
		    (!rdma_is_kernel_res(e) &&
		     ns == task_active_pid_ns(e->task)))
			cnt++;
	}
	rdma_rt_read_unlock(dev, type);
	return cnt;
}
EXPORT_SYMBOL(rdma_restrack_count);

static void set_kern_name(struct rdma_restrack_entry *res)
{
	struct ib_pd *pd;

	switch (res->type) {
	case RDMA_RESTRACK_QP:
		pd = container_of(res, struct ib_qp, res)->pd;
		if (!pd) {
			WARN_ONCE(true, "XRC QPs are not supported\n");
			/* Survive, despite the programmer's error */
			res->kern_name = " ";
		}
		break;
	case RDMA_RESTRACK_MR:
		pd = container_of(res, struct ib_mr, res)->pd;
		break;
	default:
		/* Other types set kern_name directly */
		pd = NULL;
		break;
	}

	if (pd)
		res->kern_name = pd->res.kern_name;
}

static struct ib_device *res_to_dev(struct rdma_restrack_entry *res)
{
	switch (res->type) {
	case RDMA_RESTRACK_PD:
		return container_of(res, struct ib_pd, res)->device;
	case RDMA_RESTRACK_CQ:
		return container_of(res, struct ib_cq, res)->device;
	case RDMA_RESTRACK_QP:
		return container_of(res, struct ib_qp, res)->device;
	case RDMA_RESTRACK_CM_ID:
		return container_of(res, struct rdma_id_private,
				    res)->id.device;
	case RDMA_RESTRACK_MR:
		return container_of(res, struct ib_mr, res)->device;
	case RDMA_RESTRACK_CTX:
		return container_of(res, struct ib_ucontext, res)->device;
	default:
		WARN_ONCE(true, "Wrong resource tracking type %u\n", res->type);
		return NULL;
	}
}

void rdma_restrack_set_task(struct rdma_restrack_entry *res,
			    const char *caller)
{
	if (caller) {
		res->kern_name = caller;
		return;
	}

	if (res->task)
		put_task_struct(res->task);
	get_task_struct(current);
	res->task = current;
}
EXPORT_SYMBOL(rdma_restrack_set_task);

/**
 * rdma_restrack_add() - add new resoruce to DB and get ID in return
 * @res: resoruce to add
 *
 * Return: 0 on success
 */
int rdma_restrack_add(struct rdma_restrack_entry *res)
{
	struct ib_device *dev = res_to_dev(res);
	struct xarray *xa = rdma_dev_to_xa(dev, res->type);
	struct rdma_restrack_root *rt = dev->res;
	int ret;

	/*
	 * Once all drivers are converted, we can remove this check
	 * and remove call to rdma_restrack_add() from rdma_restrack_kadd()
	 * and rdma_restrack_uadd()
	 */
	if (xa_load(xa, res->id))
		return 0;

	kref_init(&res->kref);
	init_completion(&res->comp);
	res->valid = true;

	if (rt[res->type].range.max) {
		/* Not HW-capable device */
		ret = rt_xa_alloc_cyclic(xa, &res->id, res,
					 &rt[res->type].range,
					 &rt[res->type].next_id, GFP_KERNEL);
	} else {
		ret = xa_insert(xa, res->id, res, GFP_KERNEL);
	}

	/*
	 * WARNs below indicates an error in driver code.
	 * The check of -EEXIST is never occuried and added here
	 * to allow simple removal of xa_load above.
	 */
	WARN_ONCE(ret == -EEXIST, "Tried to add non-unique %s entry %u\n",
		  type2str(res->type), res->id);
	WARN_ONCE(ret == -ENOSPC,
		  "There are no more free indexes for type %s entry %u\n",
		  type2str(res->type), res->id);

	if (ret)
		res->valid = false;

	return ret;
}
EXPORT_SYMBOL(rdma_restrack_add);

/**
 * rdma_restrack_kadd() - add kernel object to the reource tracking database
 * @res:  resource entry
 */
void rdma_restrack_kadd(struct rdma_restrack_entry *res)
{
	struct ib_device *dev = res_to_dev(res);
	struct xarray *xa = rdma_dev_to_xa(dev, res->type);

	res->task = NULL;
	set_kern_name(res);
	res->user = false;
	/*
	 * Temporaly, we are not intested in return value,
	 * once conversion will be finished, it will be checked by drivers.
	 */
	if (rdma_restrack_add(res))
		return;

	xa_set_mark(xa, res->id, RES_VISIBLE);
}
EXPORT_SYMBOL(rdma_restrack_kadd);

/**
 * rdma_restrack_uadd() - add user object to the reource tracking database
 * @res:  resource entry
 */
void rdma_restrack_uadd(struct rdma_restrack_entry *res)
{
	struct ib_device *dev = res_to_dev(res);
	struct xarray *xa = rdma_dev_to_xa(dev, res->type);

	if (res->type != RDMA_RESTRACK_CM_ID)
		res->task = NULL;

	if (!res->task)
		rdma_restrack_set_task(res, NULL);
	res->kern_name = NULL;

	res->user = true;
	/*
	 * Temporaly, we are not intested in return value,
	 * once conversion will be finished, it will be checked by drivers.
	 */
	if (rdma_restrack_add(res))
		return;

	xa_set_mark(xa, res->id, RES_VISIBLE);
}
EXPORT_SYMBOL(rdma_restrack_uadd);

int __must_check rdma_restrack_get(struct rdma_restrack_entry *res)
{
	return kref_get_unless_zero(&res->kref);
}
EXPORT_SYMBOL(rdma_restrack_get);

/**
 * rdma_restrack_get_byid() - translate from ID to restrack object
 * @dev: IB device
 * @type: resource track type
 * @id: ID to take a look
 *
 * Return: Pointer to restrack entry or -ENOENT in case of error.
 */
struct rdma_restrack_entry *
rdma_restrack_get_byid(struct ib_device *dev,
		       enum rdma_restrack_type type, u32 id)
{
	struct xarray *xa = rdma_dev_to_xa(dev, type);
	struct rdma_restrack_entry *res;

	res = xa_load(xa, id);
	if (!res || !rdma_restrack_get(res) ||
	    !xa_get_mark(xa, res->id, RES_VISIBLE))
		return ERR_PTR(-ENOENT);
	return res;
}
EXPORT_SYMBOL(rdma_restrack_get_byid);

static void restrack_release(struct kref *kref)
{
	struct rdma_restrack_entry *res;

	res = container_of(kref, struct rdma_restrack_entry, kref);
	complete(&res->comp);
}

int rdma_restrack_put(struct rdma_restrack_entry *res)
{
	return kref_put(&res->kref, restrack_release);
}
EXPORT_SYMBOL(rdma_restrack_put);

void rdma_restrack_del(struct rdma_restrack_entry *res)
{
	struct ib_device *dev = res_to_dev(res);
	struct xarray *xa;

	if (!res->valid)
		goto out;

	/*
	 * All objects except CM_ID set valid device immediately
	 * after new object is created, it means that for not valid
	 * objects will still have "dev".
	 *
	 * It is not the case for CM_ID, newly created object has
	 * this field set to NULL and it is set in _cma_attach_to_dev()
	 * only.
	 *
	 * Because we don't want to add any conditions on call
	 * to rdma_restrack_del(), the check below protects from
	 * NULL-dereference.
	 */
	if (!dev)
		return;

	xa = rdma_dev_to_xa(dev, res->type);
	if (!xa_load(xa, res->id))
		goto out;

	rdma_restrack_put(res);

	wait_for_completion(&res->comp);

	down_write(&dev->res[res->type].rwsem);
	xa_erase(xa, res->id);
	res->valid = false;
	up_write(&dev->res[res->type].rwsem);

out:
	if (res->task) {
		put_task_struct(res->task);
		res->task = NULL;
	}
}
EXPORT_SYMBOL(rdma_restrack_del);

/**
 * rdma_rt_set_id_range() - initialize IDs range. Devices which are able
 *                          to manage thos IDs in HW will leave max and reserved
 *                          as zero.
 * @dev: IB device to work
 * @type: resource track type
 * @reserved: number of entries to keep aside
 * @max: maximal possible value
 */
void rdma_rt_set_id_range(struct ib_device *dev, enum rdma_restrack_type type,
			  u32 reserved, u32 max)
{
	struct rdma_restrack_root *rt = dev->res;

	rt[type].range.max = max;
	rt[type].range.min = reserved;
	rt[type].next_id = reserved;
}
EXPORT_SYMBOL(rdma_rt_set_id_range);

/**
 * rdma_rt_set_id() - Set unique ID as seen by restrack,
 *                    needed for HW-capable device
 * @res: resrouce to operate
 * @id: ID to set
 *
 * Return: 0 on success
 */
int rdma_rt_set_id(struct rdma_restrack_entry *res, u32 id)
{
	struct ib_device *dev = res_to_dev(res);
	struct xarray *xa = rdma_dev_to_xa(dev, res->type);

	if (xa_load(xa, id) == res) {
		WARN_ONCE(true, "Tried to add non-unique %s entry %u\n",
			  type2str(res->type), res->id);
		return -EEXIST;
	}

	res->id = id;
	return 0;
}
EXPORT_SYMBOL(rdma_rt_set_id);
