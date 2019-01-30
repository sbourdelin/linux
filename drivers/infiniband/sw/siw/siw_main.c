/*
 * Software iWARP device driver
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2019, IBM Corporation
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
#include <rdma/rdma_netlink.h>

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
const bool zcopy_tx = true;

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
const bool siw_tcp_nagle;

/* Select MPA version to be used during connection setup */
u_char mpa_version = MPA_REVISION_2;

/* Selects MPA P2P mode (additional handshake during connection
 * setup, if true.
 */
const bool peer_to_peer;

struct task_struct *siw_tx_thread[NR_CPUS];
struct crypto_shash *siw_crypto_shash;

static ssize_t sw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%x\n", VERSION_ID_SOFTIWARP);
}

static DEVICE_ATTR_RO(sw_version);

static ssize_t parent_show(struct device *device,
			   struct device_attribute *attr, char *buf)
{
	struct siw_device *sdev =
		rdma_device_to_drv_device(device, struct siw_device, base_dev);

	return snprintf(buf, 16, "%s\n", sdev->netdev->name);
}

static DEVICE_ATTR_RO(parent);

static struct attribute *siw_dev_attributes[] = {
	&dev_attr_sw_version.attr,
	&dev_attr_parent.attr,
	NULL,
};

static const struct attribute_group siw_attr_group = {
	.attrs = siw_dev_attributes,
};

static int siw_modify_port(struct ib_device *base_dev, u8 port, int mask,
			   struct ib_port_modify *props)
{
	return -EOPNOTSUPP;
}

static int siw_device_register(struct siw_device *sdev, const char *name)
{
	struct ib_device *base_dev = &sdev->base_dev;
	static int dev_id = 1;
	int rv;

	base_dev->driver_id = RDMA_DRIVER_SIW;
	rdma_set_device_sysfs_group(base_dev, &siw_attr_group);

	rv = ib_register_device(base_dev, name);
	if (rv) {
		pr_warn("siw: device registration error %d\n", rv);
		return rv;
	}
	siw_debugfs_add_device(sdev);

	sdev->vendor_part_id = dev_id++;

	siw_dbg(sdev, "HWaddr=%02x.%02x.%02x.%02x.%02x.%02x\n",
		*(u8 *)sdev->netdev->dev_addr,
		*((u8 *)sdev->netdev->dev_addr + 1),
		*((u8 *)sdev->netdev->dev_addr + 2),
		*((u8 *)sdev->netdev->dev_addr + 3),
		*((u8 *)sdev->netdev->dev_addr + 4),
		*((u8 *)sdev->netdev->dev_addr + 5));

	return 0;
}

static void siw_device_cleanup(struct siw_device *sdev)
{
	siw_debugfs_del_device(sdev);

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
}

static void siw_device_destroy(struct siw_device *sdev)
{
	siw_idr_release(sdev);
	kfree(sdev->base_dev.iwcm);
	dev_put(sdev->netdev);
}

/*
 * Returns siw device if registered for given net device.
 * Increments reference count on contained base ib_device,
 * if siw device was found (via ib_device_get_by_netdev()).
 */
static struct siw_device *siw_dev_from_netdev(struct net_device *netdev)
{
	struct ib_device *base_dev =
		ib_device_get_by_netdev(netdev, RDMA_DRIVER_SIW);

	return (base_dev != NULL) ? siw_dev_base2siw(base_dev) : NULL;
}

static struct net_device *siw_get_netdev(struct ib_device *base_dev, u8 port)
{
	struct siw_device *sdev = siw_dev_base2siw(base_dev);

	if (!sdev->netdev)
		return NULL;

	dev_hold(sdev->netdev);

	return sdev->netdev;
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
	int i, num_cpus, cpu, min_use,
	    node = sdev->numa_node,
	    tx_cpu = -1;

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
	if (!num_cpus)
		goto out;

	cpu = cpumask_first(tx_cpumask);

	for (i = 0, min_use = SIW_MAX_QP; i < num_cpus;
	     i++, cpu = cpumask_next(cpu, tx_cpumask)) {
		int usage;

		/* Skip any cores which have no TX thread */
		if (!siw_tx_thread[cpu])
			continue;

		usage = atomic_read(&per_cpu(use_cnt, cpu));
		if (usage <= min_use) {
			tx_cpu = cpu;
			min_use = usage;
		}
	}
	siw_dbg(sdev, "tx cpu %d, node %d, %d qp's\n",
		tx_cpu, node, min_use);

out:
	if (tx_cpu >= 0)
		atomic_inc(&per_cpu(use_cnt, tx_cpu));
	else
		pr_warn("siw: no tx cpu found\n");

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
				   u32 flags, struct ib_udata *udata)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static int siw_destroy_ah(struct ib_ah *ah, u32 flags)
{
	return -EOPNOTSUPP;
}

