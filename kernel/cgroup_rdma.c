/*
 * This file is subject to the terms and conditions of version 2 of the GNU
 * General Public License.  See the file COPYING in the main directory of the
 * Linux distribution for more details.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/threads.h>
#include <linux/pid.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/seq_file.h>
#include <linux/hashtable.h>
#include <linux/cgroup.h>
#include <linux/cgroup_rdma.h>

static DEFINE_MUTEX(dev_mutex);
static LIST_HEAD(dev_list);

enum rdmacg_file_type {
	RDMACG_VERB_RESOURCE_MAX,
	RDMACG_VERB_RESOURCE_STAT,
	RDMACG_HW_RESOURCE_MAX,
	RDMACG_HW_RESOURCE_STAT,
};

#define RDMACG_USR_CMD_REMOVE "remove"

/* resource tracker per resource for rdma cgroup */
struct rdmacg_resource {
	int max;
	atomic_t usage;
};

/**
 * pool type indicating either it got created as part of default
 * operation or user has configured the group.
 * Depends on the creator of the pool, its decided to free up
 * later or not.
 */
enum rpool_creator {
	RDMACG_RPOOL_CREATOR_DEFAULT,
	RDMACG_RPOOL_CREATOR_USR,
};

/**
 * resource pool object which represents, per cgroup, per device,
 * per resource pool_type resources. There are multiple instance
 * of this object per cgroup, therefore it cannot be embedded within
 * rdma_cgroup structure. Its maintained as list.
 */
struct cg_resource_pool {
	struct list_head cg_list;
	struct rdmacg_device *device;
	enum rdmacg_resource_pool_type type;

	struct rdmacg_resource *resources;

	atomic_t refcnt;	/* count active user tasks of this pool */
	enum rpool_creator creator;	/* user created or default type */
};

static struct rdma_cgroup *css_rdmacg(struct cgroup_subsys_state *css)
{
	return container_of(css, struct rdma_cgroup, css);
}

static struct rdma_cgroup *parent_rdmacg(struct rdma_cgroup *cg)
{
	return css_rdmacg(cg->css.parent);
}

static inline struct rdma_cgroup *task_rdmacg(struct task_struct *task)
{
	return css_rdmacg(task_css(task, rdma_cgrp_id));
}

static struct rdmacg_resource_pool_ops*
get_pool_ops(struct rdmacg_device *device,
	     enum rdmacg_resource_pool_type pool_type)
{
	return device->rpool_ops[pool_type];
}

static inline void set_resource_limit(struct cg_resource_pool *rpool,
				      int index, int new_max)
{
	rpool->resources[index].max = new_max;
}

static void free_cg_rpool(struct cg_resource_pool *rpool)
{
	kfree(rpool->resources);
	kfree(rpool);
}

static void dealloc_cg_rpool(struct rdma_cgroup *cg,
			     struct cg_resource_pool *rpool)
{
	/*
	 * Don't free the resource pool which is created by the
	 * user, otherwise we lose the configured limits. We don't
	 * gain much either by splitting storage of limit and usage.
	 * So keep it around until user deletes the limits.
	 */
	if (rpool->creator == RDMACG_RPOOL_CREATOR_USR)
		return;

	spin_lock(&cg->rpool_list_lock);

	/*
	 * If its started getting used by other task, before we take the
	 * spin lock, then skip freeing it.
	 */
	if (atomic_read(&rpool->refcnt) == 0) {
		list_del_init(&rpool->cg_list);
		spin_unlock(&cg->rpool_list_lock);

		free_cg_rpool(rpool);
		return;
	}
	spin_unlock(&cg->rpool_list_lock);
}

static void put_cg_rpool(struct rdma_cgroup *cg,
			 struct cg_resource_pool *rpool)
{
	if (atomic_dec_and_test(&rpool->refcnt))
		dealloc_cg_rpool(cg, rpool);
}

