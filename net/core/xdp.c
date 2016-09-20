/*
 * Kernel Connection Multiplexor
 *
 * Copyright (c) 2016 Tom Herbert <tom@herbertland.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */
#include <net/xdp.h>

DEFINE_STATIC_KEY_FALSE(xdp_hooks_needed);
EXPORT_SYMBOL(xdp_hooks_needed);

static DEFINE_MUTEX(xdp_hook_mutex);

/* Mutex xdp_hook_mutex must be held */
static int __xdp_register_one_hook(struct net_device *dev,
				   struct list_head *hook_list,
				   struct xdp_hook_entry *entry,
				   struct xdp_hook_ops *prev_elem)
{
	int err;

	/* Check if we driver XDP needs initialization */
	if (!dev->xdp_hook_cnt && dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_DEV_INIT;
		err = dev->netdev_ops->ndo_xdp(dev, &xdp_op);
		if (err)
			return err;
	}

	list_add_rcu(&entry->ops.list, prev_elem->list.prev);
	static_branch_inc(&xdp_hooks_needed);
	dev->xdp_hook_cnt++;

	return 0;
}

int __xdp_register_hook(struct net_device *dev,
			struct list_head *hook_list,
			const struct xdp_hook_ops *reg,
			bool change)
{
	struct xdp_hook_entry *entry;
	struct xdp_hook_ops *elem, *prevelem = NULL;
	int err;

	mutex_lock(&xdp_hook_mutex);

	/* Walk list, see if hook is already registered and determin insertion
	 * point.
	 */
	list_for_each_entry(elem, hook_list, list) {
		struct xdp_hook_entry *tent;

		tent = container_of(elem, struct xdp_hook_entry, ops);
		if (tent->orig_ops == reg) {
			if (change) {
				void *old_priv;

				/* Only allow changing priv field in an existing
				 * hook.
				 */
				old_priv = rcu_dereference_protected(elem->priv,
					lockdep_is_held(&xdp_hook_mutex));
				rcu_assign_pointer(elem->priv, reg->priv);
				if (old_priv && elem->put_priv)
					elem->put_priv(old_priv);
				err = 0;
				goto out;
			} else {
				/* Already registered */
				err = -EALREADY;
				goto out;
			}
		}
		if (reg->priority < elem->priority)
			prevelem = elem;
	}

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->orig_ops = reg;
	entry->ops = *reg;

	if (prevelem)
		elem = prevelem;

	err = __xdp_register_one_hook(dev, hook_list, entry, elem);
	if (err)
		goto err;

out:
	mutex_unlock(&xdp_hook_mutex);

	return 0;

err:
	mutex_unlock(&xdp_hook_mutex);
	kfree(entry);
	return err;
}
EXPORT_SYMBOL_GPL(__xdp_register_hook);

/* Mutext xdp_hook_mutex must be held */
static void __xdp_unregister_one_hook(struct net_device *dev,
				      struct list_head *hook_list,
				      struct xdp_hook_ops *elem)
{
	struct xdp_hook_entry *entry =
		container_of(elem, struct xdp_hook_entry, ops);

	list_del_rcu(&entry->ops.list);
	static_branch_dec(&xdp_hooks_needed);
	dev->xdp_hook_cnt--;

	if (elem->priv && elem->put_priv)
		elem->put_priv(elem->priv);

	if (!dev->xdp_hook_cnt && dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_DEV_FINISH;
		dev->netdev_ops->ndo_xdp(dev, &xdp_op);
	}
}

void __xdp_unregister_hook(struct net_device *dev,
			   struct list_head *hook_list,
			   const struct xdp_hook_ops *reg)
{
	struct xdp_hook_entry *entry;
	struct xdp_hook_ops *elem;

	mutex_lock(&xdp_hook_mutex);
	list_for_each_entry(elem, hook_list, list) {
		entry = container_of(elem, struct xdp_hook_entry, ops);
		if (entry->orig_ops == reg) {
			__xdp_unregister_one_hook(dev, hook_list, elem);
			break;
		}
	}
	mutex_unlock(&xdp_hook_mutex);
	if (&elem->list == hook_list) {
		WARN(1, "xdp_unregister__hook: hook not found!\n");
		return;
	}
	synchronize_net();

	kfree(entry);
}
EXPORT_SYMBOL_GPL(__xdp_unregister_hook);

static void __xdp_unregister_hooks(struct net_device *dev,
				   struct list_head *hook_list)
{
	struct xdp_hook_ops *elem, *telem;

	list_for_each_entry_safe(elem, telem, hook_list, list)
		__xdp_unregister_one_hook(dev, hook_list, elem);
}

void xdp_unregister_all_hooks(struct net_device *dev)
{
	struct napi_struct *napi;

	/* Unregister NAPI hooks for device */
	list_for_each_entry(napi, &dev->napi_list, dev_list)
		__xdp_unregister_hooks(dev, &napi->xdp_hook_list);

	/* Unregister device hooks */
	__xdp_unregister_hooks(dev, &dev->xdp_hook_list);
}
EXPORT_SYMBOL_GPL(xdp_unregister_all_hooks);

void xdp_unregister_net_hooks(struct net *net, struct xdp_hook_ops *reg)
{
	struct net_device *dev;
	struct napi_struct *napi;

	list_for_each_entry_rcu(dev, &net->dev_base_head, dev_list) {
		list_for_each_entry(napi, &dev->napi_list, dev_list)
			xdp_unregister_napi_hook(napi, reg);

		xdp_unregister_dev_hook(dev, reg);
	}
}
EXPORT_SYMBOL_GPL(xdp_unregister_net_hooks);

bool __xdp_find_hook(struct list_head *hook_list,
		  const struct xdp_hook_ops *reg,
		  struct xdp_hook_ops *ret)
{
	struct xdp_hook_entry *entry;
	struct xdp_hook_ops *elem;

	list_for_each_entry_rcu(elem, hook_list, list) {
		entry = container_of(elem, struct xdp_hook_entry, ops);
		if (entry->orig_ops == reg) {
			*ret = *elem;
			return true;
		}
	}
	return false;
}
EXPORT_SYMBOL_GPL(__xdp_find_hook);
