/*
 * netpolicy.c: Net policy support
 * Copyright (c) 2016, Intel Corporation.
 * Author: Kan Liang (kan.liang@intel.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * NET policy intends to simplify the network configuration and get a good
 * network performance according to the hints(policy) which is applied by user.
 *
 * Motivation
 * 	- The network performance is not good with default system settings.
 *	- It is too difficult to do automatic tuning for all possible
 *	  workloads, since workloads have different requirements. Some
 *	  workloads may want high throughput. Some may need low latency.
 *	- There are lots of manual configurations. Fine grained configuration
 *	  is too difficult for users.
 * 	So, it is a big challenge to get good network performance.
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>

#ifdef CONFIG_PROC_FS

static int net_policy_proc_show(struct seq_file *m, void *v)
{
	struct net_device *dev = (struct net_device *)m->private;

	seq_printf(m, "%s doesn't support net policy manager\n", dev->name);

	return 0;
}

static int net_policy_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, net_policy_proc_show, PDE_DATA(inode));
}

static const struct file_operations proc_net_policy_operations = {
	.open		= net_policy_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
	.owner		= THIS_MODULE,
};

static int netpolicy_proc_dev_init(struct net *net, struct net_device *dev)
{
	dev->proc_dev = proc_net_mkdir(net, dev->name, net->proc_netpolicy);
	if (!dev->proc_dev)
		return -ENOMEM;

	if (!proc_create_data("policy", S_IWUSR | S_IRUGO,
			      dev->proc_dev, &proc_net_policy_operations,
			      (void *)dev)) {
		remove_proc_subtree(dev->name, net->proc_netpolicy);
		return -ENOMEM;
	}
	return 0;
}

static int __net_init netpolicy_net_init(struct net *net)
{
	struct net_device *dev, *aux;

	net->proc_netpolicy = proc_net_mkdir(net, "netpolicy",
					     net->proc_net);
	if (!net->proc_netpolicy)
		return -ENOMEM;

	for_each_netdev_safe(net, dev, aux) {
		netpolicy_proc_dev_init(net, dev);
	}

	return 0;
}

#else /* CONFIG_PROC_FS */

static int __net_init netpolicy_net_init(struct net *net)
{
	return 0;
}
#endif /* CONFIG_PROC_FS */

static void __net_exit netpolicy_net_exit(struct net *net)
{
#ifdef CONFIG_PROC_FS
	remove_proc_subtree("netpolicy", net->proc_net);
#endif /* CONFIG_PROC_FS */
}

static struct pernet_operations netpolicy_net_ops = {
	.init = netpolicy_net_init,
	.exit = netpolicy_net_exit,
};

static int __init netpolicy_init(void)
{
	int ret;

	ret = register_pernet_subsys(&netpolicy_net_ops);

	return ret;
}

static void __exit netpolicy_exit(void)
{
	unregister_pernet_subsys(&netpolicy_net_ops);
}

subsys_initcall(netpolicy_init);
module_exit(netpolicy_exit);