static struct cg_resource_pool*
alloc_cg_rpool(struct rdma_cgroup *cg,
	       struct rdmacg_device *device,
	       int count,
	       enum rdmacg_resource_pool_type type)
{
	struct cg_resource_pool *rpool;
	int i, ret;

	rpool = kzalloc(sizeof(*rpool), GFP_KERNEL);
	if (!rpool) {
		ret = -ENOMEM;
		goto err;
	}
	rpool->resources = kcalloc(count, sizeof(*rpool->resources),
				   GFP_KERNEL);
	if (!rpool->resources) {
		ret = -ENOMEM;
		goto alloc_err;
	}

	/* set pool ownership and type, so that it can be freed correctly */
	rpool->device = device;
	rpool->type = type;
	INIT_LIST_HEAD(&rpool->cg_list);
	rpool->creator = RDMACG_RPOOL_CREATOR_DEFAULT;

	for (i = 0; i < count; i++)
		set_resource_limit(rpool, i, S32_MAX);

	return rpool;

alloc_err:
	kfree(rpool);
err:
	return ERR_PTR(ret);
}

static struct cg_resource_pool*
find_cg_rpool_locked(struct rdma_cgroup *cg,
		     struct rdmacg_device *device,
		     enum rdmacg_resource_pool_type type)

{
	struct cg_resource_pool *pool;

	lockdep_assert_held(cg->rpool_list_lock);

	list_for_each_entry(pool, &cg->rpool_head, cg_list)
		if (pool->device == device && pool->type == type)
			return pool;

	return NULL;
}

static struct cg_resource_pool*
find_cg_rpool(struct rdma_cgroup *cg,
	      struct rdmacg_device *device,
	      enum rdmacg_resource_pool_type type)
{
	struct cg_resource_pool *rpool;

	spin_lock(&cg->rpool_list_lock);
	rpool = find_cg_rpool_locked(cg, device, type);
	spin_unlock(&cg->rpool_list_lock);
	return rpool;
}

/**
 * get_cg_rpool - get pid_cg map reference.
 * @cg: cgroup for which resouce pool to be allocated.
 * @device: device for which to allocate resource pool.
 * @type: type of the resource pool.
 *
 * It searches a cgroup resource pool object for a given device and resource
 * type, if it finds it would return it. If none is present, it would allocate
 * new resource pool entry of default type.
 * Returns resource pool on success, else return error pointer or null.
 */
static struct cg_resource_pool*
get_cg_rpool(struct rdma_cgroup *cg,
	     struct rdmacg_device *device,
	     enum rdmacg_resource_pool_type type)
{
	struct cg_resource_pool *rpool, *other_rpool;
	struct rdmacg_pool_info *pool_info;
	struct rdmacg_resource_pool_ops *ops;
	int ret = 0;

	spin_lock(&cg->rpool_list_lock);
	rpool = find_cg_rpool_locked(cg, device, type);
	if (rpool) {
		atomic_inc(&rpool->refcnt);
		spin_unlock(&cg->rpool_list_lock);
		return rpool;
	}
	spin_unlock(&cg->rpool_list_lock);

	/*
	 * ops cannot be NULL at this stage, as caller made to charge/get
	 * the resource pool being aware of such need and invoking it
	 * because it has setup resource pool ops.
	 */
	ops = get_pool_ops(device, type);
	pool_info = ops->get_resource_pool_tokens(device);
	if (!pool_info) {
		ret = -EINVAL;
		goto err;
	}
	if (!pool_info->resource_count) {
		ret = -EINVAL;
		goto err;
	}

	rpool = alloc_cg_rpool(cg, device, pool_info->resource_count, type);
	if (IS_ERR_OR_NULL(rpool))
		return rpool;

	/*
	 * cgroup lock is held to synchronize with multiple
	 * resource pool creation in parallel.
	 */
	spin_lock(&cg->rpool_list_lock);
	other_rpool = find_cg_rpool_locked(cg, device, type);

