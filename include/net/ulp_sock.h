/*
 * Pluggable upper layer protocol support in sockets.
 *
 * Copyright (c) 2016-2017, Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016-2017, Dave Watson <davejwatson@fb.com>. All rights reserved.
 * Copyright (c) 2017, Tom Herbert <tom@quantonium.net>. All rights reserved.
 *
 */

#ifndef __NET_ULP_SOCK_H
#define __NET_ULP_SOCK_H

#include <linux/socket.h>

#define ULP_MAX             128
#define ULP_BUF_MAX         (ULP_NAME_MAX * ULP_MAX)

struct ulp_ops {
	struct list_head list;

	/* initialize ulp */
	int (*init)(struct sock *sk, char __user *optval, int len);

	/* cleanup ulp */
	void (*release)(struct sock *sk);

	/* Get ULP specific parameters in getsockopt */
	int (*get_params)(struct sock *sk, char __user *optval, int *optlen);

	char name[ULP_NAME_MAX];
	struct module *owner;
};

#ifdef CONFIG_ULP_SOCK

int ulp_register(struct ulp_ops *type);
void ulp_unregister(struct ulp_ops *type);
int ulp_set(struct sock *sk, char __user *optval, int len);
int ulp_get_config(struct sock *sk, char __user *optval, int *optlen);
void ulp_get_available(char *buf, size_t len);
void ulp_cleanup(struct sock *sk);

#else

static inline int ulp_register(struct ulp_ops *type)
{
	return -EOPNOTSUPP;
}

static inline void ulp_unregister(struct ulp_ops *type)
{
}

static inline int ulp_set(struct sock *sk, char __user *optval, int len)
{
	return -EOPNOTSUPP;
}

static inline int ulp_get_config(struct sock *sk, char __user *optval,
				 int *optlen)
{
	return -EOPNOTSUPP;
}

static inline void ulp_get_available(char *buf, size_t len)
{
}

static inline void ulp_cleanup(struct sock *sk)
{
}

#endif

#endif /* __NET_ULP_SOCK_H */
