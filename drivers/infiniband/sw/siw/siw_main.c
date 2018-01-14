/*
 * Software iWARP device driver
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2017, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses. You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/net_namespace.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"
#include "siw_verbs.h"
#include <linux/kthread.h>

MODULE_AUTHOR("Bernard Metzler");
MODULE_DESCRIPTION("Software iWARP Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.2");

/* transmit from user buffer, if possible */
const bool zcopy_tx;

/* Restrict usage of GSO, if hardware peer iwarp is unable to process
 * large packets. gso_seg_limit = 1 lets siw send only packets up to
 * one real MTU in size, but severly limits maximum bandwidth.
 * gso_seg_limit = 0 makes use of GSO (and more than doubles throughput
 * for large transfers).
 */
const int gso_seg_limit;

/* Attach siw also with loopback devices */
const bool loopback_enabled = true;

/* We try to negotiate CRC on, if true */
const bool mpa_crc_required;

/* MPA CRC on/off enforced */
const bool mpa_crc_strict;

/* Set TCP_NODELAY, and push messages asap */
const bool siw_lowdelay = true;
/* Set TCP_QUICKACK */
const bool tcp_quickack;

/* Select MPA version to be used during connection setup */
u_char mpa_version = MPA_REVISION_2;

/* Selects MPA P2P mode (additional handshake during connection
 * setup, if true
 */
const bool peer_to_peer;

static LIST_HEAD(siw_devlist);

struct task_struct *siw_tx_thread[NR_CPUS];
struct crypto_shash *siw_crypto_shash;

static ssize_t show_sw_version(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct siw_device *sdev = container_of(dev, struct siw_device,
					       base_dev.dev);

	return sprintf(buf, "%x\n", sdev->attrs.version);
}

static DEVICE_ATTR(sw_version, 0444, show_sw_version, NULL);

static struct device_attribute *siw_dev_attributes[] = {
	&dev_attr_sw_version
};

static int siw_modify_port(struct ib_device *base_dev, u8 port, int mask,
			   struct ib_port_modify *props)
{
	return -EOPNOTSUPP;
}

static int siw_device_register(struct siw_device *sdev)
{
	struct ib_device *base_dev = &sdev->base_dev;
	int rv, i;
	static int dev_id = 1;

	rv = ib_register_device(base_dev, NULL);
	if (rv) {
		pr_warn("siw: %s: registration error %d\n",
			base_dev->name, rv);
		return rv;
	}

	for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i) {
		rv = device_create_file(&base_dev->dev, siw_dev_attributes[i]);
		if (rv) {
			pr_warn("siw: %s: create file error: rv=%d\n",
				base_dev->name, rv);
			ib_unregister_device(base_dev);
			return rv;
		}
	}
	siw_debugfs_add_device(sdev);

	sdev->attrs.vendor_part_id = dev_id++;

	siw_dbg(sdev, "HWaddr=%02x.%02x.%02x.%02x.%02x.%02x\n",
		*(u8 *)sdev->netdev->dev_addr,
		*((u8 *)sdev->netdev->dev_addr + 1),
		*((u8 *)sdev->netdev->dev_addr + 2),
		*((u8 *)sdev->netdev->dev_addr + 3),
		*((u8 *)sdev->netdev->dev_addr + 4),
		*((u8 *)sdev->netdev->dev_addr + 5));

	sdev->is_registered = 1;

	return 0;
}

static void siw_device_deregister(struct siw_device *sdev)
{
	int i;

	siw_debugfs_del_device(sdev);

	if (sdev->is_registered) {

		siw_dbg(sdev, "deregister\n");

		for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i)
			device_remove_file(&sdev->base_dev.dev,
					   siw_dev_attributes[i]);

		ib_unregister_device(&sdev->base_dev);
	}
	if (atomic_read(&sdev->num_ctx) || atomic_read(&sdev->num_srq) ||
	    atomic_read(&sdev->num_mr) || atomic_read(&sdev->num_cep) ||
	    atomic_read(&sdev->num_qp) || atomic_read(&sdev->num_cq) ||
	    atomic_read(&sdev->num_pd)) {
		pr_warn("siw at %s: orphaned resources!\n",
			sdev->netdev->name);
		pr_warn("           CTX %d, SRQ %d, QP %d, CQ %d, MEM %d, CEP %d, PD %d\n",
			atomic_read(&sdev->num_ctx),
			atomic_read(&sdev->num_srq),
			atomic_read(&sdev->num_qp),
			atomic_read(&sdev->num_cq),
			atomic_read(&sdev->num_mr),
			atomic_read(&sdev->num_cep),
			atomic_read(&sdev->num_pd));
	}

	while (!list_empty(&sdev->cep_list)) {
		struct siw_cep *cep = list_entry(sdev->cep_list.next,
						 struct siw_cep, devq);
		list_del(&cep->devq);
		pr_warn("siw: at %s: free orphaned CEP 0x%p, state %d\n",
			sdev->base_dev.name, cep, cep->state);
		kfree(cep);
	}
	sdev->is_registered = 0;
}

