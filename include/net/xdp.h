/*
 * eXpress Data Path (XDP)
 *
 * Copyright (c) 2017 Tom Herbert <tom@herbertland.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef __NET_XDP_H_
#define __NET_XDP_H_

#include <linux/filter.h>
#include <linux/netdevice.h>
#include <linux/static_key.h>

/* XDP data structure.
 *
 * Fields:
 *   data - pointer to first byte of data
 *   data_end - pointer to last byte
 *   data_hard_start - point to first possible byte
 *
 * Length is deduced by xdp->data_end - xdp->data.
 */
struct xdp_buff {
	void *data;
	void *data_end;
	void *data_hard_start;
};

typedef unsigned int xdp_hookfn(const void *priv, struct xdp_buff *xdp);
typedef void xdp_put_privfn(const void *priv);

#define XDP_TAG_SIZE	8 /* Should be at least BPF_TAG_SIZE */

/* xdp_hook struct
 *
 * This structure contains the ops and data for an XDP hook. A pointer
 * to this structure providing the definition of a hook is passed into
 * the XDP register function to set up a hook. The XDP register function
 * mallocs its own xdp_hook structure and copies the values from the
 * xdp_hook definition. The register function also saves the pointer value
 * of the xdp_hook definition argument; this pointer is used in subsequent
 * calls to XDP to find or unregister the hook.
 *
 * Fields:
 *
 *   priority - priority for insertion into set. The set is ordered lowest to
 *	highest priority.
 *   priv - private data associated with hook. This is passed as an argument
 *	to the hook function. This is a bpf_prog structure.
 *   put_priv - function call when XDP is done with private data.
 *   def - point to definitions of xdp_hook. The pointer value is saved as
 *      a refernce the instance of hook loaded (used to find and unregister a
 *      hook).
 *   tag - readable tag for reporting purposes
 */
struct xdp_hook {
	int priority;
	void __rcu *priv;
	const struct xdp_hook *def;
	u8 tag[XDP_TAG_SIZE];
};

/* xdp_hook_set
 *
 * This structure holds a set of XDP hooks in an array of size num. This
 * structure is used in netdevice to refer to the XDP hooks for a whole
 * device or in the napi structure to contain the hooks for an individual
 * RX queue.
 */
struct xdp_hook_set {
	unsigned int num;
	struct rcu_head rcu;
	struct xdp_hook hooks[0];
};

#define XDP_SET_SIZE(_num) (sizeof(struct xdp_hook_set) + ((_num) * \
	sizeof(struct xdp_hook)))

extern struct xdp_hook xdp_bpf_hook;

extern struct static_key_false xdp_napi_hooks_needed;
extern struct static_key_false xdp_dev_hooks_needed;

/* Check if XDP hooks are set for a napi or its device */
static inline bool xdp_hook_run_needed_check(struct net_device *dev,
					     struct napi_struct *napi)
{
	return ((static_branch_unlikely(&xdp_dev_hooks_needed) &&
		dev->xdp_hooks) ||
		(static_branch_unlikely(&xdp_napi_hooks_needed) &&
		 napi->xdp_hooks));
}

static inline int __xdp_run_one_hook(struct xdp_hook *hook,
				     struct xdp_buff *xdp)
{
	void *priv = rcu_dereference(hook->priv);

	return BPF_PROG_RUN((struct bpf_prog *)priv, (void *)xdp);
}

/* Core function to run the XDP hooks. This must be as fast as possible */
static inline int __xdp_hook_run(struct xdp_hook_set *hook_set,
				 struct xdp_buff *xdp,
				 struct xdp_hook **last_hook)
{
	struct xdp_hook *hook;
	int i, ret;

	if (unlikely(!hook_set))
		return XDP_PASS;

	hook = &hook_set->hooks[0];
	ret = __xdp_run_one_hook(hook, xdp);
	*last_hook = hook;

	for (i = 1; i < hook_set->num; i++) {
		if (ret != XDP_PASS)
			break;
		hook = &hook_set->hooks[i];
		ret = __xdp_run_one_hook(hook, xdp);
		*last_hook = hook;
	}

	return ret;
}

/* Run the XDP hooks for a napi device and return a reference to the last
 * hook processed. Called from a driver's receive routine. RCU
 * read lock must be held.
 */
static inline int xdp_hook_run_ret_last(struct napi_struct *napi,
					struct xdp_buff *xdp,
					struct xdp_hook **last_hook)
{
	struct net_device *dev = napi->dev;
	struct xdp_hook_set *hook_set;
	int ret = XDP_PASS;

