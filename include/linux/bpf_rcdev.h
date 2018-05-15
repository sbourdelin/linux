/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_RCDEV_H
#define _BPF_RCDEV_H

#include <linux/bpf.h>
#include <uapi/linux/bpf.h>

#ifdef CONFIG_BPF_RAWIR_EVENT
int rc_dev_prog_attach(const union bpf_attr *attr);
int rc_dev_prog_detach(const union bpf_attr *attr);
int rc_dev_prog_query(const union bpf_attr *attr, union bpf_attr __user *uattr);
#else
static inline int rc_dev_prog_attach(const union bpf_attr *attr)
{
	return -EINVAL;
}

static inline int rc_dev_prog_detach(const union bpf_attr *attr)
{
	return -EINVAL;
}

static inline int rc_dev_prog_query(const union bpf_attr *attr,
				    union bpf_attr __user *uattr)
{
	return -EINVAL;
}
#endif

#endif /* _BPF_RCDEV_H */