static void siw_device_destroy(struct siw_device *sdev)
{
	siw_dbg(sdev, "destroy device\n");
	siw_idr_release(sdev);

	kfree(sdev->base_dev.iwcm);
	dev_put(sdev->netdev);

	ib_dealloc_device(&sdev->base_dev);
}

static struct siw_device *siw_dev_from_netdev(struct net_device *dev)
{
	if (!list_empty(&siw_devlist)) {
		struct list_head *pos;

		list_for_each(pos, &siw_devlist) {
			struct siw_device *sdev =
				list_entry(pos, struct siw_device, list);
			if (sdev->netdev == dev)
				return sdev;
		}
	}
	return NULL;
}

static int siw_create_tx_threads(void)
{
	int cpu, rv, assigned = 0;

	for_each_online_cpu(cpu) {
		/* Skip HT cores */
		if (cpu % cpumask_weight(topology_sibling_cpumask(cpu))) {
			siw_tx_thread[cpu] = NULL;
			continue;
		}
		siw_tx_thread[cpu] = kthread_create(siw_run_sq,
						   (unsigned long *)(long)cpu,
						   "siw_tx/%d", cpu);
		if (IS_ERR(siw_tx_thread[cpu])) {
			rv = PTR_ERR(siw_tx_thread[cpu]);
			siw_tx_thread[cpu] = NULL;
			pr_info("Creating TX thread for CPU %d failed", cpu);
			continue;
		}
		kthread_bind(siw_tx_thread[cpu], cpu);

		wake_up_process(siw_tx_thread[cpu]);
		assigned++;
	}
	return assigned;
}

static int siw_dev_qualified(struct net_device *netdev)
{
	/*
	 * Additional hardware support can be added here
	 * (e.g. ARPHRD_FDDI, ARPHRD_ATM, ...) - see
	 * <linux/if_arp.h> for type identifiers.
	 */
	if (netdev->type == ARPHRD_ETHER ||
	    netdev->type == ARPHRD_IEEE802 ||
	    (netdev->type == ARPHRD_LOOPBACK && loopback_enabled))
		return 1;

	return 0;
}

static DEFINE_PER_CPU(atomic_t, use_cnt = ATOMIC_INIT(0));

static struct {
	struct cpumask **tx_valid_cpus;
	int num_nodes;
} siw_cpu_info;

static int siw_init_cpulist(void)
{
	int i, num_nodes;

	num_nodes = num_possible_nodes();
	siw_cpu_info.num_nodes = num_nodes;

	siw_cpu_info.tx_valid_cpus = kcalloc(num_nodes, sizeof(void *),
					     GFP_KERNEL);
	if (!siw_cpu_info.tx_valid_cpus) {
		siw_cpu_info.num_nodes = 0;
		return -ENOMEM;
	}

	for (i = 0; i < siw_cpu_info.num_nodes; i++) {
		siw_cpu_info.tx_valid_cpus[i] = kzalloc(sizeof(struct cpumask),
							GFP_KERNEL);
		if (!siw_cpu_info.tx_valid_cpus[i])
			goto out_err;

		cpumask_clear(siw_cpu_info.tx_valid_cpus[i]);
	}
	for_each_possible_cpu(i)
		cpumask_set_cpu(i, siw_cpu_info.tx_valid_cpus[cpu_to_node(i)]);

	return 0;

out_err:
	siw_cpu_info.num_nodes = 0;
	while (i) {
		kfree(siw_cpu_info.tx_valid_cpus[i]);
		siw_cpu_info.tx_valid_cpus[i--] = NULL;
	}
	kfree(siw_cpu_info.tx_valid_cpus);
	siw_cpu_info.tx_valid_cpus = NULL;

	return -ENOMEM;
}

