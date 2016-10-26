/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include "main.h"
#include "peer.h"
#include "user.h"
#include "util.h"
#include "util/active.h"

/**
 * bus1_peer_new() - allocate new peer
 *
 * Allocate a new peer. It is immediately activated and ready for use. It is
 * not linked into any context. The caller will get exclusively access to the
 * peer object on success.
 *
 * Note that the peer is opened on behalf of 'current'. That is, it pins its
 * credentials and namespaces.
 *
 * Return: Pointer to peer, ERR_PTR on failure.
 */
struct bus1_peer *bus1_peer_new(void)
{
	static atomic64_t peer_ids = ATOMIC64_INIT(0);
	const struct cred *cred = current_cred();
	struct bus1_peer *peer;
	struct bus1_user *user;

	user = bus1_user_ref_by_uid(cred->uid);
	if (IS_ERR(user))
		return ERR_CAST(user);

	peer = kmalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer) {
		bus1_user_unref(user);
		return ERR_PTR(-ENOMEM);
	}

	/* initialize constant fields */
	peer->id = atomic64_inc_return(&peer_ids);
	peer->flags = 0;
	peer->cred = get_cred(current_cred());
	peer->pid_ns = get_pid_ns(task_active_pid_ns(current));
	peer->user = user;
	peer->debugdir = NULL;
	init_waitqueue_head(&peer->waitq);
	bus1_active_init(&peer->active);

	/* initialize data section */
	mutex_init(&peer->data.lock);
	peer->data.pool = BUS1_POOL_NULL;
	bus1_queue_init(&peer->data.queue);

	/* initialize peer-private section */
	mutex_init(&peer->local.lock);
	peer->local.map_handles = RB_ROOT;
	peer->local.handle_ids = 0;

	if (!IS_ERR_OR_NULL(bus1_debugdir)) {
		char idstr[22];

		snprintf(idstr, sizeof(idstr), "peer-%llx", peer->id);

		peer->debugdir = debugfs_create_dir(idstr, bus1_debugdir);
		if (!peer->debugdir) {
			pr_err("cannot create debugfs dir for peer %llx\n",
			       peer->id);
		} else if (!IS_ERR_OR_NULL(peer->debugdir)) {
			bus1_debugfs_create_atomic_x("active", S_IRUGO,
						     peer->debugdir,
						     &peer->active.count);
		}
	}

	bus1_active_activate(&peer->active);
	return peer;
}

static int bus1_peer_disconnect(struct bus1_peer *peer)
{
	bus1_active_deactivate(&peer->active);
	bus1_active_drain(&peer->active, &peer->waitq);

	if (!bus1_active_cleanup(&peer->active, &peer->waitq,
				 NULL, NULL))
		return -ESHUTDOWN;

	return 0;
}

/**
 * bus1_peer_free() - destroy peer
 * @peer:	peer to destroy, or NULL
 *
 * Destroy a peer object that was previously allocated via bus1_peer_new().
 * This synchronously waits for any outstanding operations on this peer to
 * finish, then releases all linked resources and deallocates the peer in an
 * rcu-delayed manner.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_peer *bus1_peer_free(struct bus1_peer *peer)
{
	if (!peer)
		return NULL;

	/* disconnect from environment */
	bus1_peer_disconnect(peer);

	/* deinitialize peer-private section */
	WARN_ON(!RB_EMPTY_ROOT(&peer->local.map_handles));
	mutex_destroy(&peer->local.lock);

	/* deinitialize data section */
	bus1_queue_deinit(&peer->data.queue);
	bus1_pool_deinit(&peer->data.pool);
	mutex_destroy(&peer->data.lock);

	/* deinitialize constant fields */
	debugfs_remove_recursive(peer->debugdir);
	bus1_active_deinit(&peer->active);
	peer->user = bus1_user_unref(peer->user);
	put_pid_ns(peer->pid_ns);
	put_cred(peer->cred);
	kfree_rcu(peer, rcu);

	return NULL;
}
