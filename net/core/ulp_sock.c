/*
 * Pluggable upper layer protocol support in sockets.
 *
 * Copyright (c) 2016-2017, Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016-2017, Dave Watson <davejwatson@fb.com>. All rights reserved.
 * Copyright (c) 2017, Tom Herbert <tom@quantonium.net>. All rights reserved.
 *
 */

#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/ulp_sock.h>

static DEFINE_SPINLOCK(ulp_list_lock);
static LIST_HEAD(ulp_list);

/* Simple linear search, don't expect many entries! */
static struct ulp_ops *ulp_find(const char *name)
{
	struct ulp_ops *e;

	list_for_each_entry_rcu(e, &ulp_list, list) {
		if (strcmp(e->name, name) == 0)
			return e;
	}

	return NULL;
}

static const struct ulp_ops *__ulp_find_autoload(const char *name)
{
	const struct ulp_ops *ulp = NULL;

	rcu_read_lock();
	ulp = ulp_find(name);

#ifdef CONFIG_MODULES
	if (!ulp && capable(CAP_NET_ADMIN)) {
		rcu_read_unlock();
		request_module("%s", name);
		rcu_read_lock();
		ulp = ulp_find(name);
	}
#endif
	if (!ulp || !try_module_get(ulp->owner))
		ulp = NULL;

	rcu_read_unlock();
	return ulp;
}

/* Attach new upper layer protocol to the list
 * of available protocols.
 */
int ulp_register(struct ulp_ops *ulp)
{
	int ret = 0;

	spin_lock(&ulp_list_lock);
	if (ulp_find(ulp->name)) {
		pr_notice("%s already registered or non-unique name\n",
			  ulp->name);
		ret = -EEXIST;
	} else {
		list_add_tail_rcu(&ulp->list, &ulp_list);
	}
	spin_unlock(&ulp_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ulp_register);

void ulp_unregister(struct ulp_ops *ulp)
{
	spin_lock(&ulp_list_lock);
	list_del_rcu(&ulp->list);
	spin_unlock(&ulp_list_lock);

	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(ulp_unregister);

/* Build string with list of available upper layer protocl values */
void ulp_get_available(char *buf, size_t maxlen)
{
	struct ulp_ops *ulp_ops;
	size_t offs = 0;

	*buf = '\0';
	rcu_read_lock();
	list_for_each_entry_rcu(ulp_ops, &ulp_list, list) {
		offs += snprintf(buf + offs, maxlen - offs,
				 "%s%s",
				 offs == 0 ? "" : " ", ulp_ops->name);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(ulp_get_available);

void ulp_cleanup(struct sock *sk)
{
	if (!sk->sk_ulp_ops)
		return;

	if (sk->sk_ulp_ops->release)
		sk->sk_ulp_ops->release(sk);

	module_put(sk->sk_ulp_ops->owner);
}
EXPORT_SYMBOL_GPL(ulp_cleanup);

/* Change upper layer protocol for socket, Called from setsockopt. */
int ulp_set(struct sock *sk, char __user *optval, int len)
{
	const struct ulp_ops *ulp_ops;
	struct ulp_config ulpc;
	int err = 0;

	if (len < sizeof(ulpc))
		return -EINVAL;

	if (copy_from_user(&ulpc, optval, sizeof(ulpc)))
		return -EFAULT;

	if (sk->sk_ulp_ops)
		return -EEXIST;

	ulp_ops = __ulp_find_autoload(ulpc.ulp_name);
	if (!ulp_ops)
		return -ENOENT;

	optval += sizeof(ulpc);
	len -= sizeof(ulpc);

	err = ulp_ops->init(sk, optval, len);
	if (err)
		return err;

	sk->sk_ulp_ops = ulp_ops;

	return 0;
}
EXPORT_SYMBOL_GPL(ulp_set);

/* Get upper layer protocol for socket. Called from getsockopt. */
int ulp_get_config(struct sock *sk, char __user *optval, int *optlen)
{
	struct ulp_config ulpc;
	int len = *optlen;
	int used_len;
	int ret;

	if (get_user(len, optlen))
		return -EFAULT;

	if (len < sizeof(ulpc))
		return -EINVAL;

	if (!sk->sk_ulp_ops) {
		if (put_user(0, optlen))
			return -EFAULT;
		return 0;
	}

	memcpy(ulpc.ulp_name, sk->sk_ulp_ops->name,
	       sizeof(ulpc.ulp_name));

	if (copy_to_user(optval, &ulpc, sizeof(ulpc)))
		return -EFAULT;

	used_len = sizeof(ulpc);

	if (sk->sk_ulp_ops->get_params) {
		len -= sizeof(ulpc);
		optval += sizeof(ulpc);

		ret = sk->sk_ulp_ops->get_params(sk, optval, &len);
		if (ret)
			return ret;

		used_len += len;
	}

	if (put_user(used_len, optlen))
		return -EFAULT;

	return 0;
}
EXPORT_SYMBOL_GPL(ulp_get_config);