static void siw_destroy_cpulist(void)
{
	int i = 0;

	while (i < siw_cpu_info.num_nodes)
		kfree(siw_cpu_info.tx_valid_cpus[i++]);

	kfree(siw_cpu_info.tx_valid_cpus);
}

/*
 * Choose CPU with least number of active QP's from NUMA node of
 * TX interface.
 */
int siw_get_tx_cpu(struct siw_device *sdev)
{
	const struct cpumask *tx_cpumask;
	int i, num_cpus, cpu, tx_cpu = -1, min_use,
	    node = sdev->numa_node;

	if (node < 0)
		tx_cpumask = cpu_online_mask;
	else
		tx_cpumask = siw_cpu_info.tx_valid_cpus[node];

	num_cpus = cpumask_weight(tx_cpumask);
	if (!num_cpus) {
		/* no CPU on this NUMA node */
		tx_cpumask = cpu_online_mask;
		num_cpus = cpumask_weight(tx_cpumask);
	}
	if (!num_cpus) {
		pr_warn("siw: no tx cpu found\n");
		return tx_cpu;
	}
	cpu = cpumask_first(tx_cpumask);

	for (i = 0, min_use = SIW_MAX_QP; i < num_cpus;
	     i++, cpu = cpumask_next(cpu, tx_cpumask)) {
		int usage;

		/* Skip any cores which have no TX thread */
		if (!siw_tx_thread[cpu])
			continue;

		usage = atomic_inc_return(&per_cpu(use_cnt, cpu));

		if (usage < min_use) {
			min_use = usage;
			tx_cpu = cpu;
		} else {
			atomic_dec_return(&per_cpu(use_cnt, cpu));
		}
		if (min_use == 1)
			break;
	}
	siw_dbg(sdev, "tx cpu %d, node %d, %d qp's\n",
		cpu, node, min_use);

	return tx_cpu;
}

void siw_put_tx_cpu(int cpu)
{
	atomic_dec(&per_cpu(use_cnt, cpu));
}

static void siw_verbs_sq_flush(struct ib_qp *base_qp)
{
	struct siw_qp *qp = siw_qp_base2siw(base_qp);

	down_write(&qp->state_lock);
	siw_sq_flush(qp);
	up_write(&qp->state_lock);
}

static void siw_verbs_rq_flush(struct ib_qp *base_qp)
{
	struct siw_qp *qp = siw_qp_base2siw(base_qp);

	down_write(&qp->state_lock);
	siw_rq_flush(qp);
	up_write(&qp->state_lock);
}

static struct ib_ah *siw_create_ah(struct ib_pd *pd, struct rdma_ah_attr *attr,
				   struct ib_udata *udata)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static int siw_destroy_ah(struct ib_ah *ah)
{
	return -EOPNOTSUPP;
}

static struct siw_device *siw_device_create(struct net_device *netdev)
{
	struct siw_device *sdev;
	struct ib_device *base_dev;
	struct device *parent = netdev->dev.parent;

	sdev = (struct siw_device *)ib_alloc_device(sizeof(*sdev));
	if (!sdev)
		goto out;

	base_dev = &sdev->base_dev;

	if (!parent) {
		/*
		 * The loopback device has no parent device,
		 * so it appears as a top-level device. To support
		 * loopback device connectivity, take this device
		 * as the parent device. Skip all other devices
		 * w/o parent device.
		 */
		if (netdev->type != ARPHRD_LOOPBACK) {
			pr_warn("siw: device %s skipped (no parent dev)\n",
				netdev->name);
			ib_dealloc_device(base_dev);
			sdev = NULL;
			goto out;
		}
		parent = &netdev->dev;
	}
	base_dev->iwcm = kmalloc(sizeof(struct iw_cm_verbs), GFP_KERNEL);
	if (!base_dev->iwcm) {
		ib_dealloc_device(base_dev);
		sdev = NULL;
		goto out;
	}

	sdev->netdev = netdev;
	list_add_tail(&sdev->list, &siw_devlist);

	strcpy(base_dev->name, SIW_IBDEV_PREFIX);
	strlcpy(base_dev->name + strlen(SIW_IBDEV_PREFIX), netdev->name,
		IB_DEVICE_NAME_MAX - strlen(SIW_IBDEV_PREFIX));

