/*
 * eXpress Data Path
 *
 * Copyright (c) 2017 Tom Herbert <tom@herbertland.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */
#include <linux/bpf.h>
#include <net/xdp.h>

DEFINE_STATIC_KEY_FALSE(xdp_dev_hooks_needed);
EXPORT_SYMBOL(xdp_dev_hooks_needed);

DEFINE_STATIC_KEY_FALSE(xdp_napi_hooks_needed);
EXPORT_SYMBOL(xdp_napi_hooks_needed);

static DEFINE_MUTEX(xdp_hook_mutex);

int __xdp_register_hook(struct net_device *dev,
			struct xdp_hook_set __rcu **xdp_hooks,
			const struct xdp_hook *def,
			bool change, bool dev_hook)
{
	struct xdp_hook_set *new_hooks = NULL, *old_hooks;
	struct xdp_hook *hook;
	int index, targindex = 0;
	int i, err;

	mutex_lock(&xdp_hook_mutex);

	old_hooks = rcu_dereference(*xdp_hooks);

	if (old_hooks) {
		/* Walk over hooks, see if hook is already registered and
		 * determine insertion point.
		 */

		for (index = 0; index < old_hooks->num; index++) {
			hook = &old_hooks->hooks[index];
			if (hook->def != def) {
				if (def->priority < hook->priority)
					targindex = index;
				continue;
			}

			if (change) {
				void *old_priv;

				/* Only allow changing priv field in an existing
				 * hook.
				 */
				old_priv = rcu_dereference_protected(hook->priv,
					lockdep_is_held(&xdp_hook_mutex));
				rcu_assign_pointer(hook->priv, def->priv);
				if (old_priv)
					bpf_prog_put((struct bpf_prog *)old_priv);
				goto out;
			} else {
				/* Already registered */
				err = -EALREADY;
				goto err;
			}
		}
	}

	/* Need to add new hook set. index holds number of entries in hooks
	 * set (zero if hooks set is NULL). targindex holds index to insert
	 * new hook.
	 */
	new_hooks = kzalloc(XDP_SET_SIZE(index + 1), GFP_KERNEL);
	if (!new_hooks) {
		err = -ENOMEM;
		goto err;
	}

	/* Initialize XDP in driver */
	if (!dev->xdp_hook_cnt && dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_MODE_ON;
		err = dev->netdev_ops->ndo_xdp(dev, &xdp_op);
		if (err)
			goto err;
	}

	if (old_hooks) {
		for (i = 0; i < targindex; i++)
			new_hooks->hooks[i] = old_hooks->hooks[i];

		for (i++; i < index + 1; i++)
			new_hooks->hooks[i] = old_hooks->hooks[i - 1];
	}

	new_hooks->hooks[targindex] = *def;
	rcu_assign_pointer(new_hooks->hooks[targindex].priv, def->priv);
	new_hooks->num = index + 1;
	rcu_assign_pointer(*xdp_hooks, new_hooks);

	if (old_hooks)
		kfree_rcu(old_hooks, rcu);

	if (dev_hook)
		static_branch_inc(&xdp_dev_hooks_needed);
	else
		static_branch_inc(&xdp_napi_hooks_needed);

	dev->xdp_hook_cnt++;

out:
	mutex_unlock(&xdp_hook_mutex);

	return 0;

err:
	mutex_unlock(&xdp_hook_mutex);
	kfree(new_hooks);
	return err;
}
EXPORT_SYMBOL_GPL(__xdp_register_hook);

