/*
 * Copyright (C) 2008 IBM Corporation
 * Author: Yuqiong Sun <suny@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 */

#include <linux/export.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/ima.h>

#include "ima.h"

static void get_ima_ns(struct ima_namespace *ns);

int ima_init_namespace(struct ima_namespace *ns)
{
	ns->ns_status_tree = RB_ROOT;
	rwlock_init(&ns->ns_status_lock);
	ns->ns_status_cache = KMEM_CACHE(ns_status, SLAB_PANIC);
	return 0;
}

int ima_ns_init(void)
{
	return ima_init_namespace(&init_ima_ns);
}

static struct ima_namespace *create_ima_ns(void)
{
	struct ima_namespace *ima_ns;

	ima_ns = kmalloc(sizeof(*ima_ns), GFP_KERNEL);
	if (ima_ns)
		kref_init(&ima_ns->kref);

	return ima_ns;
}

/**
 * Clone a new ns copying an original ima namespace, setting refcount to 1
 *
 * @old_ns: old ima namespace to clone
 * @user_ns: user namespace that current task runs in
 * Return ERR_PTR(-ENOMEM) on error (failure to kmalloc), new ns otherwise
 */
static struct ima_namespace *clone_ima_ns(struct user_namespace *user_ns,
					  struct ima_namespace *old_ns)
{
	struct ima_namespace *ns;
	int err;

	ns = create_ima_ns();
	if (!ns)
		return ERR_PTR(-ENOMEM);

	err = ns_alloc_inum(&ns->ns);
	if (err) {
		kfree(ns);
		return ERR_PTR(err);
	}

	ns->ns.ops = &imans_operations;
	get_ima_ns(old_ns);
	ns->parent = old_ns;
	ns->user_ns = get_user_ns(user_ns);

	ima_init_namespace(ns);

	return ns;
}

/**
 * Copy task's ima namespace, or clone it if flags specifies CLONE_NEWNS.
 *
 * @flags: flags used in the clone syscall
 * @user_ns: user namespace that current task runs in
 * @old_ns: old ima namespace to clone
 */

struct ima_namespace *copy_ima(unsigned long flags,
			       struct user_namespace *user_ns,
			       struct ima_namespace *old_ns)
{
	struct ima_namespace *new_ns;

	BUG_ON(!old_ns);
	get_ima_ns(old_ns);

	if (!(flags & CLONE_NEWNS))
		return old_ns;

	new_ns = clone_ima_ns(user_ns, old_ns);
	put_ima_ns(old_ns);

	return new_ns;
}

static void free_ns_status_cache(struct ima_namespace *ns)
{
	struct ns_status *status, *next;

	write_lock(&ns->ns_status_lock);
	rbtree_postorder_for_each_entry_safe(status, next,
					     &ns->ns_status_tree, rb_node)
		kmem_cache_free(ns->ns_status_cache, status);
	ns->ns_status_tree = RB_ROOT;
	write_unlock(&ns->ns_status_lock);
	kmem_cache_destroy(ns->ns_status_cache);
}

static void destroy_ima_ns(struct ima_namespace *ns)
{
	put_user_ns(ns->user_ns);
	ns_free_inum(&ns->ns);
	free_ns_status_cache(ns);
	kfree(ns);
}

static void free_ima_ns(struct kref *kref)
{
	struct ima_namespace *ns;

	ns = container_of(kref, struct ima_namespace, kref);
	destroy_ima_ns(ns);
}

static void get_ima_ns(struct ima_namespace *ns)
{
	kref_get(&ns->kref);
}

void put_ima_ns(struct ima_namespace *ns)
{
	kref_put(&ns->kref, free_ima_ns);
}

static inline struct ima_namespace *to_ima_ns(struct ns_common *ns)
{
	return container_of(ns, struct ima_namespace, ns);
}

static struct ns_common *imans_get(struct task_struct *task)
{
	struct ima_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->ima_ns;
		get_ima_ns(ns);
	}
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void imans_put(struct ns_common *ns)
{
	put_ima_ns(to_ima_ns(ns));
}

