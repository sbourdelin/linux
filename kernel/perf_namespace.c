/*
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation, version 2 of the
 *  License.
 */

#include <linux/export.h>
#include <linux/perf_namespace.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>

static struct perf_namespace *create_perf_ns(struct user_namespace *user_ns)
{
	struct perf_namespace *perf_ns;
	int err;

	perf_ns = kmalloc(sizeof(struct perf_namespace), GFP_KERNEL);
	if (!perf_ns)
		return ERR_PTR(-ENOMEM);

	kref_init(&perf_ns->kref);
	err = ns_alloc_inum(&perf_ns->ns);
	if (err) {
		kfree(perf_ns);
		return ERR_PTR(err);
	}

	perf_ns->ns.ops = &perfns_operations;
	perf_ns->user_ns = get_user_ns(user_ns);
	return perf_ns;
}

struct perf_namespace *copy_perf_ns(unsigned long flags,
	struct user_namespace *user_ns, struct perf_namespace *old_ns)
{
	struct perf_namespace *new_ns;

	BUG_ON(!old_ns);
	get_perf_ns(old_ns);

	if (!(flags & CLONE_NEWPERF))
		return old_ns;

	new_ns = create_perf_ns(user_ns);

	put_perf_ns(old_ns);
	return new_ns;
}

void free_perf_ns(struct kref *kref)
{
	struct perf_namespace *ns;

	ns = container_of(kref, struct perf_namespace, kref);
	put_user_ns(ns->user_ns);
	ns_free_inum(&ns->ns);
	kfree(ns);
}

static inline struct perf_namespace *to_perf_ns(struct ns_common *ns)
{
	return container_of(ns, struct perf_namespace, ns);
}

static struct ns_common *perfns_get(struct task_struct *task)
{
	struct perf_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->perf_ns;
		get_perf_ns(ns);
	}
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void perfns_put(struct ns_common *ns)
{
	put_perf_ns(to_perf_ns(ns));
}

static int perfns_install(struct nsproxy *nsproxy, struct ns_common *new)
{
	struct perf_namespace *ns = to_perf_ns(new);

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(current_user_ns(), CAP_SYS_ADMIN))
		return -EPERM;

	get_perf_ns(ns);
	put_perf_ns(nsproxy->perf_ns);
	nsproxy->perf_ns = ns;
	return 0;
}

const struct proc_ns_operations perfns_operations = {
	.name		= "perf",
	.type		= CLONE_NEWPERF,
	.get		= perfns_get,
	.put		= perfns_put,
	.install	= perfns_install,
};

/*
 * TODO: Find a better place to put this..
 */
struct perf_namespace init_perf_ns = {
	.kref = {
		.refcount = ATOMIC_INIT(2),
	},
	.user_ns = &init_user_ns,
	.ns.inum = PROC_PERF_INIT_INO,
#ifdef CONFIG_PERF_NS
	.ns.ops = &perfns_operations,
#endif
};
EXPORT_SYMBOL_GPL(init_perf_ns);