int __xdp_unregister_hook(struct net_device *dev,
			  struct xdp_hook_set __rcu **xdp_hooks,
			  const struct xdp_hook *def,
			  bool dev_hook)
{
	struct xdp_hook_set *old_hooks, *new_hooks = NULL;
	struct xdp_hook *hook;
	int i, index;
	int err = 0;

	old_hooks = rcu_dereference(*xdp_hooks);

	mutex_lock(&xdp_hook_mutex);

	for (index = 0; index < old_hooks->num; index++) {
		hook = &old_hooks->hooks[index];
		if (hook->def != def)
			continue;

		if (old_hooks->num > 1) {
			new_hooks = kzalloc(XDP_SET_SIZE(
				old_hooks->num  - 1), GFP_KERNEL);

			if (!new_hooks) {
				err = -ENOMEM;
				goto out;
			}
			for (i = 0; i < index; i++)
				new_hooks->hooks[i] = old_hooks->hooks[i];
			for (i++; i < index; i++)
				new_hooks->hooks[i - 1] = old_hooks->hooks[i];

			new_hooks->num = old_hooks->num - 1;
		}

		break;
	}

	if (index >= old_hooks->num)
		goto out;

	rcu_assign_pointer(*xdp_hooks, new_hooks);

	if (old_hooks)
		kfree_rcu(old_hooks, rcu);

	dev->xdp_hook_cnt--;

	if (dev_hook)
		static_branch_dec(&xdp_dev_hooks_needed);
	else
		static_branch_dec(&xdp_napi_hooks_needed);

	if (hook->priv)
		bpf_prog_put((struct bpf_prog *)hook->priv);

	if (!dev->xdp_hook_cnt && dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_MODE_OFF;
		dev->netdev_ops->ndo_xdp(dev, &xdp_op);
	}

out:
	mutex_unlock(&xdp_hook_mutex);
	synchronize_net();

	return err;
}
EXPORT_SYMBOL_GPL(__xdp_unregister_hook);

static void __xdp_unregister_hooks(struct net_device *dev,
				   struct xdp_hook_set __rcu **xdp_hooks,
				   bool dev_hook)
{
	struct xdp_hook_set *old_hooks;
	int i;

	mutex_lock(&xdp_hook_mutex);

	old_hooks = rcu_dereference(*xdp_hooks);

	if (!old_hooks) {
		mutex_unlock(&xdp_hook_mutex);
		return;
	}

	for (i = 0; i < old_hooks->num; i++) {
		if (dev_hook)
			static_branch_dec(&xdp_dev_hooks_needed);
		else
			static_branch_dec(&xdp_napi_hooks_needed);
		dev->xdp_hook_cnt--;
	}

	rcu_assign_pointer(*xdp_hooks, NULL);

	if (!dev->xdp_hook_cnt && dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_MODE_OFF;
		dev->netdev_ops->ndo_xdp(dev, &xdp_op);
	}

	mutex_unlock(&xdp_hook_mutex);

	kfree_rcu(old_hooks, rcu);
}

void xdp_unregister_all_hooks(struct net_device *dev)
{
	struct napi_struct *napi;

	/* Unregister NAPI hooks for device */
	list_for_each_entry(napi, &dev->napi_list, dev_list)
		__xdp_unregister_hooks(dev, &napi->xdp_hooks, false);

	/* Unregister device hooks */
	__xdp_unregister_hooks(dev, &dev->xdp_hooks, true);
}
EXPORT_SYMBOL_GPL(xdp_unregister_all_hooks);

void xdp_unregister_net_hooks(struct net *net, struct xdp_hook *def)
{
	struct net_device *dev;
	struct napi_struct *napi;

	list_for_each_entry_rcu(dev, &net->dev_base_head, dev_list) {
		list_for_each_entry(napi, &dev->napi_list, dev_list)
			xdp_unregister_napi_hook(napi, def);

		xdp_unregister_dev_hook(dev, def);
	}
}
EXPORT_SYMBOL_GPL(xdp_unregister_net_hooks);

bool __xdp_find_hook(struct xdp_hook_set __rcu **xdp_hooks,
		     const struct xdp_hook *def,
		     struct xdp_hook *ret)
{
	struct xdp_hook_set *old_hooks;
	struct xdp_hook *hook;
	bool retval = false;
	int index;

	rcu_read_lock();

	old_hooks = rcu_dereference(*xdp_hooks);

	if (!old_hooks)
		goto out;

	for (index = 0; index < old_hooks->num; index++) {
		hook = &old_hooks->hooks[index];
		if (hook->def != def)
			continue;

		if (ret)
			*ret = *hook;
		retval = true;
		goto out;
	}

out:
	rcu_read_unlock();

	return retval;
}
EXPORT_SYMBOL_GPL(__xdp_find_hook);

int xdp_bpf_check_prog(struct net_device *dev, struct bpf_prog *prog)
{
	if (dev->netdev_ops->ndo_xdp) {
		struct netdev_xdp xdp_op = {};

		xdp_op.command = XDP_CHECK_BPF_PROG;
		xdp_op.prog = prog;

		return dev->netdev_ops->ndo_xdp(dev, &xdp_op);
	} else {
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(xdp_bpf_check_prog);