static int imans_install(struct nsproxy *nsproxy, struct ns_common *new)
{
	struct ima_namespace *ns = to_ima_ns(new);

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(current_user_ns(), CAP_SYS_ADMIN))
		return -EPERM;

	get_ima_ns(ns);
	put_ima_ns(nsproxy->ima_ns);
	nsproxy->ima_ns = ns;
	return 0;
}

const struct proc_ns_operations imans_operations = {
	.name = "ima",
	.type = CLONE_NEWNS,
	.get = imans_get,
	.put = imans_put,
	.install = imans_install,
};

struct ima_namespace init_ima_ns = {
	.kref = KREF_INIT(2),
	.user_ns = &init_user_ns,
	.ns.inum = PROC_IMA_INIT_INO,
#ifdef CONFIG_IMA_NS
	.ns.ops = &imans_operations,
#endif
	.parent = NULL,
};
EXPORT_SYMBOL(init_ima_ns);

/*
 * __ima_ns_status_find - return the ns_status associated with an inode
 */
static struct ns_status *__ima_ns_status_find(struct ima_namespace *ns,
					      struct inode *inode)
{
	struct ns_status *status;
	struct rb_node *n = ns->ns_status_tree.rb_node;

	while (n) {
		status = rb_entry(n, struct ns_status, rb_node);

		if (inode < status->inode)
			n = n->rb_left;
		else if (inode->i_ino > status->i_ino)
			n = n->rb_right;
		else
			break;
	}
	if (!n)
		return NULL;

	return status;
}

/*
 * ima_ns_status_find - return the ns_status associated with an inode
 */
static struct ns_status *ima_ns_status_find(struct ima_namespace *ns,
					    struct inode *inode)
{
	struct ns_status *status;

	read_lock(&ns->ns_status_lock);
	status = __ima_ns_status_find(ns, inode);
	read_unlock(&ns->ns_status_lock);

	return status;
}

void insert_ns_status(struct ima_namespace *ns, struct inode *inode,
		      struct ns_status *status)
{
	struct rb_node **p;
	struct rb_node *node, *parent = NULL;
	struct ns_status *test_status;

	p = &ns->ns_status_tree.rb_node;
	while (*p) {
		parent = *p;
		test_status = rb_entry(parent, struct ns_status, rb_node);
		if (inode < test_status->inode)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	node = &status->rb_node;
	rb_link_node(node, parent, p);
	rb_insert_color(node, &ns->ns_status_tree);
}

struct ns_status *ima_get_ns_status(struct ima_namespace *ns,
				    struct inode *inode)
{
	struct ns_status *status;
	int skip_insert = 0;

	status = ima_ns_status_find(ns, inode);
	if (status) {
		/*
		 * Unlike integrity_iint_cache we are not free'ing the
		 * ns_status data when the inode is free'd. So, in addition to
		 * checking the inode pointer, we need to make sure the
		 * (i_generation, i_ino) pair matches as well. In the future
		 * we might want to add support for lazily walking the rbtree
		 * to clean it up.
		 */
		if (inode->i_ino == status->i_ino &&
		    inode->i_generation == status->i_generation)
			return status;

		/* Same inode number is reused, overwrite the ns_status */
		skip_insert = 1;
	} else {
		status = kmem_cache_alloc(ns->ns_status_cache, GFP_NOFS);
		if (!status)
			return ERR_PTR(-ENOMEM);
	}

	write_lock(&ns->ns_status_lock);

	if (!skip_insert)
		insert_ns_status(ns, inode, status);

	status->inode = inode;
	status->i_ino = inode->i_ino;
	status->i_generation = inode->i_generation;
	status->flags = 0UL;
	write_unlock(&ns->ns_status_lock);

	return status;
}

#define IMA_NS_STATUS_ACTIONS	IMA_AUDIT
#define IMA_NS_STATUS_FLAGS	IMA_AUDITED

unsigned long iint_flags(struct integrity_iint_cache *iint,
			 struct ns_status *status)
{
	if (!status)
		return iint->flags;

	return iint->flags & (status->flags & IMA_NS_STATUS_FLAGS);
}

unsigned long set_iint_flags(struct integrity_iint_cache *iint,
			     struct ns_status *status, unsigned long flags)
{
	iint->flags = flags;
	if (status)
		status->flags = flags & IMA_NS_STATUS_FLAGS;
	return flags;
}
