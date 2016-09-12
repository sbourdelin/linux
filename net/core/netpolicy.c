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
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/rtnetlink.h>
#include <linux/sort.h>

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

static u32 netpolicy_get_cpu_information(void)
{
	return num_online_cpus();
}

static void netpolicy_free_sys_map(struct net_device *dev)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;

	kfree(s_info->rx);
	s_info->rx = NULL;
	s_info->avail_rx_num = 0;
	kfree(s_info->tx);
	s_info->tx = NULL;
	s_info->avail_tx_num = 0;
}

static int netpolicy_update_sys_map(struct net_device *dev,
				    struct netpolicy_dev_info *d_info,
				    u32 cpu)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;
	u32 num, i, online_cpu;
	cpumask_var_t cpumask;

	if (!alloc_cpumask_var(&cpumask, GFP_ATOMIC))
		return -ENOMEM;

	/* update rx cpu map */
	if (cpu > d_info->rx_num)
		num = d_info->rx_num;
	else
		num = cpu;

	s_info->avail_rx_num = num;
	s_info->rx = kcalloc(num, sizeof(*s_info->rx), GFP_ATOMIC);
	if (!s_info->rx)
		goto err;
	cpumask_copy(cpumask, cpu_online_mask);

	i = 0;
	for_each_cpu(online_cpu, cpumask) {
		if (i == num)
			break;
		s_info->rx[i].cpu = online_cpu;
		s_info->rx[i].queue = i;
		s_info->rx[i].irq = d_info->rx_irq[i];
		i++;
	}

	/* update tx cpu map */
	if (cpu >= d_info->tx_num)
		num = d_info->tx_num;
	else
		num = cpu;

	s_info->avail_tx_num = num;
	s_info->tx = kcalloc(num, sizeof(*s_info->tx), GFP_ATOMIC);
	if (!s_info->tx)
		goto err;

	i = 0;
	for_each_cpu(online_cpu, cpumask) {
		if (i == num)
			break;
		s_info->tx[i].cpu = online_cpu;
		s_info->tx[i].queue = i;
		s_info->tx[i].irq = d_info->tx_irq[i];
		i++;
	}

	free_cpumask_var(cpumask);
	return 0;
err:
	netpolicy_free_sys_map(dev);
	free_cpumask_var(cpumask);
	return -ENOMEM;
}

static void netpolicy_clear_affinity(struct net_device *dev)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;
	u32 i;

	for (i = 0; i < s_info->avail_rx_num; i++) {
		irq_clear_status_flags(s_info->rx[i].irq, IRQ_NO_BALANCING);
		irq_set_affinity_hint(s_info->rx[i].irq, cpu_online_mask);
	}

	for (i = 0; i < s_info->avail_tx_num; i++) {
		irq_clear_status_flags(s_info->tx[i].irq, IRQ_NO_BALANCING);
		irq_set_affinity_hint(s_info->tx[i].irq, cpu_online_mask);
	}
}

static void netpolicy_set_affinity(struct net_device *dev)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;
	u32 i;

	for (i = 0; i < s_info->avail_rx_num; i++) {
		irq_set_status_flags(s_info->rx[i].irq, IRQ_NO_BALANCING);
		irq_set_affinity_hint(s_info->rx[i].irq, cpumask_of(s_info->rx[i].cpu));
	}

	for (i = 0; i < s_info->avail_tx_num; i++) {
		irq_set_status_flags(s_info->tx[i].irq, IRQ_NO_BALANCING);
		irq_set_affinity_hint(s_info->tx[i].irq, cpumask_of(s_info->tx[i].cpu));
	}
}

static void netpolicy_free_obj_list(struct net_device *dev)
{
	int i, j;
	struct netpolicy_object *obj, *tmp;

	spin_lock(&dev->np_ob_list_lock);
	for (i = 0; i < NETPOLICY_RXTX; i++) {
		for (j = NET_POLICY_NONE; j < NET_POLICY_MAX; j++) {
			if (list_empty(&dev->netpolicy->obj_list[i][j]))
				continue;
			list_for_each_entry_safe(obj, tmp, &dev->netpolicy->obj_list[i][j], list) {
				list_del(&obj->list);
				kfree(obj);
			}
		}
	}
	spin_unlock(&dev->np_ob_list_lock);
}