	/*
	 * if other task added resource pool for this device for this cgroup
	 * free up which was recently created and use the one we found.
	 */
	if (other_rpool) {
		atomic_inc(&other_rpool->refcnt);
		spin_unlock(&cg->rpool_list_lock);
		free_cg_rpool(rpool);
		return other_rpool;
	}

	atomic_inc(&rpool->refcnt);
	list_add_tail(&rpool->cg_list, &cg->rpool_head);

	spin_unlock(&cg->rpool_list_lock);
	return rpool;

err:
	spin_unlock(&cg->rpool_list_lock);
	return ERR_PTR(ret);
}

/**
 * uncharge_cg_resource - hierarchically uncharge resource for rdma cgroup
 * @cg: pointer to cg to uncharge and all parents in hierarchy
 * @device: pointer to ib device
 * @type: the type of resource pool to uncharge
 * @index: index of the resource to uncharge in cg (resource pool)
 * @num: the number of rdma resource to uncharge
 *
 * It also frees the resource pool in the hierarchy for the resource pool
 * of default type which was created as part of charing operation.
 */
static void uncharge_cg_resource(struct rdma_cgroup *cg,
				 struct rdmacg_device *device,
				 enum rdmacg_resource_pool_type type,
				 int index, int num)
{
	struct cg_resource_pool *rpool;

	rpool = find_cg_rpool(cg, device, type);
	/*
	 * A negative count (or overflow) is invalid,
	 * it indicates a bug in the rdma controller.
	 */
	WARN_ON_ONCE(atomic_add_negative(-num,
					 &rpool->resources[index].usage));
	put_cg_rpool(cg, rpool);
}

/**
 * rdmacg_uncharge_resource - hierarchically uncharge rdma resource count
 * @device: pointer to rdmacg device
 * @type: the type of resource pool to charge
 * @index: index of the resource to uncharge in cg in given resource pool type
 * @num: the number of rdma resource to uncharge
 *
 */
void rdmacg_uncharge(struct rdma_cgroup *cg,
		     struct rdmacg_device *device,
		     enum rdmacg_resource_pool_type type,
		     int index, int num)
{
	struct rdma_cgroup *p;

	for (p = cg; p; p = parent_rdmacg(p))
		uncharge_cg_resource(p, device, type, index, num);

	css_put(&cg->css);
}
EXPORT_SYMBOL(rdmacg_uncharge);

/**
 * rdmacg_try_charge_resource - hierarchically try to charge
 * the rdma resource count
 * @device: pointer to rdmacg device
 * @rdmacg: pointer to rdma cgroup which will own this resource.
 * @type: the type of resource pool to charge
 * @index: index of the resource to charge in cg (resource pool)
 * @num: the number of rdma resource to charge
 *
 * This function follows charing resource in hierarchical way.
 * It will fail if the charge would cause the new value to exceed the
 * hierarchical limit.
 * Returns 0 if the charge succeded, otherwise -EAGAIN, -ENOMEM or -EINVAL.
 * Returns pointer to rdmacg for this resource.
 *
 * Charger needs to account resources on three criteria.
 * (a) per cgroup (b) per device & (c) per resource type resource usage.
 * Per cgroup resource usage ensures that tasks of cgroup doesn't cross
 * the configured limits.
 * Per device provides granular configuration in multi device usage.
 * Per resource type allows resource charing for multiple
 * category of resources - currently (a) verb level & (b) hw driver
 * defined.
 * It allocates resource pool in the hierarchy for each parent it come
 * across for first resource. Later on resource pool will be available.
 * Therefore it will be much faster thereon to charge/uncharge.
 */
