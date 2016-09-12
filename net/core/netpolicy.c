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
#include <net/rtnetlink.h>

static int netpolicy_get_dev_info(struct net_device *dev,
				  struct netpolicy_dev_info *d_info)
{
	if (!dev->netdev_ops->ndo_get_irq_info)
		return -ENOTSUPP;
	return dev->netdev_ops->ndo_get_irq_info(dev, d_info);
}

static void netpolicy_free_dev_info(struct netpolicy_dev_info *d_info)
{
	kfree(d_info->rx_irq);
	kfree(d_info->tx_irq);
}

const char *policy_name[NET_POLICY_MAX] = {
	"NONE"
};
#ifdef CONFIG_PROC_FS

static int net_policy_proc_show(struct seq_file *m, void *v)
{
	struct net_device *dev = (struct net_device *)m->private;
	int i;

	if (WARN_ON(!dev->netpolicy))
		return -EINVAL;

	if (dev->netpolicy->cur_policy == NET_POLICY_NONE) {
		seq_printf(m, "%s: There is no policy applied\n", dev->name);
		seq_printf(m, "%s: The available policy include:", dev->name);
		for_each_set_bit(i, dev->netpolicy->avail_policy, NET_POLICY_MAX)
			seq_printf(m, " %s", policy_name[i]);
		seq_printf(m, "\n");
	} else {
		seq_printf(m, "%s: POLICY %s is running on the system\n",
			   dev->name, policy_name[dev->netpolicy->cur_policy]);
	}

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
#endif /* CONFIG_PROC_FS */

int init_netpolicy(struct net_device *dev)
{
	int ret;

	spin_lock(&dev->np_lock);
	ret = 0;

	if (!dev->netdev_ops->ndo_netpolicy_init) {
		ret = -ENOTSUPP;
		goto unlock;
	}

	if (dev->netpolicy)
		goto unlock;

	dev->netpolicy = kzalloc(sizeof(*dev->netpolicy), GFP_ATOMIC);
	if (!dev->netpolicy) {
		ret = -ENOMEM;
		goto unlock;
	}

	ret = dev->netdev_ops->ndo_netpolicy_init(dev, dev->netpolicy);
	if (ret) {
		kfree(dev->netpolicy);
		dev->netpolicy = NULL;
	}

unlock:
	spin_unlock(&dev->np_lock);
	return ret;
}

void uninit_netpolicy(struct net_device *dev)
{
	spin_lock(&dev->np_lock);
	if (dev->netpolicy) {
		kfree(dev->netpolicy);
		dev->netpolicy = NULL;
	}
	spin_unlock(&dev->np_lock);
}

static int __net_init netpolicy_net_init(struct net *net)
{
	struct net_device *dev, *aux;

#ifdef CONFIG_PROC_FS
	net->proc_netpolicy = proc_net_mkdir(net, "netpolicy",
					     net->proc_net);
	if (!net->proc_netpolicy)
		return -ENOMEM;
#endif /* CONFIG_PROC_FS */

	rtnl_lock();
	for_each_netdev_safe(net, dev, aux) {
		if (!init_netpolicy(dev)) {
#ifdef CONFIG_PROC_FS
			if (netpolicy_proc_dev_init(net, dev))
				uninit_netpolicy(dev);
			else
#endif /* CONFIG_PROC_FS */
			pr_info("NETPOLICY: Init net policy for %s\n", dev->name);
		}
	}
	rtnl_unlock();

	return 0;
}

static void __net_exit netpolicy_net_exit(struct net *net)
{
	struct net_device *dev, *aux;

	rtnl_lock();
	for_each_netdev_safe(net, dev, aux)
		uninit_netpolicy(dev);
	rtnl_unlock();
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