static int netpolicy_disable(struct net_device *dev)
{
	if (dev->netpolicy->irq_affinity)
		netpolicy_clear_affinity(dev);
	netpolicy_free_sys_map(dev);
	netpolicy_free_obj_list(dev);

	return 0;
}

static int netpolicy_enable(struct net_device *dev)
{
	int ret;
	struct netpolicy_dev_info d_info;
	u32 cpu;

	if (WARN_ON(!dev->netpolicy))
		return -EINVAL;

	/* get driver information */
	ret = netpolicy_get_dev_info(dev, &d_info);
	if (ret)
		return ret;

	/* get cpu information */
	cpu = netpolicy_get_cpu_information();

	/* create sys map */
	ret = netpolicy_update_sys_map(dev, &d_info, cpu);
	if (ret) {
		netpolicy_free_dev_info(&d_info);
		return ret;
	}

	/* set irq affinity */
	if (dev->netpolicy->irq_affinity)
		netpolicy_set_affinity(dev);

	netpolicy_free_dev_info(&d_info);
	return 0;
}

const char *policy_name[NET_POLICY_MAX] = {
	"NONE"
};

static u32 cpu_to_queue(struct net_device *dev,
			u32 cpu, bool is_rx)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;
	int i;

	if (is_rx) {
		for (i = 0; i < s_info->avail_rx_num; i++) {
			if (s_info->rx[i].cpu == cpu)
				return s_info->rx[i].queue;
		}
	} else {
		for (i = 0; i < s_info->avail_tx_num; i++) {
			if (s_info->tx[i].cpu == cpu)
				return s_info->tx[i].queue;
		}
	}

	return ~0;
}

static int netpolicy_add_obj(struct net_device *dev,
			     u32 cpu, bool is_rx,
			     enum netpolicy_name policy)
{
	struct netpolicy_object *obj;
	int dir = is_rx ? NETPOLICY_RX : NETPOLICY_TX;

	obj = kzalloc(sizeof(*obj), GFP_ATOMIC);
	if (!obj)
		return -ENOMEM;
	obj->cpu = cpu;
	obj->queue = cpu_to_queue(dev, cpu, is_rx);
	list_add_tail(&obj->list, &dev->netpolicy->obj_list[dir][policy]);

	return 0;
}

struct sort_node {
	int	node;
	int	distance;
};

static inline int node_distance_cmp(const void *a, const void *b)
{
	const struct sort_node *_a = a;
	const struct sort_node *_b = b;

	return _a->distance - _b->distance;
}

static int _netpolicy_gen_obj_list(struct net_device *dev, bool is_rx,
				   enum netpolicy_name policy,
				   struct sort_node *nodes, int num_node,
				   struct cpumask *node_avail_cpumask)
{
	cpumask_var_t node_tmp_cpumask, sibling_tmp_cpumask;
	struct cpumask *node_assigned_cpumask;
	int i, ret = -ENOMEM;
	u32 cpu;

	if (!alloc_cpumask_var(&node_tmp_cpumask, GFP_ATOMIC))
		return ret;
	if (!alloc_cpumask_var(&sibling_tmp_cpumask, GFP_ATOMIC))
		goto alloc_fail1;

	node_assigned_cpumask = kcalloc(num_node, sizeof(struct cpumask), GFP_ATOMIC);
	if (!node_assigned_cpumask)
		goto alloc_fail2;

	/* Don't share physical core */
	for (i = 0; i < num_node; i++) {
		if (cpumask_weight(&node_avail_cpumask[nodes[i].node]) == 0)
			continue;
		spin_lock(&dev->np_ob_list_lock);
		cpumask_copy(node_tmp_cpumask, &node_avail_cpumask[nodes[i].node]);
		while (cpumask_weight(node_tmp_cpumask)) {
			cpu = cpumask_first(node_tmp_cpumask);

			/* push to obj list */
			ret = netpolicy_add_obj(dev, cpu, is_rx, policy);
			if (ret) {
				spin_unlock(&dev->np_ob_list_lock);
				goto err;
			}

			cpumask_set_cpu(cpu, &node_assigned_cpumask[nodes[i].node]);
			cpumask_and(sibling_tmp_cpumask, node_tmp_cpumask, topology_sibling_cpumask(cpu));
			cpumask_xor(node_tmp_cpumask, node_tmp_cpumask, sibling_tmp_cpumask);
		}
		spin_unlock(&dev->np_ob_list_lock);
	}