int rdmacg_try_charge(struct rdma_cgroup **rdmacg,
		      struct rdmacg_device *device,
		      enum rdmacg_resource_pool_type type,
		      int index, int num)
{
	struct cg_resource_pool *rpool;
	struct rdma_cgroup *cg, *p, *q;
	int ret;

	cg = task_rdmacg(current);

	for (p = cg; p; p = parent_rdmacg(p)) {
		s64 new;

		rpool = get_cg_rpool(p, device, type);
		if (IS_ERR_OR_NULL(rpool)) {
			ret = PTR_ERR(rpool);
			goto err;
		}

		new = atomic_add_return(num, &rpool->resources[index].usage);
		if (new > rpool->resources[index].max) {
			ret = -EAGAIN;
			goto revert;
		}
	}
	/*
	 * hold on to the css, as cgroup can be removed but resource
	 * accounting happens on css.
	 */
	css_get(&cg->css);
	*rdmacg = cg;
	return 0;

revert:
	uncharge_cg_resource(p, device, type, index, num);
err:
	for (q = cg; q != p; q = parent_rdmacg(q))
		uncharge_cg_resource(q, device, type, index, num);
	return ret;
}
EXPORT_SYMBOL(rdmacg_try_charge);

/**
 * rdmacg_register_rdmacg_device - register rdmacg device to rdma controller.
 * @device: pointer to rdmacg device whose resources need to be accounted.
 *
 * If IB stack wish a device to participate in rdma cgroup resource
 * tracking, it must invoke this API to register with rdma cgroup before
 * any user space application can start using the RDMA resources. IB stack
 * and/or HCA driver must invoke rdmacg_set_rpool_ops() either for verb or
 * for hw or for both the types as they are mandetory operations to have
 * to register with rdma cgroup.
 *
 */
void rdmacg_register_device(struct rdmacg_device *device, char *dev_name)
{
	INIT_LIST_HEAD(&device->rdmacg_list);
	device->name = dev_name;

	mutex_lock(&dev_mutex);
	list_add_tail(&device->rdmacg_list, &dev_list);
	mutex_unlock(&dev_mutex);
}
EXPORT_SYMBOL(rdmacg_register_device);

/**
 * rdmacg_unregister_rdmacg_device - unregister the rdmacg device
 * from rdma controller.
 * @device: pointer to rdmacg device which was previously registered with rdma
 *          controller using rdmacg_register_device().
 *
 * IB stack must invoke this after all the resources of the IB device
 * are destroyed and after ensuring that no more resources will be created
 * when this API is invoked.
 */
void rdmacg_unregister_device(struct rdmacg_device *device)
{
	/*
	 * Synchronize with any active resource settings,
	 * usage query happening via configfs.
	 * At this stage, there should not be any active resource pools
	 * for this device, as RDMA/IB stack is expected to shutdown,
	 * tear down all the applications and free up resources.
	 */
	mutex_lock(&dev_mutex);
	list_del_init(&device->rdmacg_list);
	mutex_unlock(&dev_mutex);
}
EXPORT_SYMBOL(rdmacg_unregister_device);

/**
 * rdmacg_set_rpool_ops - helper function to set the resource pool
 *                        call back function to provide matching
 *                        string tokens to for defining names of the
 *                        resources.
 * @device: pointer to rdmacg device for which to set the resource pool
 *          operations.
 * @pool_type: resource pool type, either VERBS type or HW type.
 * @ops: pointer to function pointers to return (a) matching token
 *       which will be invoked to find out string tokens and
 *       (b) to find out maximum resource limits that is supported
 *       by each device of given resource pool type.
 *
 * This helper function allows setting resouce pool specific operation
 * callback functions.
 * It must be called one by respective subsystem that implements resource
 * definition of rdma and owns the task of charging/uncharing the resource.
 * This must be called before ib device is registered with the rdma cgroup
 * using rdmacg_register_rdmacg_device().
 */