static void siw_unregistered(struct ib_device *base_dev)
{
	struct siw_device *sdev = siw_dev_base2siw(base_dev);

	siw_device_cleanup(sdev);
	siw_device_destroy(sdev);
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

	base_dev->driver_unregister = siw_unregistered;

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
	base_dev->ops.query_device = siw_query_device;
	base_dev->ops.query_port = siw_query_port;
	base_dev->ops.get_port_immutable = siw_get_port_immutable;
	base_dev->ops.get_netdev = siw_get_netdev;
	base_dev->ops.query_qp = siw_query_qp;
	base_dev->ops.modify_port = siw_modify_port;
	base_dev->ops.query_pkey = siw_query_pkey;
	base_dev->ops.query_gid = siw_query_gid;
	base_dev->ops.alloc_ucontext = siw_alloc_ucontext;
	base_dev->ops.dealloc_ucontext = siw_dealloc_ucontext;
	base_dev->ops.mmap = siw_mmap;
	base_dev->ops.alloc_pd = siw_alloc_pd;
	base_dev->ops.dealloc_pd = siw_dealloc_pd;
	base_dev->ops.create_ah = siw_create_ah;
	base_dev->ops.destroy_ah = siw_destroy_ah;
	base_dev->ops.create_qp = siw_create_qp;
	base_dev->ops.modify_qp = siw_verbs_modify_qp;
	base_dev->ops.destroy_qp = siw_destroy_qp;
	base_dev->ops.create_cq = siw_create_cq;
	base_dev->ops.destroy_cq = siw_destroy_cq;
	base_dev->ops.resize_cq = NULL;
	base_dev->ops.poll_cq = siw_poll_cq;
	base_dev->ops.get_dma_mr = siw_get_dma_mr;
	base_dev->ops.reg_user_mr = siw_reg_user_mr;
	base_dev->ops.dereg_mr = siw_dereg_mr;
	base_dev->ops.alloc_mr = siw_alloc_mr;
	base_dev->ops.map_mr_sg = siw_map_mr_sg;
	base_dev->ops.dealloc_mw = NULL;

	base_dev->ops.create_srq = siw_create_srq;
	base_dev->ops.modify_srq = siw_modify_srq;
	base_dev->ops.query_srq = siw_query_srq;
	base_dev->ops.destroy_srq = siw_destroy_srq;
	base_dev->ops.post_srq_recv = siw_post_srq_recv;

	base_dev->ops.attach_mcast = NULL;
	base_dev->ops.detach_mcast = NULL;
	base_dev->ops.process_mad = siw_no_mad;

	base_dev->ops.req_notify_cq = siw_req_notify_cq;
	base_dev->ops.post_send = siw_post_send;
	base_dev->ops.post_recv = siw_post_receive;

	base_dev->ops.drain_sq = siw_verbs_sq_flush;
	base_dev->ops.drain_rq = siw_verbs_rq_flush;

	base_dev->iwcm->connect = siw_connect;
	base_dev->iwcm->accept = siw_accept;
	base_dev->iwcm->reject = siw_reject;
	base_dev->iwcm->create_listen = siw_create_listen;
	base_dev->iwcm->destroy_listen = siw_destroy_listen;
	base_dev->iwcm->add_ref = siw_qp_get_ref;
	base_dev->iwcm->rem_ref = siw_qp_put_ref;
	base_dev->iwcm->get_qp = siw_get_base_qp;

	/* Disable TCP port mapper service */
	base_dev->iwcm->driver_flags = IW_F_NO_PORT_MAP;

	memcpy(base_dev->iwcm->ifname, netdev->name,
	       sizeof(base_dev->iwcm->ifname));

	sdev->attrs.max_qp = SIW_MAX_QP;
	sdev->attrs.max_qp_wr = SIW_MAX_QP_WR;
	sdev->attrs.max_ord = SIW_MAX_ORD_QP;
	sdev->attrs.max_ird = SIW_MAX_IRD_QP;
	sdev->attrs.max_sge = SIW_MAX_SGE;
	sdev->attrs.max_sge_rd = SIW_MAX_SGE_RD;
	sdev->attrs.max_cq = SIW_MAX_CQ;
	sdev->attrs.max_cqe = SIW_MAX_CQE;
	sdev->attrs.max_mr = SIW_MAX_MR;
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
out:
	if (sdev)
		dev_hold(netdev);

	return sdev;
}