	for (i = 0; i < num_node; i++) {
		cpumask_xor(node_tmp_cpumask, &node_avail_cpumask[nodes[i].node], &node_assigned_cpumask[nodes[i].node]);
		if (cpumask_weight(node_tmp_cpumask) == 0)
			continue;
		spin_lock(&dev->np_ob_list_lock);
		for_each_cpu(cpu, node_tmp_cpumask) {
			/* push to obj list */
			ret = netpolicy_add_obj(dev, cpu, is_rx, policy);
			if (ret) {
				spin_unlock(&dev->np_ob_list_lock);
				goto err;
			}
			cpumask_set_cpu(cpu, &node_assigned_cpumask[nodes[i].node]);
		}
		spin_unlock(&dev->np_ob_list_lock);
	}

err:
	kfree(node_assigned_cpumask);
alloc_fail2:
	free_cpumask_var(sibling_tmp_cpumask);
alloc_fail1:
	free_cpumask_var(node_tmp_cpumask);

	return ret;
}

static int netpolicy_gen_obj_list(struct net_device *dev,
				  enum netpolicy_name policy)
{
	struct netpolicy_sys_info *s_info = &dev->netpolicy->sys_info;
	struct cpumask *node_avail_cpumask;
	struct sort_node *nodes;
	int i, ret, node = 0;
	int num_nodes = 1;
	u32 cpu;
#ifdef CONFIG_NUMA
	int dev_node = 0;
	int val;
#endif
	/* The network performance for objects could be different
	 * because of the queue and cpu topology.
	 * The objects will be ordered accordingly,
	 * and put high performance object in the front.
	 *
	 * The priority rules as below,
	 * - The local object. (Local means cpu and queue are in the same node.)
	 * - The cpu in the object is the only logical core in physical core.
	 *   The sibiling core's object has not been added in the object list yet.
	 * - The rest of objects
	 *
	 * So the order of object list is as below:
	 * 1. Local core + the only logical core
	 * 2. Remote core + the only logical core
	 * 3. Local core + the core's sibling is already in the object list
	 * 4. Remote core + the core's sibling is already in the object list
	 */
#ifdef CONFIG_NUMA
	dev_node = dev_to_node(dev->dev.parent);
	num_nodes = num_online_nodes();
#endif

	nodes = kcalloc(num_nodes, sizeof(*nodes), GFP_ATOMIC);
	if (!nodes)
		return -ENOMEM;

	node_avail_cpumask = kcalloc(num_nodes, sizeof(struct cpumask), GFP_ATOMIC);
	if (!node_avail_cpumask) {
		kfree(nodes);
		return -ENOMEM;
	}

#ifdef CONFIG_NUMA
	/* order the node from near to far */
	for_each_node_mask(i, node_online_map) {
		val = node_distance(dev_node, i);
		nodes[node].node = i;
		nodes[node].distance = val;
		node++;
	}
	sort(nodes, num_nodes, sizeof(*nodes),
	     node_distance_cmp, NULL);
#else
	nodes[0].node = 0;
#endif

	for (i = 0; i < s_info->avail_rx_num; i++) {
		cpu = s_info->rx[i].cpu;
		cpumask_set_cpu(cpu, &node_avail_cpumask[cpu_to_node(cpu)]);
	}
	ret = _netpolicy_gen_obj_list(dev, true, policy, nodes,
				      node, node_avail_cpumask);
	if (ret)
		goto err;

	for (i = 0; i < node; i++)
		cpumask_clear(&node_avail_cpumask[nodes[i].node]);

	for (i = 0; i < s_info->avail_tx_num; i++) {
		cpu = s_info->tx[i].cpu;
		cpumask_set_cpu(cpu, &node_avail_cpumask[cpu_to_node(cpu)]);
	}
	ret = _netpolicy_gen_obj_list(dev, false, policy, nodes,
				      node, node_avail_cpumask);
	if (ret)
		goto err;

err:
	kfree(nodes);
	kfree(node_avail_cpumask);
	return ret;
}

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
	int ret, i, j;

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
		goto unlock;
	}

	spin_lock(&dev->np_ob_list_lock);
	for (i = 0; i < NETPOLICY_RXTX; i++) {
		for (j = NET_POLICY_NONE; j < NET_POLICY_MAX; j++)
			INIT_LIST_HEAD(&dev->netpolicy->obj_list[i][j]);
	}
	spin_unlock(&dev->np_ob_list_lock);

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