void rdmacg_set_rpool_ops(struct rdmacg_device *device,
			  enum rdmacg_resource_pool_type pool_type,
			  struct rdmacg_resource_pool_ops *ops)
{
	device->rpool_ops[pool_type] = ops;
}
EXPORT_SYMBOL(rdmacg_set_rpool_ops);

/**
 * rdmacg_clear_rpool_ops -
 * helper function to clear the resource pool ops which was setup
 * before using rdmacg_set_rpool_ops.
 * @device: pointer to ib device for which to clear the resource pool
 *          operations.
 * @pool_type: resource pool type, either VERBS type or HW type.
 *
 * This must be called after deregistering ib device using rdma cgroup
 * using rdmacg_unregister_rdmacg_device().
 */
void rdmacg_clear_rpool_ops(struct rdmacg_device *device,
			    enum rdmacg_resource_pool_type pool_type)
{
	device->rpool_ops[pool_type] = NULL;
}
EXPORT_SYMBOL(rdmacg_clear_rpool_ops);

/**
 * rdmacg_query_limit - query the resource limits that
 * might have been configured by the user.
 * @device: pointer to ib device
 * @pid: thread group pid that wants to know the resource limits
 *       of the cgroup.
 * @type: the type of resource pool to know the limits of.
 * @limits: pointer to an array of limits where rdma cg will provide
 *          the configured limits of the cgroup.
 * @limits_array_len: number of array elements to be filled up at limits.
 *
 * This function follows charing resource in hierarchical way.
 * It will fail if the charge would cause the new value to exceed the
 * hierarchical limit.
 * Returns 0 if the charge succeded, otherwise appropriate error code.
 */
int rdmacg_query_limit(struct rdmacg_device *device,
		       enum rdmacg_resource_pool_type type,
		       int *limits, int max_count)
{
	struct rdma_cgroup *cg, *p, *q;
	struct cg_resource_pool *rpool;
	struct rdmacg_pool_info *pool_info;
	struct rdmacg_resource_pool_ops *ops;
	int i, status = 0;

	cg = task_rdmacg(current);

	ops = get_pool_ops(device, type);
	if (!ops) {
		status = -EINVAL;
		goto err;
	}
	pool_info = ops->get_resource_pool_tokens(device);
	if (!pool_info) {
		status = -EINVAL;
		goto err;
	}
	if (pool_info->resource_count == 0 ||
	    max_count > pool_info->resource_count) {
		status = -EINVAL;
		goto err;
	}

	/* initialize to max */
	for (i = 0; i < max_count; i++)
		limits[i] = S32_MAX;

	/* check in hirerchy which pool get the least amount of
	 * resource limits.
	 */
	for (p = cg; p; p = parent_rdmacg(p)) {
		/* get handle to cgroups rpool */
		rpool = get_cg_rpool(cg, device, type);
		if (IS_ERR_OR_NULL(rpool))
			goto rpool_err;

		for (i = 0; i < max_count; i++)
			limits[i] = min_t(int, limits[i],
					rpool->resources[i].max);

		put_cg_rpool(cg, rpool);
	}
	return 0;

rpool_err:
	for (q = cg; q != p; q = parent_rdmacg(q))
		put_cg_rpool(q, find_cg_rpool(q, device, type));
err:
	return status;
}
EXPORT_SYMBOL(rdmacg_query_limit);

static int rdmacg_parse_limits(char *options, struct match_token *opt_tbl,
			       int *new_limits, u64 *enables)
{
	substring_t argstr[MAX_OPT_ARGS];
	const char *c;
	int err = -ENOMEM;

	/* parse resource options */
	while ((c = strsep(&options, " ")) != NULL) {
		int token, intval, ret;

		err = -EINVAL;
		token = match_token((char *)c, opt_tbl, argstr);
		if (token < 0)
			goto err;

		ret = match_int(&argstr[0], &intval);
		if (ret < 0) {
			pr_err("bad value (not int) at '%s'\n", c);
			goto err;
		}
		new_limits[token] = intval;
		*enables |= BIT(token);
	}
	return 0;

err:
	return err;
}

