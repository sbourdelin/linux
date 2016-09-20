/*
 * eXpress Data Path (XDP)
 *
 * Copyright (c) 2016 Tom Herbert <tom@herbertland.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef __NET_XDP_H_
#define __NET_XDP_H_

#include <linux/netdevice.h>
#include <linux/static_key.h>
#include <uapi/linux/xdp.h>

/* XDP data structure.
 *
 * Fields:
 *   data - pointer to first byte of data
 *   data_end - pointer to last byte
 *
 * Length is deduced by xdp->data_end - xdp->data.
 */
struct xdp_buff {
	void *data;
	void *data_end;
};

typedef unsigned int xdp_hookfn(const void *priv, struct xdp_buff *xdp);
typedef void xdp_put_privfn(const void *priv);

/* xdp_hook_ops struct
 *
 * This structure contains the ops of an XDP hook. A pointer to this structure
 * is passed into the XDP register function to set up a hook. The XDP
 * register function mallocs its own xdp_hook_ops structure and copies the
 * values from the xdp_hook_ops passed in. The register function also stores
 * the pointer value of the xdp_hook_ops argument; this pointer is used
 * in subsequently calls to XDP to identify the registered hook.
 *
 * Fields:
 *
 *   list - list glue
 *   priority - priority for insertion into list. List is ordered lowest to
 *	greatest priority.
 *   priv - private data associated with hook. This is passed as an argument
 *	to the hook function
 *   hook - function to call when hooks are run
 *   put_priv - function call when XDP is done with private data
 */
struct xdp_hook_ops {
	struct list_head list;
	int priority;
	void __rcu *priv;
	xdp_hookfn *hook;
	xdp_put_privfn *put_priv;
};

struct xdp_hook_entry {
	const struct xdp_hook_ops *orig_ops;
	struct xdp_hook_ops ops;
};

extern struct xdp_hook_ops xdp_bpf_hook_ops;

extern struct static_key_false xdp_hooks_needed;

/* Check if XDP hooks are set for a napi or its device */
static inline bool xdp_hook_run_needed_check(struct napi_struct *napi)
{
	return (static_branch_unlikely(&xdp_hooks_needed) &&
		(!(list_empty(&napi->dev->xdp_hook_list) &&
		   list_empty(&napi->xdp_hook_list))));
}

static inline int __xdp_hook_run(struct list_head *list_head,
				 struct xdp_buff *xdp)
{
	struct xdp_hook_ops *elem;
	int ret = XDP_PASS;

	list_for_each_entry(elem, list_head, list) {
		ret = elem->hook(elem->priv, xdp);
		if (ret != XDP_PASS)
			break;
	}

	return ret;
}

/* Run the XDP hooks for a napi device. Called from a driver's receive
 * routine
 */
static inline int xdp_hook_run(struct napi_struct *napi, struct xdp_buff *xdp)
{
	struct net_device *dev = napi->dev;
	int ret = XDP_PASS;

	if (static_branch_unlikely(&xdp_hooks_needed)) {
		/* Run hooks in napi first */
		ret = __xdp_hook_run(&napi->xdp_hook_list, xdp);
		if (ret != XDP_PASS)
			return ret;

		/* Now run device hooks */
		ret = __xdp_hook_run(&dev->xdp_hook_list, xdp);
		if (ret != XDP_PASS)
			return ret;
	}

	return ret;
}

int __xdp_register_hook(struct net_device *dev,
			struct list_head *list,
			const struct xdp_hook_ops *reg,
			bool change);

/* Register an XDP hook and a device */
static inline int xdp_register_dev_hook(struct net_device *dev,
					const struct xdp_hook_ops *reg)
{
	return __xdp_register_hook(dev, &dev->xdp_hook_list, reg, false);
}

/* Register an XDP hook and a napi instance */
static inline int xdp_register_napi_hook(struct napi_struct *napi,
					 const struct xdp_hook_ops *reg)
{
	return __xdp_register_hook(napi->dev, &napi->xdp_hook_list, reg, false);
}

/* Change an XDP hook.
 *
 *    - If the hook does not exist (xdp_hook_ops does not match a hook set on
 *      the device), then attempt to register the hook.
 *    - Else, change the private data (priv field in xdp_hook_ops) in the
 *      existing hook to be the new one (in reg). All the other fields in
 *      xdp_hook_ops are ignored in that case.
 */

/* Change a device XDP hook */
static inline int xdp_change_dev_hook(struct net_device *dev,
				      const struct xdp_hook_ops *reg)
{
	return __xdp_register_hook(dev, &dev->xdp_hook_list, reg, true);
}

/* Change a napi XDP hook */
static inline int xdp_change_napi_hook(struct napi_struct *napi,
				       const struct xdp_hook_ops *reg)
{
	return __xdp_register_hook(napi->dev, &napi->xdp_hook_list, reg, true);
}

void __xdp_unregister_hook(struct net_device *dev,
			   struct list_head *list,
			   const struct xdp_hook_ops *reg);

/* Unregister device XDP hook */
static inline void xdp_unregister_dev_hook(struct net_device *dev,
					   const struct xdp_hook_ops *reg)
{
	return __xdp_unregister_hook(dev, &dev->xdp_hook_list, reg);
}

/* Unregister a napi XDP hook */
static inline void xdp_unregister_napi_hook(struct napi_struct *napi,
					    const struct xdp_hook_ops *reg)
{
	return __xdp_unregister_hook(napi->dev, &napi->xdp_hook_list, reg);
}

/* Unregister all XDP hooks associated with a device (both the device hooks
 * and hooks on all napi instances. This function is called when the netdev
 * is being freed.
 */
void xdp_unregister_all_hooks(struct net_device *dev);

/* Unregister all XDP hooks for a given xdp_hook_ops in a net. This walks
 * all devices in net and napis for each device to unregister matching hooks.
 * This can be called when a module that had registered some number of hooks
 * is being unloaded.
 */
void xdp_unregister_net_hooks(struct net *net, struct xdp_hook_ops *reg);

/* Find a registered device hook.
 *   - If hook is found *ret is set to the values in the registered hook and
 *     true is returned.
 *   - Else false is returned.
 */
bool __xdp_find_hook(struct list_head *list, const struct xdp_hook_ops *reg,
		     struct xdp_hook_ops *ret);

/* Find a device XDP hook */
static inline bool xdp_find_dev_hook(struct net_device *dev,
				     const struct xdp_hook_ops *reg,
				     struct xdp_hook_ops *ret)
{
	return __xdp_find_hook(&dev->xdp_hook_list, reg, ret);
}

/* Find a napi XDP hook */
static inline bool xdp_find_napi_hook(struct napi_struct *napi,
				      const struct xdp_hook_ops *reg,
				      struct xdp_hook_ops *ret)
{
	return __xdp_find_hook(&napi->xdp_hook_list, reg, ret);
}

static inline void xdp_warn_invalid_action(u32 act)
{
	WARN_ONCE(1, "Illegal XDP return value %u, expect packet loss\n", act);
}

#endif /* __NET_XDP_H_ */