	memset(&base_dev->node_guid, 0, sizeof(base_dev->node_guid));

	if (netdev->type != ARPHRD_LOOPBACK) {
		memcpy(&base_dev->node_guid, netdev->dev_addr, 6);
	} else {
		/*
		 * The loopback device does not have a HW address,
		 * but connection mangagement lib expects gid != 0
		 */
		size_t gidlen = min_t(size_t, strlen(base_dev->name), 6);

		memcpy(&base_dev->node_guid, base_dev->name, gidlen);
	}
	base_dev->owner = THIS_MODULE;

	base_dev->uverbs_cmd_mask =
	    (1ull << IB_USER_VERBS_CMD_GET_CONTEXT) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_DEVICE) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_PORT) |
	    (1ull << IB_USER_VERBS_CMD_ALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_DEALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_REG_MR) |
	    (1ull << IB_USER_VERBS_CMD_DEREG_MR) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_CQ) |
	    (1ull << IB_USER_VERBS_CMD_POLL_CQ) |
	    (1ull << IB_USER_VERBS_CMD_REQ_NOTIFY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_QP) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_QP) |
	    (1ull << IB_USER_VERBS_CMD_MODIFY_QP) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_QP) |
	    (1ull << IB_USER_VERBS_CMD_POST_SEND) |
	    (1ull << IB_USER_VERBS_CMD_POST_RECV) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_MODIFY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_REG_MR) |
	    (1ull << IB_USER_VERBS_CMD_DEREG_MR) |
	    (1ull << IB_USER_VERBS_CMD_POST_SRQ_RECV);

	base_dev->node_type = RDMA_NODE_RNIC;
	memcpy(base_dev->node_desc, SIW_NODE_DESC_COMMON,
	       sizeof(SIW_NODE_DESC_COMMON));

	/*
	 * Current model (one-to-one device association):
	 * One Softiwarp device per net_device or, equivalently,
	 * per physical port.
	 */
	base_dev->phys_port_cnt = 1;

	base_dev->dev.parent = parent;
	base_dev->dev.dma_ops = &dma_virt_ops;

	base_dev->num_comp_vectors = num_possible_cpus();
	base_dev->query_device = siw_query_device;
	base_dev->query_port = siw_query_port;
	base_dev->get_port_immutable = siw_get_port_immutable;
	base_dev->query_qp = siw_query_qp;
	base_dev->modify_port = siw_modify_port;
	base_dev->query_pkey = siw_query_pkey;
	base_dev->query_gid = siw_query_gid;
	base_dev->alloc_ucontext = siw_alloc_ucontext;
	base_dev->dealloc_ucontext = siw_dealloc_ucontext;
	base_dev->mmap = siw_mmap;
	base_dev->alloc_pd = siw_alloc_pd;
	base_dev->dealloc_pd = siw_dealloc_pd;
	base_dev->create_ah = siw_create_ah;
	base_dev->destroy_ah = siw_destroy_ah;
	base_dev->create_qp = siw_create_qp;
	base_dev->modify_qp = siw_verbs_modify_qp;
	base_dev->destroy_qp = siw_destroy_qp;
	base_dev->create_cq = siw_create_cq;
	base_dev->destroy_cq = siw_destroy_cq;
	base_dev->resize_cq = NULL;
	base_dev->poll_cq = siw_poll_cq;
	base_dev->get_dma_mr = siw_get_dma_mr;
	base_dev->reg_user_mr = siw_reg_user_mr;
	base_dev->dereg_mr = siw_dereg_mr;
	base_dev->alloc_mr = siw_alloc_mr;
	base_dev->map_mr_sg = siw_map_mr_sg;
	base_dev->dealloc_mw = NULL;

	base_dev->create_srq = siw_create_srq;
	base_dev->modify_srq = siw_modify_srq;
	base_dev->query_srq = siw_query_srq;
	base_dev->destroy_srq = siw_destroy_srq;
	base_dev->post_srq_recv = siw_post_srq_recv;

	base_dev->attach_mcast = NULL;
	base_dev->detach_mcast = NULL;
	base_dev->process_mad = siw_no_mad;

	base_dev->req_notify_cq = siw_req_notify_cq;
	base_dev->post_send = siw_post_send;
	base_dev->post_recv = siw_post_receive;

	base_dev->drain_sq = siw_verbs_sq_flush;
	base_dev->drain_rq = siw_verbs_rq_flush;

	base_dev->iwcm->connect = siw_connect;
	base_dev->iwcm->accept = siw_accept;
	base_dev->iwcm->reject = siw_reject;
	base_dev->iwcm->create_listen = siw_create_listen;
	base_dev->iwcm->destroy_listen = siw_destroy_listen;
	base_dev->iwcm->add_ref = siw_qp_get_ref;
	base_dev->iwcm->rem_ref = siw_qp_put_ref;
	base_dev->iwcm->get_qp = siw_get_base_qp;

	sdev->attrs.version = VERSION_ID_SOFTIWARP;
	sdev->attrs.vendor_id = SIW_VENDOR_ID;
	sdev->attrs.sw_version = VERSION_ID_SOFTIWARP;
	sdev->attrs.max_qp = SIW_MAX_QP;
	sdev->attrs.max_qp_wr = SIW_MAX_QP_WR;
	sdev->attrs.max_ord = SIW_MAX_ORD_QP;
	sdev->attrs.max_ird = SIW_MAX_IRD_QP;
	sdev->attrs.cap_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;
	sdev->attrs.max_sge = SIW_MAX_SGE;
	sdev->attrs.max_sge_rd = SIW_MAX_SGE_RD;
	sdev->attrs.max_cq = SIW_MAX_CQ;
	sdev->attrs.max_cqe = SIW_MAX_CQE;
	sdev->attrs.max_mr = SIW_MAX_MR;
	sdev->attrs.max_mr_size = rlimit(RLIMIT_MEMLOCK);
	sdev->attrs.max_pd = SIW_MAX_PD;
	sdev->attrs.max_mw = SIW_MAX_MW;
	sdev->attrs.max_fmr = SIW_MAX_FMR;
	sdev->attrs.max_srq = SIW_MAX_SRQ;
	sdev->attrs.max_srq_wr = SIW_MAX_SRQ_WR;
	sdev->attrs.max_srq_sge = SIW_MAX_SGE;

	siw_idr_init(sdev);
	INIT_LIST_HEAD(&sdev->cep_list);
	INIT_LIST_HEAD(&sdev->qp_list);
	INIT_LIST_HEAD(&sdev->mr_list);

	atomic_set(&sdev->num_ctx, 0);
	atomic_set(&sdev->num_srq, 0);
	atomic_set(&sdev->num_qp, 0);
	atomic_set(&sdev->num_cq, 0);
	atomic_set(&sdev->num_mr, 0);
	atomic_set(&sdev->num_pd, 0);
	atomic_set(&sdev->num_cep, 0);

	sdev->numa_node = dev_to_node(parent);

	sdev->is_registered = 0;