static enum rdmacg_resource_pool_type of_to_pool_type(int of_type)
{
	enum rdmacg_resource_pool_type pool_type;

	switch (of_type) {
	case RDMACG_VERB_RESOURCE_MAX:
	case RDMACG_VERB_RESOURCE_STAT:
		pool_type = RDMACG_RESOURCE_POOL_VERB;
		break;
	case RDMACG_HW_RESOURCE_MAX:
	case RDMACG_HW_RESOURCE_STAT:
	default:
		pool_type = RDMACG_RESOURCE_POOL_HW;
		break;
	};
	return pool_type;
}

static struct rdmacg_device *_rdmacg_get_device(const char *name)
{
	struct rdmacg_device *device;

	list_for_each_entry(device, &dev_list, rdmacg_list)
		if (!strcmp(name, device->name))
			return device;

	return NULL;
}

static void remove_unused_cg_rpool(struct rdma_cgroup *cg,
				   struct rdmacg_device *device,
				   enum rdmacg_resource_pool_type type,
				   int count)
{
	struct cg_resource_pool *rpool = NULL;
	int i;

	spin_lock(&cg->rpool_list_lock);
	rpool = find_cg_rpool_locked(cg, device, type);
	if (!rpool) {
		spin_unlock(&cg->rpool_list_lock);
		return;
	}
	/*
	 * Found the resource pool, check now what to do
	 * based on its reference count.
	 */
	if (atomic_read(&rpool->refcnt) == 0) {
		/*
		 * If there is no active user of the rpool,
		 * free the memory, default group will get
		 * allocated automatically when new resource
		 * is created.
		 */
		list_del_init(&rpool->cg_list);

		spin_unlock(&cg->rpool_list_lock);

		free_cg_rpool(rpool);
	} else {
		/*
		 * If there are active processes and thereby active resources,
		 * than set limits to max. Resource pool will get freed later
		 * on when last resource will get deallocated.
		 */
		for (i = 0; i < count; i++)
			set_resource_limit(rpool, i, S32_MAX);
		rpool->creator = RDMACG_RPOOL_CREATOR_DEFAULT;

		spin_unlock(&cg->rpool_list_lock);
	}
}

static ssize_t rdmacg_resource_set_max(struct kernfs_open_file *of,
				       char *buf, size_t nbytes, loff_t off)
{
	struct rdma_cgroup *cg = css_rdmacg(of_css(of));
	const char *dev_name;
	struct cg_resource_pool *rpool;
	struct rdmacg_resource_pool_ops *ops;
	struct rdmacg_device *device;
	char *options = strstrip(buf);
	enum rdmacg_resource_pool_type pool_type;
	struct rdmacg_pool_info *resource_tokens;
	u64 enables = 0;
	int *new_limits;
	int i = 0, ret = 0;
	bool remove = false;

	/* extract the device name first */
	dev_name = strsep(&options, " ");
	if (!dev_name) {
		ret = -EINVAL;
		goto err;
	}

	/* check if user asked to remove the cgroup limits */
	if (strstr(options, RDMACG_USR_CMD_REMOVE))
		remove = true;

	/* acquire lock to synchronize with hot plug devices */
	mutex_lock(&dev_mutex);

	device = _rdmacg_get_device(dev_name);
	if (!device) {
		ret = -ENODEV;
		goto parse_err;
	}
	pool_type = of_to_pool_type(of_cft(of)->private);
	ops = get_pool_ops(device, pool_type);
	if (!ops) {
		ret = -EINVAL;
		goto parse_err;
	}

	resource_tokens = ops->get_resource_pool_tokens(device);
	if (IS_ERR_OR_NULL(resource_tokens)) {
		ret = -EINVAL;
		goto parse_err;
	}

	if (remove) {
		remove_unused_cg_rpool(cg, device, pool_type,
				       resource_tokens->resource_count);
		/* user asked to clear the limits; ignore rest of the options */
		goto parse_err;
	}

	new_limits = kcalloc(resource_tokens->resource_count, sizeof(int),
			     GFP_KERNEL);
	if (!new_limits) {
		ret = -ENOMEM;
		goto parse_err;
	}
	/* user didn't ask to remove, act on the options */
	ret = rdmacg_parse_limits(options,
				  resource_tokens->resource_table,
				  new_limits, &enables);
	if (ret)
		goto opt_err;

	rpool = get_cg_rpool(cg, device, pool_type);
	if (IS_ERR_OR_NULL(rpool)) {
		if (IS_ERR(rpool))
			ret = PTR_ERR(rpool);
		else
			ret = -ENOMEM;
		goto opt_err;
	}
	/*
	 * Set pool type as user regardless of previous type as
	 * user is configuring the limit now.
	 */
	rpool->creator = RDMACG_RPOOL_CREATOR_USR;

	/* now set the new limits on the existing or newly created rool */
	while (enables) {
		/* if user set the limit, enables bit is set */
		if (enables & BIT(i)) {
			enables &= ~BIT(i);
			set_resource_limit(rpool, i, new_limits[i]);
		}
		i++;
	}
	atomic_dec(&rpool->refcnt);
opt_err:
	kfree(new_limits);
parse_err:
	mutex_unlock(&dev_mutex);
err:
	return ret ?: nbytes;
}

