// SPDX-License-Identifier: GPL-2.0
#include <linux/uaccess.h>
#include <linux/bpfilter.h>
#include <uapi/linux/bpf.h>
#include <linux/wait.h>
#include <linux/kmod.h>
#include <linux/module.h>

struct bpfilter_umh_ops bpfilter_ops;
EXPORT_SYMBOL_GPL(bpfilter_ops);

static int bpfilter_mbox_request(struct sock *sk, int optname,
				 char __user *optval,
				 unsigned int optlen, bool is_set)
{
	int err;

	mutex_lock(&bpfilter_ops.mutex);
	if (!bpfilter_ops.process_sockopt) {
		err = request_module("bpfilter");
		if (err)
			goto unlock;

		if (!bpfilter_ops.process_sockopt) {
			if (!bpfilter_ops.start_umh ||
			    bpfilter_ops.start_umh()) {
				err = -ECHILD;
				goto unlock;
			}
		}
	}
	err = bpfilter_ops.process_sockopt(sk, optname, optval, optlen, is_set);
unlock:
	mutex_unlock(&bpfilter_ops.mutex);
	return err;
}

int bpfilter_ip_set_sockopt(struct sock *sk, int optname, char __user *optval,
			    unsigned int optlen)
{
	return bpfilter_mbox_request(sk, optname, optval, optlen, true);
}

int bpfilter_ip_get_sockopt(struct sock *sk, int optname, char __user *optval,
			    int __user *optlen)
{
	int len;

	if (get_user(len, optlen))
		return -EFAULT;

	return bpfilter_mbox_request(sk, optname, optval, len, false);
}

static int __init init_bpfilter_sockopt(void)
{
	mutex_init(&bpfilter_ops.mutex);
	return 0;
}

module_init(init_bpfilter_sockopt);