static void siw_netdev_unregistered(struct work_struct *work)
{
	struct siw_device *sdev = container_of(work, struct siw_device,
					       netdev_unregister);

	struct siw_qp_attrs qp_attrs;
	struct list_head *pos, *tmp;

	memset(&qp_attrs, 0, sizeof(qp_attrs));
	qp_attrs.state = SIW_QP_STATE_ERROR;

	/*
	 * Mark all current QP's of this device dead
	 */
	list_for_each_safe(pos, tmp, &sdev->qp_list) {
		struct siw_qp *qp = list_entry(pos, struct siw_qp, devq);

		down_write(&qp->state_lock);
		(void) siw_qp_modify(qp, &qp_attrs, SIW_QP_ATTR_STATE);
		up_write(&qp->state_lock);
	}
	ib_unregister_device_and_put(&sdev->base_dev);
}

static int siw_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *arg)
{
	struct net_device	*netdev = netdev_notifier_info_to_dev(arg);
	struct siw_device	*sdev;

	dev_dbg(&netdev->dev, "siw: event %lu\n", event);

	if (dev_net(netdev) != &init_net)
		return NOTIFY_OK;

	sdev = siw_dev_from_netdev(netdev);
	if (!sdev)
		return NOTIFY_OK;

	switch (event) {

	case NETDEV_UP:
		sdev->state = IB_PORT_ACTIVE;
		siw_port_event(sdev, 1, IB_EVENT_PORT_ACTIVE);
		break;

	case NETDEV_DOWN:
		sdev->state = IB_PORT_DOWN;
		siw_port_event(sdev, 1, IB_EVENT_PORT_ERR);
		break;

	case NETDEV_REGISTER:
		/*
		 * Device registration now handled only by
		 * rdma netlink commands. So it shall be impossible
		 * to end up here with a valid siw device.
		 */
		siw_dbg(sdev, "unexpected NETDEV_REGISTER event\n");
		break;

	case NETDEV_UNREGISTER:
		INIT_WORK(&sdev->netdev_unregister, siw_netdev_unregistered);
		schedule_work(&sdev->netdev_unregister);
		break;

	case NETDEV_CHANGEADDR:
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
	ib_device_put(&sdev->base_dev);

	return NOTIFY_OK;
}

static struct notifier_block siw_netdev_nb = {
	.notifier_call = siw_netdev_event,
};

static int siw_newlink(const char *basedev_name, struct net_device *netdev)
{
	struct siw_device *sdev;
	int rv;

	sdev = siw_dev_from_netdev(netdev);
	if (sdev) {
		ib_device_put(&sdev->base_dev);
		rv = -EEXIST;
		goto out;
	}
	if (!siw_dev_qualified(netdev)) {
		rv = -EINVAL;
		goto out;
	}
	sdev = siw_device_create(netdev);
	if (sdev) {
		dev_dbg(&netdev->dev, "siw: new device\n");

		if (netif_running(netdev) && netif_carrier_ok(netdev))
			sdev->state = IB_PORT_ACTIVE;
		else
			sdev->state = IB_PORT_DOWN;

		rv = siw_device_register(sdev, basedev_name);
		if (rv) {
			siw_device_destroy(sdev);
			ib_dealloc_device(&sdev->base_dev);
			goto out;
		}
	} else {
		rv = -ENOMEM;
	}
out:
	return rv;
}

static struct rdma_link_ops siw_link_ops = {
	.type = "siw",
	.newlink = siw_newlink,
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
	rdma_link_register(&siw_link_ops);

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
	rdma_link_unregister(&siw_link_ops);
	ib_unregister_driver(RDMA_DRIVER_SIW);

	siw_cm_exit();

	if (siw_crypto_shash)
		crypto_free_shash(siw_crypto_shash);

	siw_debugfs_delete();
	siw_destroy_cpulist();

	pr_info("SoftiWARP detached\n");
}

module_init(siw_init_module);
module_exit(siw_exit_module);

MODULE_ALIAS_RDMA_LINK("siw");