static int get_resource_val(struct rdmacg_resource *resource,
			    enum rdmacg_file_type type)
{
	int val = 0;

	switch (type) {
	case RDMACG_VERB_RESOURCE_MAX:
	case RDMACG_HW_RESOURCE_MAX:
		val = resource->max;
		break;
	case RDMACG_VERB_RESOURCE_STAT:
	case RDMACG_HW_RESOURCE_STAT:
		val = atomic_read(&resource->usage);
		break;
	default:
		val = 0;
		break;
	};
	return val;
}

static u32 *get_cg_rpool_values(struct rdma_cgroup *cg,
				struct rdmacg_device *device,
				enum rdmacg_resource_pool_type pool_type,
				enum rdmacg_file_type resource_type,
				int resource_count)
{
	struct cg_resource_pool *rpool;
	u32 *value_tbl;
	int i, ret;

	value_tbl = kcalloc(resource_count, sizeof(u32), GFP_KERNEL);
	if (!value_tbl) {
		ret = -ENOMEM;
		goto err;
	}

	rpool = get_cg_rpool(cg, device, pool_type);
	if (IS_ERR_OR_NULL(rpool)) {
		if (IS_ERR(rpool))
			ret = PTR_ERR(rpool);
		else
			ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < resource_count; i++) {
		value_tbl[i] = get_resource_val(&rpool->resources[i],
						resource_type);
	}
	put_cg_rpool(cg, rpool);
	return value_tbl;

err:
	return ERR_PTR(ret);
}

static int print_rpool_values(struct seq_file *sf,
			      struct rdmacg_pool_info *pool_info,
			      u32 *value_tbl)
{
	struct match_token *resource_table;
	char *name;
	int i, ret, name_len;

	resource_table = pool_info->resource_table;

	for (i = 0; i < pool_info->resource_count; i++) {
		if (value_tbl[i] == S32_MAX) {
			/*
			 * Since we have to print max string, and token
			 * string cannot be changed, make a copy and print
			 * from there.
			 */
			name_len = strlen(resource_table[i].pattern);
			name = kzalloc(name_len, GFP_KERNEL);
			if (!name) {
				ret = -ENOMEM;
				goto err;
			}
			strcpy(name, resource_table[i].pattern);

			/* change to string instead of int from %d to %s */
			name[name_len - 1] = 's';
			seq_printf(sf, name, "max");
			kfree(name);
		} else {
			seq_printf(sf, resource_table[i].pattern, value_tbl[i]);
		}
		seq_putc(sf, ' ');
	}
	return 0;

err:
	return ret;
}