out:
	if (sdev)
		dev_hold(netdev);

	return sdev;
}

static int siw_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *arg)
{
	struct net_device	*netdev = netdev_notifier_info_to_dev(arg);
	struct in_device	*in_dev;
	struct siw_device	*sdev;

	dev_dbg(&netdev->dev, "siw: event %lu\n", event);

	if (dev_net(netdev) != &init_net)
		goto done;

	sdev = siw_dev_from_netdev(netdev);

	switch (event) {

	case NETDEV_UP:
		if (!sdev)
			break;

		if (sdev->is_registered) {
			sdev->state = IB_PORT_ACTIVE;
			siw_port_event(sdev, 1, IB_EVENT_PORT_ACTIVE);
			break;
		}
		in_dev = in_dev_get(netdev);
		if (!in_dev) {
			dev_dbg(&netdev->dev, "siw: no in_device\n");
			sdev->state = IB_PORT_INIT;
			break;
		}
		if (in_dev->ifa_list) {
			sdev->state = IB_PORT_ACTIVE;
			if (siw_device_register(sdev))
				sdev->state = IB_PORT_INIT;
		} else {
			dev_dbg(&netdev->dev, "siw: no ifa_list\n");
			sdev->state = IB_PORT_INIT;
		}
		in_dev_put(in_dev);

		break;

	case NETDEV_DOWN:
		if (sdev && sdev->is_registered) {
			sdev->state = IB_PORT_DOWN;
			siw_port_event(sdev, 1, IB_EVENT_PORT_ERR);
			break;
		}
		break;

	case NETDEV_REGISTER:
		if (!sdev) {
			if (!siw_dev_qualified(netdev))
				break;

			sdev = siw_device_create(netdev);
			if (sdev) {
				sdev->state = IB_PORT_INIT;
				dev_dbg(&netdev->dev, "siw: new device\n");
			}
		}
		break;

	case NETDEV_UNREGISTER:
		if (sdev) {
			if (sdev->is_registered)
				siw_device_deregister(sdev);
			list_del(&sdev->list);
			siw_device_destroy(sdev);
		}
		break;

	case NETDEV_CHANGEADDR:
		if (sdev->is_registered)
			siw_port_event(sdev, 1, IB_EVENT_LID_CHANGE);

		break;
	/*
	 * Todo: Below netdev events are currently not handled.
	 */
	case NETDEV_CHANGEMTU:
	case NETDEV_GOING_DOWN:
	case NETDEV_CHANGE:

		break;

	default:
		break;
	}
done:
	return NOTIFY_OK;
}