	if (static_branch_unlikely(&xdp_napi_hooks_needed)) {
		/* Run hooks in napi first */
		hook_set = rcu_dereference(napi->xdp_hooks);
		ret = __xdp_hook_run(hook_set, xdp, last_hook);

		/* Check for dev hooks now taking into account that
		 * we need to check for XDP_PASS having been
		 * returned only if they are need (this is why
		 * we don't do a fall through).
		 */
		if (static_branch_unlikely(&xdp_dev_hooks_needed)) {
			if (ret != XDP_PASS)
				return ret;
			hook_set = rcu_dereference(dev->xdp_hooks);
			ret = __xdp_hook_run(hook_set, xdp, last_hook);
		}
	} else if (static_branch_unlikely(&xdp_dev_hooks_needed)) {
		/* Now run device hooks */
		hook_set = rcu_dereference(dev->xdp_hooks);
		ret = __xdp_hook_run(hook_set, xdp, last_hook);
	}

	return ret;
}

/* Run the XDP hooks for a napi device. Called from a driver's receive
 * routine. RCU read lock must be held.
 */
static inline int xdp_hook_run(struct napi_struct *napi,
			       struct xdp_buff *xdp)
{
	struct xdp_hook *last_hook;

	return xdp_hook_run_ret_last(napi, xdp, &last_hook);
}

/* Register an XDP hook
 *    dev: Assoicated net_device
 *    hook_set: Hook set
 *    def: Definition of the hook. The values are copied from this to a
 *	   malloc'ed structure. The base_def pointer is saved as a
 *	   reference to the hook to manage it
 *    change: Change hook if it exists
 *    dev_hook: Is a hook on a net_device (as oppsed to a napi instance)
 */
int __xdp_register_hook(struct net_device *dev,
			struct xdp_hook_set __rcu **hook_set,
			const struct xdp_hook *base_def,
			bool change, bool dev_hook);

/* Register an XDP hook on a device */
static inline int xdp_register_dev_hook(struct net_device *dev,
					const struct xdp_hook *def)
{
	return __xdp_register_hook(dev, &dev->xdp_hooks, def, false, true);
}

/* Register an XDP hook on a napi instance */
static inline int xdp_register_napi_hook(struct napi_struct *napi,
					 const struct xdp_hook *def)
{
	return __xdp_register_hook(napi->dev, &napi->xdp_hooks, def, false,
				   false);
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
				      const struct xdp_hook *reg)
{
	return __xdp_register_hook(dev, &dev->xdp_hooks, reg, true, true);
}

/* Change a napi XDP hook */
static inline int xdp_change_napi_hook(struct napi_struct *napi,
				       const struct xdp_hook *reg)
{
	return __xdp_register_hook(napi->dev, &napi->xdp_hooks, reg, true,
				   false);
}

int __xdp_unregister_hook(struct net_device *dev,
			  struct xdp_hook_set __rcu **hook_set,
			  const struct xdp_hook *def, bool dev_hook);

/* Unregister device XDP hook */
static inline int xdp_unregister_dev_hook(struct net_device *dev,
					   const struct xdp_hook *def)
{
	return __xdp_unregister_hook(dev, &dev->xdp_hooks, def, true);
}

/* Unregister a napi XDP hook */
static inline int xdp_unregister_napi_hook(struct napi_struct *napi,
					    const struct xdp_hook *def)
{
	return __xdp_unregister_hook(napi->dev, &napi->xdp_hooks, def, false);
}

/* Unregister all XDP hooks associated with a device (both the device hooks
 * and hooks on all napi instances). This function is called when the netdev
 * is being freed.
 */
void xdp_unregister_all_hooks(struct net_device *dev);

/* Unregister all XDP hooks for a given xdp_hook_ops in a net. This walks
 * all devices in net and napis for each device to unregister matching hooks.
 * This can be called when a module that had registered some number of hooks
 * is being unloaded.
 */
void xdp_unregister_net_hooks(struct net *net, struct xdp_hook *def);

/* Find a registered device hook.
 *   - If hook is found *ret is set to the values in the registered hook and
 *     true is returned.
 *   - Else false is returned.
 */
bool __xdp_find_hook(struct xdp_hook_set **hook_set,
		     const struct xdp_hook *def,
		     struct xdp_hook *ret);

/* Find a device XDP hook. */
static inline bool xdp_find_dev_hook(struct net_device *dev,
				     const struct xdp_hook *def,
				     struct xdp_hook *ret)
{
	return __xdp_find_hook(&dev->xdp_hooks, def, ret);
}

/* Find a napi XDP hook. */
static inline bool xdp_find_napi_hook(struct napi_struct *napi,
				      const struct xdp_hook *def,
				      struct xdp_hook *ret)
{
	return __xdp_find_hook(&napi->xdp_hooks, def, ret);
}

int xdp_bpf_check_prog(struct net_device *dev, struct bpf_prog *prog);

static inline void xdp_warn_invalid_action(u32 act)
{
	WARN_ONCE(1, "Illegal XDP return value %u, expect packet loss\n", act);
}

#endif /* __NET_XDP_H_ */