static int rdmacg_resource_read(struct seq_file *sf, void *v)
{
	struct rdmacg_device *device;
	struct rdma_cgroup *cg = css_rdmacg(seq_css(sf));
	struct rdmacg_pool_info *pool_info;
	struct rdmacg_resource_pool_ops *ops;
	u32 *value_tbl;
	enum rdmacg_resource_pool_type pool_type;
	int ret = 0;

	pool_type = of_to_pool_type(seq_cft(sf)->private);

	mutex_lock(&dev_mutex);

	list_for_each_entry(device, &dev_list, rdmacg_list) {
		ops = get_pool_ops(device, pool_type);
		if (!ops)
			continue;

		pool_info = ops->get_resource_pool_tokens(device);
		if (IS_ERR_OR_NULL(pool_info)) {
			ret = -EINVAL;
			goto err;
		}

		/* get the value from resource pool */
		value_tbl = get_cg_rpool_values(cg, device, pool_type,
						seq_cft(sf)->private,
						pool_info->resource_count);
		if (!value_tbl) {
			ret = -ENOMEM;
			goto err;
		}

		seq_printf(sf, "%s ", device->name);
		ret = print_rpool_values(sf, pool_info, value_tbl);
		seq_putc(sf, '\n');

		kfree(value_tbl);
		if (ret)
			break;
	}
	mutex_unlock(&dev_mutex);

err:
	return ret;
}

static struct cftype rdmacg_files[] = {
	{
		.name = "verb.max",
		.write = rdmacg_resource_set_max,
		.seq_show = rdmacg_resource_read,
		.private = RDMACG_VERB_RESOURCE_MAX,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "verb.current",
		.seq_show = rdmacg_resource_read,
		.private = RDMACG_VERB_RESOURCE_STAT,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "hw.max",
		.write = rdmacg_resource_set_max,
		.seq_show = rdmacg_resource_read,
		.private = RDMACG_HW_RESOURCE_MAX,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "hw.current",
		.seq_show = rdmacg_resource_read,
		.private = RDMACG_HW_RESOURCE_STAT,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{ }	/* terminate */
};

static struct cgroup_subsys_state *
rdmacg_css_alloc(struct cgroup_subsys_state *parent)
{
	struct rdma_cgroup *cg;

	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&cg->rpool_head);
	spin_lock_init(&cg->rpool_list_lock);
	return &cg->css;
}

static void rdmacg_css_free(struct cgroup_subsys_state *css)
{
	struct rdma_cgroup *cg = css_rdmacg(css);

	kfree(cg);
}

/**
 * rdmacg_css_offline - cgroup css_offline callback
 * @css: css of interest
 *
 * This function is called when @css is about to go away and responsible
 * for shooting down all rdmacg associated with @css. As part of that it
 * marks all the resource pool as default type, so that when resources are
 * uncharged, associated resource pool can be freed as well.
 *
 */
static void rdmacg_css_offline(struct cgroup_subsys_state *css)
{
	struct rdma_cgroup *cg = css_rdmacg(css);
	struct cg_resource_pool *rpool;

	spin_lock(&cg->rpool_list_lock);

	list_for_each_entry(rpool, &cg->rpool_head, cg_list)
		rpool->creator = RDMACG_RPOOL_CREATOR_DEFAULT;

	spin_unlock(&cg->rpool_list_lock);
}

struct cgroup_subsys rdma_cgrp_subsys = {
	.css_alloc	= rdmacg_css_alloc,
	.css_free	= rdmacg_css_free,
	.css_offline	= rdmacg_css_offline,
	.legacy_cftypes	= rdmacg_files,
	.dfl_cftypes	= rdmacg_files,
};