static struct notifier_block siw_netdev_nb = {
	.notifier_call = siw_netdev_event,
};

/*
 * siw_init_module - Initialize Softiwarp module and register with netdev
 *                   subsystem to create Softiwarp devices per net_device
 */
static __init int siw_init_module(void)
{
	int rv;
	int nr_cpu;

	if (SENDPAGE_THRESH < SIW_MAX_INLINE) {
		pr_info("siw: sendpage threshold too small: %u\n",
			(int)SENDPAGE_THRESH);
		rv = EINVAL;
		goto out_error;
	}
	rv = siw_init_cpulist();
	if (rv)
		goto out_error;

	rv = siw_cm_init();
	if (rv)
		goto out_error;

	siw_debug_init();

	/*
	 * Allocate CRC SHASH object. Fail loading siw only, if CRC is
	 * required by kernel module
	 */
	siw_crypto_shash = crypto_alloc_shash("crc32c", 0, 0);
	if (IS_ERR(siw_crypto_shash)) {
		pr_info("siw: Loading CRC32c failed: %ld\n",
			PTR_ERR(siw_crypto_shash));
		siw_crypto_shash = NULL;
		if (mpa_crc_required == true)
			goto out_error;
	}
	rv = register_netdevice_notifier(&siw_netdev_nb);
	if (rv) {
		siw_debugfs_delete();
		goto out_error;
	}
	for (nr_cpu = 0; nr_cpu < nr_cpu_ids; nr_cpu++)
		siw_tx_thread[nr_cpu] = NULL;

	if (!siw_create_tx_threads()) {
		pr_info("siw: Could not start any TX thread\n");
		unregister_netdevice_notifier(&siw_netdev_nb);
		goto out_error;
	}
	pr_info("SoftiWARP attached\n");
	return 0;

out_error:
	for (nr_cpu = 0; nr_cpu < nr_cpu_ids; nr_cpu++) {
		if (siw_tx_thread[nr_cpu]) {
			siw_stop_tx_thread(nr_cpu);
			siw_tx_thread[nr_cpu] = NULL;
		}
	}
	if (siw_crypto_shash)
		crypto_free_shash(siw_crypto_shash);

	pr_info("SoftIWARP attach failed. Error: %d\n", rv);

	siw_cm_exit();
	siw_destroy_cpulist();

	return rv;
}

static void __exit siw_exit_module(void)
{
	int nr_cpu;

	for (nr_cpu = 0; nr_cpu < nr_cpu_ids; nr_cpu++) {
		if (siw_tx_thread[nr_cpu]) {
			siw_stop_tx_thread(nr_cpu);
			siw_tx_thread[nr_cpu] = NULL;
		}
	}
	unregister_netdevice_notifier(&siw_netdev_nb);

	siw_cm_exit();

	while (!list_empty(&siw_devlist)) {
		struct siw_device *sdev =
			list_entry(siw_devlist.next, struct siw_device, list);
		list_del(&sdev->list);
		if (sdev->is_registered)
			siw_device_deregister(sdev);

		siw_device_destroy(sdev);
	}
	if (siw_crypto_shash)
		crypto_free_shash(siw_crypto_shash);

	siw_debugfs_delete();
	siw_destroy_cpulist();

	pr_info("SoftiWARP detached\n");
}

module_init(siw_init_module);
module_exit(siw_exit_module);
