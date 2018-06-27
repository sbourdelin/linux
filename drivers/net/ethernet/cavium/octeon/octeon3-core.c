// SPDX-License-Identifier: GPL-2.0
/* Octeon III BGX Nexus Ethernet driver core
 *
 * Copyright (C) 2018 Cavium, Inc.
 */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kthread.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/timecounter.h>

#include <asm/octeon/cvmx-mio-defs.h>

#include "octeon3.h"

/*  First buffer:
 *
 *                            +---SKB---------+
 *                            |               |
 *                            |               |
 *                         +--+--*data        |
 *                         |  |               |
 *                         |  |               |
 *                         |  +---------------+
 *                         |       /|\
 *                         |        |
 *                         |        |
 *                        \|/       |
 * WQE - 128 -+-----> +-------------+-------+     -+-
 *            |       |    *skb ----+       |      |
 *            |       |                     |      |
 *            |       |                     |      |
 *  WQE_SKIP = 128    |                     |      |
 *            |       |                     |      |
 *            |       |                     |      |
 *            |       |                     |      |
 *            |       |                     |      First Skip
 * WQE   -----+-----> +---------------------+      |
 *                    |   word 0            |      |
 *                    |   word 1            |      |
 *                    |   word 2            |      |
 *                    |   word 3            |      |
 *                    |   word 4            |      |
 *                    +---------------------+     -+-
 *               +----+- packet link        |
 *               |    |  packet data        |
 *               |    |                     |
 *               |    |                     |
 *               |    |         .           |
 *               |    |         .           |
 *               |    |         .           |
 *               |    +---------------------+
 *               |
 *               |
 * Later buffers:|
 *               |
 *               |
 *               |
 *               |
 *               |
 *               |            +---SKB---------+
 *               |            |               |
 *               |            |               |
 *               |         +--+--*data        |
 *               |         |  |               |
 *               |         |  |               |
 *               |         |  +---------------+
 *               |         |       /|\
 *               |         |        |
 *               |         |        |
 *               |        \|/       |
 * WQE - 128 ----+--> +-------------+-------+     -+-
 *               |    |    *skb ----+       |      |
 *               |    |                     |      |
 *               |    |                     |      |
 *               |    |                     |      |
 *               |    |                     |      LATER_SKIP = 128
 *               |    |                     |      |
 *               |    |                     |      |
 *               |    |                     |      |
 *               |    +---------------------+     -+-
 *               |    |  packet link        |
 *               +--> |  packet data        |
 *                    |                     |
 *                    |                     |
 *                    |         .           |
 *                    |         .           |
 *                    |         .           |
 *                    +---------------------+
 */

#define FPA3_NUM_AURAS		1024
#define MAX_TX_QUEUE_DEPTH	512
#define MAX_RX_CONTEXTS		32
#define USE_ASYNC_IOBDMA	1	/* Always 1 */

#define SKB_AURA_MAGIC		0xbadc0ffee4dad000ULL
#define SKB_AURA_OFFSET		1
#define SKB_PTR_OFFSET		0

/* PTP registers and bits */
#define MIO_PTP_CLOCK_HI(n)	(CVMX_MIO_PTP_CLOCK_HI + NODE_OFFSET(n))
#define MIO_PTP_CLOCK_CFG(n)	(CVMX_MIO_PTP_CLOCK_CFG + NODE_OFFSET(n))
#define MIO_PTP_CLOCK_COMP(n)	(CVMX_MIO_PTP_CLOCK_COMP + NODE_OFFSET(n))

/* Misc. bitfields */
#define MIO_PTP_CLOCK_CFG_PTP_EN		BIT(0)
#define BGX_GMP_GMI_RX_FRM_CTL_PTP_MODE		BIT(12)

/* Up to 2 napis per core are supported */
#define MAX_NAPI_PER_CPU	2
#define MAX_NAPIS_PER_NODE	(MAX_CORES * MAX_NAPI_PER_CPU)

struct octeon3_napi_wrapper {
	struct napi_struct napi;
	int available;
	int idx;
	int cpu;
	struct octeon3_rx *cxt;
} ____cacheline_aligned_in_smp;

static struct octeon3_napi_wrapper
napi_wrapper[MAX_NODES][MAX_NAPIS_PER_NODE]
__cacheline_aligned_in_smp;

struct octeon3_ethernet;

struct octeon3_rx {
	struct octeon3_napi_wrapper *napiw;
	DECLARE_BITMAP(napi_idx_bitmap, MAX_CORES);
	spinlock_t napi_idx_lock;	/* Protect the napi index bitmap */
	struct octeon3_ethernet *parent;
	int rx_grp;
	int rx_irq;
	cpumask_t rx_affinity_hint;
};

struct octeon3_ethernet {
	struct bgx_port_netdev_priv bgx_priv; /* Must be first element. */
	struct list_head list;
	struct net_device *netdev;
	enum octeon3_mac_type mac_type;
	struct octeon3_rx rx_cxt[MAX_RX_CONTEXTS];
	struct ptp_clock_info ptp_info;
	struct ptp_clock *ptp_clock;
	struct cyclecounter cc;
	struct timecounter tc;
	spinlock_t ptp_lock;		/* Serialize ptp clock adjustments */
	int num_rx_cxt;
	int pki_aura;
	int pknd;
	int pko_queue;
	int node;
	int interface;
	int index;
	int rx_buf_count;
	int tx_complete_grp;
	int rx_timestamp_hw:1;
	int tx_timestamp_hw:1;
	spinlock_t stat_lock;		/* Protects stats counters */
	u64 last_packets;
	u64 last_octets;
	u64 last_dropped;
	atomic64_t rx_packets;
	atomic64_t rx_octets;
	atomic64_t rx_dropped;
	atomic64_t rx_errors;
	atomic64_t rx_length_errors;
	atomic64_t rx_crc_errors;
	atomic64_t tx_packets;
	atomic64_t tx_octets;
	atomic64_t tx_dropped;
	/* The following two fields need to be on a different cache line as
	 * they are updated by pko which invalidates the cache every time it
	 * updates them. The idea is to prevent other fields from being
	 * invalidated unnecessarily.
	 */
	char cacheline_pad1[CVMX_CACHE_LINE_SIZE];
	atomic64_t buffers_needed;
	atomic64_t tx_backlog;
	char cacheline_pad2[CVMX_CACHE_LINE_SIZE];
};

static DEFINE_MUTEX(octeon3_eth_init_mutex);

struct octeon3_ethernet_node;

struct octeon3_ethernet_worker {
	wait_queue_head_t queue;
	struct task_struct *task;
	struct octeon3_ethernet_node *oen;
	atomic_t kick;
	int order;
};

struct octeon3_ethernet_node {
	bool init_done;
	bool napi_init_done;
	int next_cpu_irq_affinity;
	int node;
	int pki_packet_pool;
	int sso_pool;
	int pko_pool;
	void *sso_pool_stack;
	void *pko_pool_stack;
	void *pki_packet_pool_stack;
	int sso_aura;
	int pko_aura;
	int tx_complete_grp;
	int tx_irq;
	cpumask_t tx_affinity_hint;
	struct octeon3_ethernet_worker workers[8];
	struct mutex device_list_lock;	/* Protects the device list */
	struct list_head device_list;
	spinlock_t napi_alloc_lock;	/* Protects napi allocations */
};

/* This array keeps track of the number of napis running on each cpu */
static u8 octeon3_cpu_napi_cnt[NR_CPUS] __cacheline_aligned_in_smp;

static int use_tx_queues;
module_param(use_tx_queues, int, 0644);
MODULE_PARM_DESC(use_tx_queues, "Use network layer transmit queues.");

static int wait_pko_response;
module_param(wait_pko_response, int, 0644);
MODULE_PARM_DESC(wait_pko_response, "Wait for response after each pko command.");

static int num_packet_buffers = 768;
module_param(num_packet_buffers, int, 0444);
MODULE_PARM_DESC(num_packet_buffers, "Number of packet buffers to allocate per port.");

static int packet_buffer_size = 2048;
module_param(packet_buffer_size, int, 0444);
MODULE_PARM_DESC(packet_buffer_size, "Size of each RX packet buffer.");

static int rx_contexts = 1;
module_param(rx_contexts, int, 0444);
MODULE_PARM_DESC(rx_contexts, "Number of RX threads per port.");

int ilk0_lanes = 1;
module_param(ilk0_lanes, int, 0444);
MODULE_PARM_DESC(ilk0_lanes, "Number of SerDes lanes used by ILK link 0.");

int ilk1_lanes = 1;
module_param(ilk1_lanes, int, 0444);
MODULE_PARM_DESC(ilk1_lanes, "Number of SerDes lanes used by ILK link 1.");

static struct octeon3_ethernet_node octeon3_eth_node[MAX_NODES];
static struct kmem_cache *octeon3_eth_sso_pko_cache;

/* Reads a 64 bit value from the processor local scratchpad memory.
 *
 * @param offset byte offset into scratch pad to read
 *
 * @return value read
 */
static inline u64 scratch_read64(u64 offset)
{
	return *(u64 *)((long)SCRATCH_BASE_ADDR + offset);
}

/* Write a 64 bit value to the processor local scratchpad memory.
 *
 * @param offset byte offset into scratch pad to write
 @ @praram value to write
 */
static inline void scratch_write64(u64 offset, u64 value)
{
	*(u64 *)((long)SCRATCH_BASE_ADDR + offset) = value;
}

static int get_pki_chan(int node, int interface, int index)
{
	int pki_chan;

	pki_chan = node << 12;

	if (OCTEON_IS_MODEL(OCTEON_CNF75XX) &&
	    (interface == 1 || interface == 2)) {
		/* SRIO */
		pki_chan |= 0x240 + (2 * (interface - 1)) + index;
	} else {
		/* BGX */
		pki_chan |= 0x800 + (0x100 * interface) + (0x10 * index);
	}

	return pki_chan;
}

/* Map auras to the field priv->buffers_needed. Used to speed up packet
 * transmission.
 */
static void *aura2bufs_needed[MAX_NODES][FPA3_NUM_AURAS];

static int octeon3_eth_lgrp_to_ggrp(int node, int grp)
{
	return (node << 8) | grp;
}

static void octeon3_eth_gen_affinity(int node, cpumask_t *mask)
{
	int cpu;

	do {
		cpu = cpumask_next(octeon3_eth_node[node].next_cpu_irq_affinity,
				   cpu_online_mask);
		octeon3_eth_node[node].next_cpu_irq_affinity++;
		if (cpu >= nr_cpu_ids) {
			octeon3_eth_node[node].next_cpu_irq_affinity = -1;
			continue;
		}
	} while (false);
	cpumask_clear(mask);
	cpumask_set_cpu(cpu, mask);
}

struct wr_ret {
	void *work;
	u16 grp;
};

static inline struct wr_ret octeon3_core_get_work_sync(int grp)
{
	u64 node = cvmx_get_node_num();
	u64 addr, response;
	struct wr_ret r;

	/* See SSO_GET_WORK_LD_S for the address to read */
	addr = SSO_GET_WORK_DMA_S_SCRADDR;
	addr |= SSO_GET_WORK_LD_S_IO;
	addr |= SSO_TAG_SWDID << SSO_GET_WORK_DID_SHIFT;
	addr |= node << SSO_GET_WORK_NODE_SHIFT;
	addr |= SSO_GET_WORK_GROUPED;
	addr |= SSO_GET_WORK_RTNGRP;
	addr |= octeon3_eth_lgrp_to_ggrp(node, grp) <<
		SSO_GET_WORK_IDX_GRP_MASK_SHIFT;
	addr |= SSO_GET_WORK_WAITW_NO_WAIT;
	response = __raw_readq((void __iomem *)addr);

	/* See SSO_GET_WORK_RTN_S for the format of the response */
	r.grp = (response & SSO_GET_WORK_RTN_S_GRP_MASK) >>
		SSO_GET_WORK_RTN_S_GRP_SHIFT;
	if (response & SSO_GET_WORK_RTN_S_NO_WORK)
		r.work = NULL;
	else
		r.work = phys_to_virt(response & SSO_GET_WORK_RTN_S_WQP_MASK);

	return r;
}

/* octeon3_core_get_work_async - Request work via a iobdma command. Doesn't wait
 *				 for the response.
 *
 * @grp: Group to request work for.
 */
static inline void octeon3_core_get_work_async(unsigned int grp)
{
	u64 data, node = cvmx_get_node_num();

	/* See SSO_GET_WORK_DMA_S for the command structure */
	data = 1ull << SSO_GET_WORK_DMA_S_LEN_SHIFT;
	data |= SSO_TAG_SWDID << SSO_GET_WORK_DID_SHIFT;
	data |= node << SSO_GET_WORK_NODE_SHIFT;
	data |= SSO_GET_WORK_GROUPED;
	data |= SSO_GET_WORK_RTNGRP;
	data |= octeon3_eth_lgrp_to_ggrp(node, grp) <<
		SSO_GET_WORK_IDX_GRP_MASK_SHIFT;
	data |= SSO_GET_WORK_WAITW_NO_WAIT;

	__raw_writeq(data, (void __iomem *)IOBDMA_ORDERED_IO_ADDR);
}

/* octeon3_core_get_response_async - Read the request work response. Must be
 *				     called after calling
 *				     octeon3_core_get_work_async().
 *
 * Returns work queue entry.
 */
static inline struct wr_ret octeon3_core_get_response_async(void)
{
	struct wr_ret r;
	u64 response;

	CVMX_SYNCIOBDMA;
	response = scratch_read64(0);

	/* See SSO_GET_WORK_RTN_S for the format of the response */
	r.grp = (response & SSO_GET_WORK_RTN_S_GRP_MASK) >>
		SSO_GET_WORK_RTN_S_GRP_SHIFT;
	if (response & SSO_GET_WORK_RTN_S_NO_WORK)
		r.work = NULL;
	else
		r.work = phys_to_virt(response & SSO_GET_WORK_RTN_S_WQP_MASK);

	return r;
}

static void octeon3_eth_replenish_rx(struct octeon3_ethernet *priv, int count)
{
	struct sk_buff *skb;
	int i;

	for (i = 0; i < count; i++) {
		void **buf;

		skb = __alloc_skb(packet_buffer_size, GFP_ATOMIC, 0,
				  priv->node);
		if (!skb)
			break;
		buf = (void **)PTR_ALIGN(skb->head, 128);
		buf[SKB_PTR_OFFSET] = skb;
		octeon_fpa3_free(priv->node, priv->pki_aura, buf);
	}
}

static bool octeon3_eth_tx_done_runnable(struct octeon3_ethernet_worker *worker)
{
	return atomic_read(&worker->kick) != 0 || kthread_should_stop();
}

static int octeon3_eth_replenish_all(struct octeon3_ethernet_node *oen)
{
	int batch_size = 32, pending = 0;
	struct octeon3_ethernet *priv;

	rcu_read_lock();
	list_for_each_entry_rcu(priv, &oen->device_list, list) {
		int amount = atomic64_sub_if_positive(batch_size,
						      &priv->buffers_needed);

		if (amount >= 0) {
			octeon3_eth_replenish_rx(priv, batch_size);
			pending += amount;
		}
	}
	rcu_read_unlock();
	return pending;
}

static int octeon3_eth_tx_complete_hwtstamp(struct octeon3_ethernet *priv,
					    struct sk_buff *skb)
{
	struct skb_shared_hwtstamps shts;
	u64 hwts, ns;

	hwts = *((u64 *)(skb->cb) + 1);
	ns = timecounter_cyc2time(&priv->tc, hwts);
	memset(&shts, 0, sizeof(shts));
	shts.hwtstamp = ns_to_ktime(ns);
	skb_tstamp_tx(skb, &shts);

	return 0;
}

static int octeon3_eth_tx_complete_worker(void *data)
{
	int backlog, backlog_stop_thresh, i, order, tx_complete_stop_thresh;
	struct octeon3_ethernet_worker *worker = data;
	struct octeon3_ethernet_node *oen = worker->oen;
	u64 aq_cnt;

	order = worker->order;
	backlog_stop_thresh = (order == 0 ? 31 : order * 80);
	tx_complete_stop_thresh = (order * 100);

	while (!kthread_should_stop()) {
		/* Replaced by wait_event to avoid warnings like
		 * "task oct3_eth/0:2:1250 blocked for more than 120 seconds."
		 */
		wait_event_interruptible(worker->queue,
					 octeon3_eth_tx_done_runnable(worker));
		atomic_dec_if_positive(&worker->kick); /* clear the flag */

		do {
			backlog = octeon3_eth_replenish_all(oen);
			for (i = 0; i < 100; i++) {
				void **work;
				struct net_device *tx_netdev;
				struct octeon3_ethernet *tx_priv;
				struct sk_buff *skb;
				struct wr_ret r;

				r = octeon3_core_get_work_sync(oen->tx_complete_grp);
				work = r.work;
				if (!work)
					break;
				tx_netdev = work[0];
				tx_priv = netdev_priv(tx_netdev);
				if (unlikely(netif_queue_stopped(tx_netdev)) && atomic64_read(&tx_priv->tx_backlog) < MAX_TX_QUEUE_DEPTH)
					netif_wake_queue(tx_netdev);
				skb = container_of((void *)work,
						   struct sk_buff, cb);
				if (unlikely(tx_priv->tx_timestamp_hw) && unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS))
					octeon3_eth_tx_complete_hwtstamp(tx_priv, skb);
				dev_kfree_skb(skb);
			}

			aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(oen->node, oen->tx_complete_grp));
			aq_cnt &= SSO_GRP_AQ_CNT_AQ_CNT_MASK;
			if ((backlog > backlog_stop_thresh ||
			     aq_cnt > tx_complete_stop_thresh) &&
			     order < ARRAY_SIZE(oen->workers) - 1) {
				atomic_set(&oen->workers[order + 1].kick, 1);
				wake_up(&oen->workers[order + 1].queue);
			}
		} while (!need_resched() && (backlog > backlog_stop_thresh ||
			 aq_cnt > tx_complete_stop_thresh));

		cond_resched();

		if (!octeon3_eth_tx_done_runnable(worker))
			octeon3_sso_irq_set(oen->node, oen->tx_complete_grp,
					    true);
	}

	return 0;
}

static irqreturn_t octeon3_eth_tx_handler(int irq, void *info)
{
	struct octeon3_ethernet_node *oen = info;

	/* Disarm the irq. */
	octeon3_sso_irq_set(oen->node, oen->tx_complete_grp, false);
	atomic_set(&oen->workers[0].kick, 1);
	wake_up(&oen->workers[0].queue);
	return IRQ_HANDLED;
}

static int octeon3_eth_global_init(unsigned int node,
				   struct platform_device *pdev)
{
	struct octeon3_ethernet_node *oen;
	unsigned int sso_intsn;
	int i, rv = 0;

	mutex_lock(&octeon3_eth_init_mutex);

	oen = octeon3_eth_node + node;

	if (oen->init_done)
		goto done;

	/* CN78XX-P1.0 cannot un-initialize PKO, so get a module
	 * reference to prevent it from being unloaded.
	 */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_0))
		if (!try_module_get(THIS_MODULE))
			dev_err(&pdev->dev, "ERROR: Could not obtain module reference for CN78XX-P1.0\n");

	INIT_LIST_HEAD(&oen->device_list);
	mutex_init(&oen->device_list_lock);
	spin_lock_init(&oen->napi_alloc_lock);

	oen->node = node;

	octeon_fpa3_init(node);
	rv = octeon_fpa3_pool_init(node, -1, &oen->sso_pool,
				   &oen->sso_pool_stack, 40960);
	if (rv)
		goto done;

	rv = octeon_fpa3_pool_init(node, -1, &oen->pko_pool,
				   &oen->pko_pool_stack, 40960);
	if (rv)
		goto done;

	rv = octeon_fpa3_pool_init(node, -1, &oen->pki_packet_pool,
				   &oen->pki_packet_pool_stack,
				   64 * num_packet_buffers);
	if (rv)
		goto done;

	rv = octeon_fpa3_aura_init(node, oen->sso_pool, -1,
				   &oen->sso_aura, num_packet_buffers, 20480);
	if (rv)
		goto done;

	rv = octeon_fpa3_aura_init(node, oen->pko_pool, -1,
				   &oen->pko_aura, num_packet_buffers, 20480);
	if (rv)
		goto done;

	dev_info(&pdev->dev, "SSO:%d:%d, PKO:%d:%d\n", oen->sso_pool,
		 oen->sso_aura, oen->pko_pool, oen->pko_aura);

	if (!octeon3_eth_sso_pko_cache) {
		octeon3_eth_sso_pko_cache = kmem_cache_create("sso_pko", 4096,
							      128, 0, NULL);
		if (!octeon3_eth_sso_pko_cache) {
			rv = -ENOMEM;
			goto done;
		}
	}

	rv = octeon_fpa3_mem_fill(node, octeon3_eth_sso_pko_cache,
				  oen->sso_aura, 1024);
	if (rv)
		goto done;

	rv = octeon_fpa3_mem_fill(node, octeon3_eth_sso_pko_cache,
				  oen->pko_aura, 1024);
	if (rv)
		goto done;

	rv = octeon3_sso_init(node, oen->sso_aura);
	if (rv)
		goto done;

	oen->tx_complete_grp = octeon3_sso_alloc_groups(node, NULL, 1, -1);
	if (oen->tx_complete_grp < 0)
		goto done;

	sso_intsn = SSO_IRQ_START | oen->tx_complete_grp;
	oen->tx_irq = irq_create_mapping(NULL, sso_intsn);
	if (!oen->tx_irq) {
		rv = -ENODEV;
		goto done;
	}

	rv = octeon3_pko_init_global(node, oen->pko_aura);
	if (rv) {
		rv = -ENODEV;
		goto done;
	}

	octeon3_pki_vlan_init(node);
	octeon3_pki_cluster_init(node, pdev);
	octeon3_pki_ltype_init(node);
	octeon3_pki_enable(node);

	for (i = 0; i < ARRAY_SIZE(oen->workers); i++) {
		oen->workers[i].oen = oen;
		init_waitqueue_head(&oen->workers[i].queue);
		oen->workers[i].order = i;
	}
	for (i = 0; i < ARRAY_SIZE(oen->workers); i++) {
		oen->workers[i].task =
			kthread_create_on_node(octeon3_eth_tx_complete_worker,
					       oen->workers + i, node,
					       "oct3_eth/%d:%d", node, i);
		if (IS_ERR(oen->workers[i].task)) {
			rv = PTR_ERR(oen->workers[i].task);
			goto done;
		} else {
#ifdef CONFIG_NUMA
			set_cpus_allowed_ptr(oen->workers[i].task,
					     cpumask_of_node(node));
#endif
			wake_up_process(oen->workers[i].task);
		}
	}

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X))
		octeon3_sso_pass1_limit(node, oen->tx_complete_grp);

	rv = request_irq(oen->tx_irq, octeon3_eth_tx_handler,
			 IRQ_TYPE_EDGE_RISING, "oct3_eth_tx_done", oen);
	if (rv)
		goto done;
	octeon3_eth_gen_affinity(node, &oen->tx_affinity_hint);
	irq_set_affinity_hint(oen->tx_irq, &oen->tx_affinity_hint);

	octeon3_sso_irq_set(node, oen->tx_complete_grp, true);

	oen->init_done = true;
done:
	mutex_unlock(&octeon3_eth_init_mutex);
	return rv;
}

static struct sk_buff *octeon3_eth_work_to_skb(void *w)
{
	struct sk_buff *skb;
	void **f = w;

	skb = f[-16];
	return skb;
}

/* octeon3_napi_alloc_cpu - Find an available cpu. This function must be called
 *			    with the napi_alloc_lock lock held.
 * @node:		    Node to allocate cpu from.
 * @cpu:		    Cpu to bind the napi to:
 *				<  0: use any cpu.
 *				>= 0: use requested cpu.
 *
 * Returns cpu number.
 * Returns <0 for error codes.
 */
static int octeon3_napi_alloc_cpu(int node, int cpu)
{
	int min_cnt = MAX_NAPIS_PER_NODE;
	int min_cpu = -EBUSY;

	if (cpu >= 0) {
		min_cpu = cpu;
	} else {
		for_each_cpu(cpu, cpumask_of_node(node)) {
			if (octeon3_cpu_napi_cnt[cpu] == 0) {
				min_cpu = cpu;
				break;
			} else if (octeon3_cpu_napi_cnt[cpu] < min_cnt) {
				min_cnt = octeon3_cpu_napi_cnt[cpu];
				min_cpu = cpu;
			}
		}
	}

	if (min_cpu < 0)
		return min_cpu;

	octeon3_cpu_napi_cnt[min_cpu]++;

	return min_cpu;
}

/* octeon3_napi_alloc - Allocate a napi.
 * @cxt: Receive context the napi will be added to.
 * @idx: Napi index within the receive context.
 * @cpu: Cpu to bind the napi to:
 *		<  0: use any cpu.
 *		>= 0: use requested cpu.
 *
 * Returns pointer to napi wrapper.
 * Returns NULL on error.
 */
static struct octeon3_napi_wrapper *octeon3_napi_alloc(struct octeon3_rx *cxt,
						       int idx, int cpu)
{
	struct octeon3_ethernet *priv = cxt->parent;
	struct octeon3_ethernet_node *oen;
	int i, node = priv->node;
	unsigned long flags;

	oen = octeon3_eth_node + node;
	spin_lock_irqsave(&oen->napi_alloc_lock, flags);

	/* Find a free napi wrapper */
	for (i = 0; i < MAX_NAPIS_PER_NODE; i++) {
		if (napi_wrapper[node][i].available) {
			/* Allocate a cpu to use */
			cpu = octeon3_napi_alloc_cpu(node, cpu);
			if (cpu < 0)
				break;

			napi_wrapper[node][i].available = 0;
			napi_wrapper[node][i].idx = idx;
			napi_wrapper[node][i].cpu = cpu;
			napi_wrapper[node][i].cxt = cxt;
			spin_unlock_irqrestore(&oen->napi_alloc_lock, flags);
			return &napi_wrapper[node][i];
		}
	}

	spin_unlock_irqrestore(&oen->napi_alloc_lock, flags);
	return NULL;
}

/* octeon_cpu_napi_sched - Schedule a napi for execution. The napi will start
 *			   executing on the cpu calling this function.
 * @info: Pointer to the napi to schedule for execution.
 */
static void octeon_cpu_napi_sched(void *info)
{
	struct napi_struct *napi = info;

	napi_schedule(napi);
}

/* octeon3_rm_napi_from_cxt - Remove a napi from a receive context.
 * @node: Node napi belongs to.
 * @napiw: Pointer to napi to remove.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
static int octeon3_rm_napi_from_cxt(int node,
				    struct octeon3_napi_wrapper *napiw)
{
	struct octeon3_ethernet_node *oen;
	struct octeon3_rx *cxt;
	unsigned long flags;
	int idx;

	oen = octeon3_eth_node + node;
	cxt = napiw->cxt;
	idx = napiw->idx;

	/* Free the napi block */
	spin_lock_irqsave(&oen->napi_alloc_lock, flags);
	octeon3_cpu_napi_cnt[napiw->cpu]--;
	napiw->available = 1;
	napiw->idx = -1;
	napiw->cpu = -1;
	napiw->cxt = NULL;
	spin_unlock_irqrestore(&oen->napi_alloc_lock, flags);

	/* Free the napi idx */
	spin_lock_irqsave(&cxt->napi_idx_lock, flags);
	bitmap_clear(cxt->napi_idx_bitmap, idx, 1);
	spin_unlock_irqrestore(&cxt->napi_idx_lock, flags);

	return 0;
}

/* octeon3_add_napi_to_cxt - Add a napi to a receive context.
 * @cxt: Pointer to receive context.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
static int octeon3_add_napi_to_cxt(struct octeon3_rx *cxt)
{
	struct octeon3_ethernet *priv = cxt->parent;
	struct octeon3_napi_wrapper *napiw;
	unsigned long flags;
	int idx, rc;

	/* Get a free napi idx */
	spin_lock_irqsave(&cxt->napi_idx_lock, flags);
	idx = find_first_zero_bit(cxt->napi_idx_bitmap, MAX_CORES);
	if (unlikely(idx >= MAX_CORES)) {
		spin_unlock_irqrestore(&cxt->napi_idx_lock, flags);
		return -ENOMEM;
	}
	bitmap_set(cxt->napi_idx_bitmap, idx, 1);
	spin_unlock_irqrestore(&cxt->napi_idx_lock, flags);

	/* Get a free napi block */
	napiw = octeon3_napi_alloc(cxt, idx, -1);
	if (unlikely(!napiw)) {
		spin_lock_irqsave(&cxt->napi_idx_lock, flags);
		bitmap_clear(cxt->napi_idx_bitmap, idx, 1);
		spin_unlock_irqrestore(&cxt->napi_idx_lock, flags);
		return -ENOMEM;
	}

	rc = smp_call_function_single(napiw->cpu, octeon_cpu_napi_sched,
				      &napiw->napi, 0);
	if (unlikely(rc))
		octeon3_rm_napi_from_cxt(priv->node, napiw);

	return rc;
}

/* Receive one packet.
 * returns the number of RX buffers consumed.
 */
static int octeon3_eth_rx_one(struct octeon3_rx *rx, bool is_async,
			      bool req_next)
{
	struct octeon3_ethernet *priv = rx->parent;
	int len_remaining, ret, segments;
	union buf_ptr packet_ptr;
	unsigned int packet_len;
	struct sk_buff *skb;
	struct wqe *work;
	struct wr_ret r;
	void **buf;
	u64 gaura;
	u8 *data;

	if (is_async)
		r = octeon3_core_get_response_async();
	else
		r = octeon3_core_get_work_sync(rx->rx_grp);
	work = r.work;
	if (!work)
		return 0;

	/* Request the next work so it'll be ready when we need it */
	if (is_async && req_next)
		octeon3_core_get_work_async(rx->rx_grp);

	skb = octeon3_eth_work_to_skb(work);

	/* Save the aura and node this skb came from to allow the pko to free
	 * the skb back to the correct aura. A magic number is also added to
	 * later verify the skb came from the fpa.
	 *
	 *  63                                    12 11  10 9                  0
	 * ---------------------------------------------------------------------
	 * |                  magic                 | node |        aura       |
	 * ---------------------------------------------------------------------
	 */
	buf = (void **)PTR_ALIGN(skb->head, 128);
	gaura = SKB_AURA_MAGIC | work->word0.aura;
	buf[SKB_AURA_OFFSET] = (void *)gaura;

	segments = work->word0.bufs;
	ret = segments;
	packet_ptr = work->packet_ptr;
	if (unlikely(work->word2.err_level <= PKI_ERRLEV_LA &&
		     work->word2.err_code != PKI_OPCODE_NONE)) {
		atomic64_inc(&priv->rx_errors);
		switch (work->word2.err_code) {
		case PKI_OPCODE_JABBER:
			atomic64_inc(&priv->rx_length_errors);
			break;
		case PKI_OPCODE_FCS:
			atomic64_inc(&priv->rx_crc_errors);
			break;
		}
		data = phys_to_virt(packet_ptr.addr);
		for (;;) {
			dev_kfree_skb_any(skb);
			segments--;
			if (segments <= 0)
				break;
			packet_ptr.u64 = *(u64 *)(data - 8);
#ifndef __LITTLE_ENDIAN
			if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
				/* PKI_BUFLINK_S's are endian-swapped */
				packet_ptr.u64 = swab64(packet_ptr.u64);
			}
#endif
			data = phys_to_virt(packet_ptr.addr);
			skb = octeon3_eth_work_to_skb((void *)round_down((unsigned long)data, 128ull));
		}
		goto out;
	}

	packet_len = work->word1.len;
	data = phys_to_virt(packet_ptr.addr);
	skb->data = data;
	skb->len = packet_len;
	len_remaining = packet_len;
	if (segments == 1) {
		/* Strip the ethernet fcs */
		skb->len -= 4;
		skb_set_tail_pointer(skb, skb->len);
	} else {
		bool first_frag = true;
		struct sk_buff *current_skb = skb;
		struct sk_buff *next_skb = NULL;
		unsigned int segment_size;

		skb_frag_list_init(skb);
		for (;;) {
			segment_size = (segments == 1) ?
				len_remaining : packet_ptr.size;
			len_remaining -= segment_size;
			if (!first_frag) {
				current_skb->len = segment_size;
				skb->data_len += segment_size;
				skb->truesize += current_skb->truesize;
			}
			skb_set_tail_pointer(current_skb, segment_size);
			segments--;
			if (segments == 0)
				break;
			packet_ptr.u64 = *(u64 *)(data - 8);
#ifndef __LITTLE_ENDIAN
			if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
				/* PKI_BUFLINK_S's are endian-swapped */
				packet_ptr.u64 = swab64(packet_ptr.u64);
			}
#endif
			data = phys_to_virt(packet_ptr.addr);
			next_skb = octeon3_eth_work_to_skb((void *)round_down((unsigned long)data, 128ull));
			if (first_frag) {
				next_skb->next =
					skb_shinfo(current_skb)->frag_list;
				skb_shinfo(current_skb)->frag_list = next_skb;
			} else {
				current_skb->next = next_skb;
				next_skb->next = NULL;
			}
			current_skb = next_skb;
			first_frag = false;
			current_skb->data = data;
		}

		/* Strip the ethernet fcs */
		pskb_trim(skb, skb->len - 4);
	}

	if (likely(priv->netdev->flags & IFF_UP)) {
		skb_checksum_none_assert(skb);
		if (unlikely(priv->rx_timestamp_hw)) {
			/* The first 8 bytes are the timestamp */
			u64 hwts = *(u64 *)skb->data;
			u64 ns;
			struct skb_shared_hwtstamps *shts;

			ns = timecounter_cyc2time(&priv->tc, hwts);
			shts = skb_hwtstamps(skb);
			memset(shts, 0, sizeof(*shts));
			shts->hwtstamp = ns_to_ktime(ns);
			__skb_pull(skb, 8);
		}

		skb->protocol = eth_type_trans(skb, priv->netdev);
		skb->dev = priv->netdev;
		if (priv->netdev->features & NETIF_F_RXCSUM) {
			if ((work->word2.lc_hdr_type == PKI_LTYPE_IP4 ||
			     work->word2.lc_hdr_type == PKI_LTYPE_IP6) &&
			    (work->word2.lf_hdr_type == PKI_LTYPE_TCP ||
			     work->word2.lf_hdr_type == PKI_LTYPE_UDP ||
			     work->word2.lf_hdr_type == PKI_LTYPE_SCTP))
				if (work->word2.err_code == 0)
					skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		netif_receive_skb(skb);
	} else {
		/* Drop any packet received for a device that isn't up */
		atomic64_inc(&priv->rx_dropped);
		dev_kfree_skb_any(skb);
	}
out:
	return ret;
}

static int octeon3_eth_napi(struct napi_struct *napi, int budget)
{
	int idx, napis_inuse, n = 0, n_bufs = 0, rx_count = 0;
	struct octeon3_napi_wrapper *napiw;
	struct octeon3_ethernet *priv;
	u64 aq_cnt, old_scratch;
	struct octeon3_rx *cxt;

	napiw = container_of(napi, struct octeon3_napi_wrapper, napi);
	cxt = napiw->cxt;
	priv = cxt->parent;

	/* Get the amount of work pending */
	aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(priv->node, cxt->rx_grp));
	aq_cnt &= SSO_GRP_AQ_CNT_AQ_CNT_MASK;
	/* Allow the last thread to add/remove threads if the work
	 * incremented/decremented by more than what the current number
	 * of threads can support.
	 */
	idx = find_last_bit(cxt->napi_idx_bitmap, MAX_CORES);
	napis_inuse = bitmap_weight(cxt->napi_idx_bitmap, MAX_CORES);

	if (napiw->idx == idx) {
		if (aq_cnt > napis_inuse * 128) {
			octeon3_add_napi_to_cxt(cxt);
		} else if (napiw->idx > 0 && aq_cnt < (napis_inuse - 1) * 128) {
			napi_complete(napi);
			octeon3_rm_napi_from_cxt(priv->node, napiw);
			return 0;
		}
	}

	if (likely(USE_ASYNC_IOBDMA)) {
		/* Save scratch in case userspace is using it */
		CVMX_SYNCIOBDMA;
		old_scratch = scratch_read64(0);

		octeon3_core_get_work_async(cxt->rx_grp);
	}

	while (rx_count < budget) {
		n = 0;

		if (likely(USE_ASYNC_IOBDMA)) {
			bool req_next = rx_count < (budget - 1) ? true : false;

			n = octeon3_eth_rx_one(cxt, true, req_next);
		} else {
			n = octeon3_eth_rx_one(cxt, false, false);
		}

		if (n == 0)
			break;

		n_bufs += n;
		rx_count++;
	}

	/* Wake up worker threads */
	n_bufs = atomic64_add_return(n_bufs, &priv->buffers_needed);
	if (n_bufs >= 32) {
		struct octeon3_ethernet_node *oen;

		oen = octeon3_eth_node + priv->node;
		atomic_set(&oen->workers[0].kick, 1);
		wake_up(&oen->workers[0].queue);
	}

	/* Stop the thread when no work is pending */
	if (rx_count < budget) {
		napi_complete(napi);

		if (napiw->idx > 0)
			octeon3_rm_napi_from_cxt(priv->node, napiw);
		else
			octeon3_sso_irq_set(cxt->parent->node, cxt->rx_grp,
					    true);
	}

	if (likely(USE_ASYNC_IOBDMA)) {
		/* Restore the scratch area */
		scratch_write64(0, old_scratch);
	}

	return rx_count;
}

/* octeon3_napi_init_node - Initialize the node napis.
 * @node: Node napis belong to.
 * @netdev: Default network device used to initialize the napis.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
static int octeon3_napi_init_node(int node, struct net_device *netdev)
{
	struct octeon3_ethernet_node *oen;
	unsigned long flags;
	int i;

	oen = octeon3_eth_node + node;
	spin_lock_irqsave(&oen->napi_alloc_lock, flags);

	if (oen->napi_init_done)
		goto done;

	for (i = 0; i < MAX_NAPIS_PER_NODE; i++) {
		netif_napi_add(netdev, &napi_wrapper[node][i].napi,
			       octeon3_eth_napi, 32);
		napi_enable(&napi_wrapper[node][i].napi);
		napi_wrapper[node][i].available = 1;
		napi_wrapper[node][i].idx = -1;
		napi_wrapper[node][i].cpu = -1;
		napi_wrapper[node][i].cxt = NULL;
	}

	oen->napi_init_done = true;
done:
	spin_unlock_irqrestore(&oen->napi_alloc_lock, flags);
	return 0;
}

#undef BROKEN_SIMULATOR_CSUM

static void ethtool_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "octeon3-ethernet");
	strcpy(info->version, "1.0");
	strcpy(info->bus_info, "Builtin");
}

static int ethtool_get_ts_info(struct net_device *ndev,
			       struct ethtool_ts_info *info)
{
	struct octeon3_ethernet *priv = netdev_priv(ndev);

	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;

	if (priv->ptp_clock)
		info->phc_index = ptp_clock_index(priv->ptp_clock);
	else
		info->phc_index = -1;

	info->tx_types = (1 << HWTSTAMP_TX_OFF) | (1 << HWTSTAMP_TX_ON);

	info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_ALL);

	return 0;
}

static const struct ethtool_ops octeon3_ethtool_ops = {
	.get_drvinfo = ethtool_get_drvinfo,
	.get_link_ksettings = bgx_port_ethtool_get_link_ksettings,
	.set_settings = bgx_port_ethtool_set_settings,
	.nway_reset = bgx_port_ethtool_nway_reset,
	.get_link = ethtool_op_get_link,
	.get_ts_info = ethtool_get_ts_info,
};

static int octeon3_eth_ndo_change_mtu(struct net_device *netdev, int new_mtu)
{
	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
		struct octeon3_ethernet *priv = netdev_priv(netdev);
		int fifo_size, max_mtu = 1500;

		/* On 78XX-Pass1 the mtu must be limited.  The PKO may
		 * to lock up when calculating the L4 checksum for
		 * large packets. How large the packets can be depends
		 * on the amount of pko fifo assigned to the port.
		 *
		 *   FIFO size                Max frame size
		 *	2.5 KB				1920
		 *	5.0 KB				4480
		 *     10.0 KB				9600
		 *
		 * The maximum mtu is set to the largest frame size minus the
		 * l2 header.
		 */
		fifo_size = octeon3_pko_get_fifo_size(priv->node,
						      priv->interface,
						      priv->index,
						      priv->mac_type);

		switch (fifo_size) {
		case 2560:
			max_mtu = 1920 - ETH_HLEN - ETH_FCS_LEN -
				(2 * VLAN_HLEN);
			break;

		case 5120:
			max_mtu = 4480 - ETH_HLEN - ETH_FCS_LEN -
				(2 * VLAN_HLEN);
			break;

		case 10240:
			max_mtu = 9600 - ETH_HLEN - ETH_FCS_LEN -
				(2 * VLAN_HLEN);
			break;

		default:
			break;
		}
		if (new_mtu > max_mtu) {
			netdev_warn(netdev, "Maximum MTU supported is %d",
				    max_mtu);
			return -EINVAL;
		}
	}
	return bgx_port_change_mtu(netdev, new_mtu);
}

static int octeon3_eth_common_ndo_init(struct net_device *netdev,
				       int extra_skip)
{
	int aura, base_rx_grp[MAX_RX_CONTEXTS], dq, i, pki_chan, r;
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_ethernet_node *oen = octeon3_eth_node + priv->node;

	netif_carrier_off(netdev);

	netdev->features |=
#ifndef BROKEN_SIMULATOR_CSUM
		NETIF_F_IP_CSUM |
		NETIF_F_IPV6_CSUM |
#endif
		NETIF_F_SG |
		NETIF_F_FRAGLIST |
		NETIF_F_RXCSUM |
		NETIF_F_LLTX;

	if (!OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X))
		netdev->features |= NETIF_F_SCTP_CRC;

	netdev->features |= NETIF_F_TSO | NETIF_F_TSO6;

	/* Set user changeable settings */
	netdev->hw_features = netdev->features;

	priv->rx_buf_count = num_packet_buffers;

	pki_chan = get_pki_chan(priv->node, priv->interface, priv->index);

	dq = octeon3_pko_interface_init(priv->node, priv->interface,
					priv->index, priv->mac_type, pki_chan);
	if (dq < 0) {
		dev_err(netdev->dev.parent, "Failed to initialize pko\n");
		return -ENODEV;
	}

	r = octeon3_pko_activate_dq(priv->node, dq, 1);
	if (r < 0) {
		dev_err(netdev->dev.parent, "Failed to activate dq\n");
		return -ENODEV;
	}

	priv->pko_queue = dq;
	octeon_fpa3_aura_init(priv->node, oen->pki_packet_pool, -1, &aura,
			      num_packet_buffers, num_packet_buffers * 2);
	priv->pki_aura = aura;
	aura2bufs_needed[priv->node][priv->pki_aura] = &priv->buffers_needed;

	r = octeon3_sso_alloc_groups(priv->node, base_rx_grp, rx_contexts, -1);
	if (r) {
		dev_err(netdev->dev.parent, "Failed to allocated SSO group\n");
		return -ENODEV;
	}
	for (i = 0; i < rx_contexts; i++) {
		priv->rx_cxt[i].rx_grp = base_rx_grp[i];
		priv->rx_cxt[i].parent = priv;

		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X))
			octeon3_sso_pass1_limit(priv->node,
						priv->rx_cxt[i].rx_grp);
	}
	priv->num_rx_cxt = rx_contexts;

	priv->tx_complete_grp = oen->tx_complete_grp;
	dev_info(netdev->dev.parent,
		 "rx sso grp:%d..%d aura:%d pknd:%d pko_queue:%d\n",
		 *base_rx_grp, *(base_rx_grp + priv->num_rx_cxt - 1),
		 priv->pki_aura, priv->pknd, priv->pko_queue);

	octeon3_pki_port_init(priv->node, priv->pki_aura, *base_rx_grp,
			      extra_skip, (packet_buffer_size - 128),
			      priv->pknd, priv->num_rx_cxt);

	priv->last_packets = 0;
	priv->last_octets = 0;
	priv->last_dropped = 0;

	octeon3_napi_init_node(priv->node, netdev);

	/* Register ethtool methods */
	netdev->ethtool_ops = &octeon3_ethtool_ops;

	return 0;
}

static int octeon3_eth_bgx_ndo_init(struct net_device *netdev)
{
	struct octeon3_ethernet	*priv = netdev_priv(netdev);
	const u8 *mac;
	int r;

	priv->pknd = bgx_port_get_pknd(priv->node, priv->interface,
				       priv->index);
	octeon3_eth_common_ndo_init(netdev, 0);

	/* Padding and FCS are done in BGX */
	r = octeon3_pko_set_mac_options(priv->node, priv->interface,
					priv->index, priv->mac_type, false,
					false, 0);
	if (r)
		return r;

	mac = bgx_port_get_mac(netdev);
	if (mac && is_valid_ether_addr(mac)) {
		memcpy(netdev->dev_addr, mac, ETH_ALEN);
		netdev->addr_assign_type &= ~NET_ADDR_RANDOM;
	} else {
		eth_hw_addr_random(netdev);
	}

	bgx_port_set_rx_filtering(netdev);
	octeon3_eth_ndo_change_mtu(netdev, netdev->mtu);

	return 0;
}

static void octeon3_eth_ndo_uninit(struct net_device *netdev)
{
	struct octeon3_ethernet	*priv = netdev_priv(netdev);
	int grp[MAX_RX_CONTEXTS], i;

	/* Shutdwon pki for this interface */
	octeon3_pki_port_shutdown(priv->node, priv->pknd);
	octeon_fpa3_release_aura(priv->node, priv->pki_aura);
	aura2bufs_needed[priv->node][priv->pki_aura] = NULL;

	/* Shutdown pko for this interface */
	octeon3_pko_interface_uninit(priv->node, &priv->pko_queue, 1);

	/* Free the receive contexts sso groups */
	for (i = 0; i < rx_contexts; i++)
		grp[i] = priv->rx_cxt[i].rx_grp;
	octeon3_sso_free_groups(priv->node, grp, rx_contexts);
}

static irqreturn_t octeon3_eth_rx_handler(int irq, void *info)
{
	struct octeon3_rx *rx = info;

	/* Disarm the irq. */
	octeon3_sso_irq_set(rx->parent->node, rx->rx_grp, false);

	napi_schedule(&rx->napiw->napi);
	return IRQ_HANDLED;
}

static int octeon3_eth_common_ndo_open(struct net_device *netdev)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_rx *rx;
	int i, idx, r;

	for (i = 0; i < priv->num_rx_cxt; i++) {
		unsigned int sso_intsn;
		int cpu;

		rx = priv->rx_cxt + i;
		sso_intsn = SSO_IRQ_START | rx->rx_grp;

		spin_lock_init(&rx->napi_idx_lock);

		rx->rx_irq = irq_create_mapping(NULL, sso_intsn);
		if (!rx->rx_irq) {
			netdev_err(netdev, "ERROR: Couldn't map hwirq: %x\n",
				   sso_intsn);
			r = -EINVAL;
			goto err1;
		}
		r = request_irq(rx->rx_irq, octeon3_eth_rx_handler,
				IRQ_TYPE_EDGE_RISING, netdev_name(netdev), rx);
		if (r) {
			netdev_err(netdev, "ERROR: Couldn't request irq: %d\n",
				   rx->rx_irq);
			r = -ENOMEM;
			goto err2;
		}

		octeon3_eth_gen_affinity(priv->node, &rx->rx_affinity_hint);
		irq_set_affinity_hint(rx->rx_irq, &rx->rx_affinity_hint);

		/* Allocate a napi index for this receive context */
		bitmap_zero(priv->rx_cxt[i].napi_idx_bitmap, MAX_CORES);
		idx = find_first_zero_bit(priv->rx_cxt[i].napi_idx_bitmap,
					  MAX_CORES);
		if (idx >= MAX_CORES) {
			netdev_err(netdev, "ERROR: Couldn't get napi index\n");
			r = -ENOMEM;
			goto err3;
		}
		bitmap_set(priv->rx_cxt[i].napi_idx_bitmap, idx, 1);
		cpu = cpumask_first(&rx->rx_affinity_hint);

		priv->rx_cxt[i].napiw = octeon3_napi_alloc(&priv->rx_cxt[i],
							   idx, cpu);
		if (!priv->rx_cxt[i].napiw) {
			r = -ENOMEM;
			goto err4;
		}

		/* Arm the irq. */
		octeon3_sso_irq_set(priv->node, rx->rx_grp, true);
	}
	octeon3_eth_replenish_rx(priv, priv->rx_buf_count);

	return 0;

err4:
	bitmap_clear(priv->rx_cxt[i].napi_idx_bitmap, idx, 1);
err3:
	irq_set_affinity_hint(rx->rx_irq, NULL);
	free_irq(rx->rx_irq, rx);
err2:
	irq_dispose_mapping(rx->rx_irq);
err1:
	for (i--; i >= 0; i--) {
		rx = priv->rx_cxt + i;
		irq_dispose_mapping(rx->rx_irq);
		free_irq(rx->rx_irq, rx);
		octeon3_rm_napi_from_cxt(priv->node, priv->rx_cxt[i].napiw);
		priv->rx_cxt[i].napiw = NULL;
	}

	return r;
}

static int octeon3_eth_bgx_ndo_open(struct net_device *netdev)
{
	int rc;

	rc = octeon3_eth_common_ndo_open(netdev);
	if (rc == 0)
		rc = bgx_port_enable(netdev);

	return rc;
}

static int octeon3_eth_common_ndo_stop(struct net_device *netdev)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_rx *rx;
	struct sk_buff *skb;
	void **w;
	int i;

	/* Allow enough time for ingress in transit packets to be drained */
	msleep(20);

	/* Wait until sso has no more work for this interface */
	for (i = 0; i < priv->num_rx_cxt; i++) {
		rx = priv->rx_cxt + i;
		while (oct_csr_read(SSO_GRP_AQ_CNT(priv->node, rx->rx_grp)))
			msleep(20);
	}

	/* Free the irq and napi context for each rx context */
	for (i = 0; i < priv->num_rx_cxt; i++) {
		rx = priv->rx_cxt + i;
		octeon3_sso_irq_set(priv->node, rx->rx_grp, false);
		irq_set_affinity_hint(rx->rx_irq, NULL);
		free_irq(rx->rx_irq, rx);
		irq_dispose_mapping(rx->rx_irq);
		rx->rx_irq = 0;

		octeon3_rm_napi_from_cxt(priv->node, rx->napiw);
		rx->napiw = NULL;
		WARN_ON(!bitmap_empty(rx->napi_idx_bitmap, MAX_CORES));
	}

	/* Free the packet buffers */
	for (;;) {
		w = octeon_fpa3_alloc(priv->node, priv->pki_aura);
		if (!w)
			break;
		skb = w[0];
		dev_kfree_skb(skb);
	}

	return 0;
}

static int octeon3_eth_bgx_ndo_stop(struct net_device *netdev)
{
	int r;

	r = bgx_port_disable(netdev);
	if (r)
		return r;

	return octeon3_eth_common_ndo_stop(netdev);
}

static inline u64 build_pko_send_hdr_desc(struct sk_buff *skb, int gaura)
{
	u64 checksum_alg, send_hdr = 0;
	u8 l4_hdr = 0;

	/* See PKO_SEND_HDR_S in the HRM for the send header descriptor
	 * format.
	 */
#ifdef __LITTLE_ENDIAN
	send_hdr |= PKO_SEND_HDR_LE;
#endif

	if (!OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
		/* Don't allocate to L2 */
		send_hdr |= PKO_SEND_HDR_N2;
	}

	/* Don't automatically free to FPA */
	send_hdr |= PKO_SEND_HDR_DF;

	send_hdr |= skb->len;
	send_hdr |= (u64)gaura << PKO_SEND_HDR_AURA_SHIFT;

	if (skb->ip_summed != CHECKSUM_NONE &&
	    skb->ip_summed != CHECKSUM_UNNECESSARY) {
#ifndef BROKEN_SIMULATOR_CSUM
		switch (skb->protocol) {
		case htons(ETH_P_IP):
			send_hdr |= ETH_HLEN << PKO_SEND_HDR_L3PTR_SHIFT;
			send_hdr |= PKO_SEND_HDR_CKL3;
			l4_hdr = ip_hdr(skb)->protocol;
			send_hdr |= (ETH_HLEN + (4 * ip_hdr(skb)->ihl)) <<
				    PKO_SEND_HDR_L4PTR_SHIFT;
			break;

		case htons(ETH_P_IPV6):
			l4_hdr = ipv6_hdr(skb)->nexthdr;
			send_hdr |= ETH_HLEN << PKO_SEND_HDR_L3PTR_SHIFT;
			break;

		default:
			break;
		}
#endif

		checksum_alg = 1; /* UDP == 1 */
		switch (l4_hdr) {
		case IPPROTO_SCTP:
			if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X))
				break;
			checksum_alg++; /* SCTP == 3 */
			/* Fall through */
		case IPPROTO_TCP: /* TCP == 2 */
			checksum_alg++;
			/* Fall through */
		case IPPROTO_UDP:
			if (skb_transport_header_was_set(skb)) {
				int l4ptr = skb_transport_header(skb) -
					skb->data;
				send_hdr &= ~PKO_SEND_HDR_L4PTR_MASK;
				send_hdr |= l4ptr << PKO_SEND_HDR_L4PTR_SHIFT;
				send_hdr |= checksum_alg <<
					    PKO_SEND_HDR_CKL4_SHIFT;
			}
			break;

		default:
			break;
		}
	}

	return send_hdr;
}

static inline u64 build_pko_send_ext_desc(struct sk_buff *skb)
{
	u64 send_ext;

	/* See PKO_SEND_EXT_S in the HRM for the send extended descriptor
	 * format.
	 */
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	send_ext = (u64)PKO_SENDSUBDC_EXT << PKO_SEND_SUBDC4_SHIFT;
	send_ext |= (u64)PKO_REDALG_E_SEND << PKO_SEND_EXT_RA_SHIFT;
	send_ext |= PKO_SEND_EXT_TSTMP;
	send_ext |= ETH_HLEN << PKO_SEND_EXT_MARKPTR_SHIFT;

	return send_ext;
}

static inline u64 build_pko_send_tso(struct sk_buff *skb, uint mtu)
{
	u64 send_tso;

	/* See PKO_SEND_TSO_S in the HRM for the send tso descriptor format */
	send_tso = 12ull << PKO_SEND_TSO_L2LEN_SHIFT;
	send_tso |= (u64)PKO_SENDSUBDC_TSO << PKO_SEND_SUBDC4_SHIFT;
	send_tso |= (skb_transport_offset(skb) + tcp_hdrlen(skb)) <<
		    PKO_SEND_TSO_SB_SHIFT;
	send_tso |= (mtu + ETH_HLEN) << PKO_SEND_TSO_MSS_SHIFT;

	return send_tso;
}

static inline u64 build_pko_send_mem_sub(u64 addr)
{
	u64 send_mem;

	/* See PKO_SEND_MEM_S in the HRM for the send mem descriptor format */
	send_mem = (u64)PKO_SENDSUBDC_MEM << PKO_SEND_SUBDC4_SHIFT;
	send_mem |= (u64)PKO_MEMDSZ_B64 << PKO_SEND_MEM_DSZ_SHIFT;
	send_mem |= (u64)PKO_MEMALG_SUB << PKO_SEND_MEM_ALG_SHIFT;
	send_mem |= 1ull << PKO_SEND_MEM_OFFSET_SHIFT;
	send_mem |= addr;

	return send_mem;
}

static inline u64 build_pko_send_mem_ts(u64 addr)
{
	u64 send_mem;

	/* See PKO_SEND_MEM_S in the HRM for the send mem descriptor format */
	send_mem = 1ull << PKO_SEND_MEM_WMEM_SHIFT;
	send_mem |= (u64)PKO_SENDSUBDC_MEM << PKO_SEND_SUBDC4_SHIFT;
	send_mem |= (u64)PKO_MEMDSZ_B64 << PKO_SEND_MEM_DSZ_SHIFT;
	send_mem |= (u64)PKO_MEMALG_SETTSTMP << PKO_SEND_MEM_ALG_SHIFT;
	send_mem |= addr;

	return send_mem;
}

static inline u64 build_pko_send_free(u64 addr)
{
	u64 send_free;

	/* See PKO_SEND_FREE_S in the HRM for the send free descriptor format */
	send_free = (u64)PKO_SENDSUBDC_FREE << PKO_SEND_SUBDC4_SHIFT;
	send_free |= addr;

	return send_free;
}

static inline u64 build_pko_send_work(int grp, u64 addr)
{
	u64 send_work;

	/* See PKO_SEND_WORK_S in the HRM for the send work descriptor format */
	send_work = (u64)PKO_SENDSUBDC_WORK << PKO_SEND_SUBDC4_SHIFT;
	send_work |= (u64)grp << PKO_SEND_WORK_GRP_SHIFT;
	send_work |= SSO_TAG_TYPE_UNTAGGED << PKO_SEND_WORK_TT_SHIFT;
	send_work |= addr;

	return send_work;
}

static int octeon3_eth_ndo_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	u64 aq_cnt = 0, *dma_addr, head_len, lmtdma_data;
	u64 pko_send_desc, scr_off = LMTDMA_SCR_OFFSET;
	int frag_count, gaura = 0, grp, i;
	struct octeon3_ethernet_node *oen;
	struct sk_buff *skb_tmp;
	unsigned int mss;
	long backlog;
	void **work;

	frag_count = 0;
	if (skb_has_frag_list(skb))
		skb_walk_frags(skb, skb_tmp)
			frag_count++;

	/* Drop the packet if pko or sso are not keeping up */
	oen = octeon3_eth_node + priv->node;
	aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(oen->node, oen->tx_complete_grp));
	aq_cnt &= SSO_GRP_AQ_CNT_AQ_CNT_MASK;
	backlog = atomic64_inc_return(&priv->tx_backlog);
	if (unlikely(backlog > MAX_TX_QUEUE_DEPTH || aq_cnt > 100000)) {
		if (use_tx_queues) {
			netif_stop_queue(netdev);
		} else {
			atomic64_dec(&priv->tx_backlog);
			goto skip_xmit;
		}
	}

	/* We have space for 11 segment pointers, If there will be
	 * more than that, we must linearize.  The count is: 1 (base
	 * SKB) + frag_count + nr_frags.
	 */
	if (unlikely(skb_shinfo(skb)->nr_frags + frag_count > 10)) {
		if (unlikely(__skb_linearize(skb)))
			goto skip_xmit;
		frag_count = 0;
	}

	work = (void **)skb->cb;
	work[0] = netdev;
	work[1] = NULL;

	/* Adjust the port statistics. */
	atomic64_inc(&priv->tx_packets);
	atomic64_add(skb->len, &priv->tx_octets);

	/* Make sure packet data writes are committed before
	 * submitting the command below
	 */
	wmb();

	/* Build the pko command */
	pko_send_desc = build_pko_send_hdr_desc(skb, gaura);
	preempt_disable();
	scratch_write64(scr_off, pko_send_desc);
	scr_off += sizeof(pko_send_desc);

	/* Request packet to be ptp timestamped */
	if ((unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) &&
	    unlikely(priv->tx_timestamp_hw)) {
		pko_send_desc = build_pko_send_ext_desc(skb);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}

	/* Add the tso descriptor if needed */
	mss = skb_shinfo(skb)->gso_size;
	if (unlikely(mss)) {
		pko_send_desc = build_pko_send_tso(skb, netdev->mtu);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}

	/* Add a gather descriptor for each segment. See PKO_SEND_GATHER_S for
	 * the send gather descriptor format.
	 */
	pko_send_desc = 0;
	pko_send_desc |= (u64)PKO_SENDSUBDC_GATHER <<
			 PKO_SEND_GATHER_SUBDC_SHIFT;
	head_len = skb_headlen(skb);
	if (head_len > 0) {
		pko_send_desc |= head_len << PKO_SEND_GATHER_SIZE_SHIFT;
		pko_send_desc |= virt_to_phys(skb->data);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}
	for (i = 1; i <= skb_shinfo(skb)->nr_frags; i++) {
		struct skb_frag_struct *fs = skb_shinfo(skb)->frags + i - 1;

		pko_send_desc &= ~(PKO_SEND_GATHER_SIZE_MASK |
				   PKO_SEND_GATHER_ADDR_MASK);
		pko_send_desc |= (u64)fs->size << PKO_SEND_GATHER_SIZE_SHIFT;
		pko_send_desc |= virt_to_phys((u8 *)page_address(fs->page.p) +
			fs->page_offset);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}
	skb_walk_frags(skb, skb_tmp) {
		pko_send_desc &= ~(PKO_SEND_GATHER_SIZE_MASK |
				   PKO_SEND_GATHER_ADDR_MASK);
		pko_send_desc |= (u64)skb_tmp->len <<
				 PKO_SEND_GATHER_SIZE_SHIFT;
		pko_send_desc |= virt_to_phys(skb_tmp->data);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}

	/* Subtract 1 from the tx_backlog. */
	pko_send_desc = build_pko_send_mem_sub(virt_to_phys(&priv->tx_backlog));
	scratch_write64(scr_off, pko_send_desc);
	scr_off += sizeof(pko_send_desc);

	/* Write the ptp timestamp in the skb itself */
	if ((unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) &&
	    unlikely(priv->tx_timestamp_hw)) {
		pko_send_desc = build_pko_send_mem_ts(virt_to_phys(&work[1]));
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}

	/* Send work when finished with the packet. */
	grp = octeon3_eth_lgrp_to_ggrp(priv->node, priv->tx_complete_grp);
	pko_send_desc = build_pko_send_work(grp, virt_to_phys(work));
	scratch_write64(scr_off, pko_send_desc);
	scr_off += sizeof(pko_send_desc);

	/* See PKO_SEND_DMA_S in the HRM for the lmtdam data format */
	lmtdma_data = (u64)(LMTDMA_SCR_OFFSET >> PKO_LMTDMA_SCRADDR_SHIFT) <<
		      PKO_QUERY_DMA_SCRADDR_SHIFT;
	if (wait_pko_response)
		lmtdma_data |= 1ull << PKO_QUERY_DMA_RTNLEN_SHIFT;
	lmtdma_data |= 0x51ull << PKO_QUERY_DMA_DID_SHIFT;
	lmtdma_data |= (u64)priv->node << PKO_QUERY_DMA_NODE_SHIFT;
	lmtdma_data |= priv->pko_queue << PKO_QUERY_DMA_DQ_SHIFT;

	dma_addr = (u64 *)(LMTDMA_ORDERED_IO_ADDR | ((scr_off & 0x78) - 8));
	*dma_addr = lmtdma_data;

	preempt_enable();

	if (wait_pko_response) {
		u64 query_rtn;

		CVMX_SYNCIOBDMA;

		/* See PKO_QUERY_RTN_S in the HRM for the return format */
		query_rtn = scratch_read64(LMTDMA_SCR_OFFSET);
		query_rtn >>= PKO_QUERY_RTN_DQSTATUS_SHIFT;
		if (unlikely(query_rtn != PKO_DQSTATUS_PASS)) {
			netdev_err(netdev, "PKO enqueue failed %llx\n",
				   (unsigned long long)query_rtn);
			dev_kfree_skb_any(skb);
		}
	}

	return NETDEV_TX_OK;
skip_xmit:
	atomic64_inc(&priv->tx_dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void octeon3_eth_ndo_get_stats64(struct net_device *netdev,
					struct rtnl_link_stats64 *s)
{
	u64 delta_dropped, delta_octets, delta_packets, dropped;
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	u64 octets, packets;

	spin_lock(&priv->stat_lock);

	octeon3_pki_get_stats(priv->node, priv->pknd, &packets, &octets,
			      &dropped);

	delta_packets = (packets - priv->last_packets) & GENMASK_ULL(47, 0);
	delta_octets = (octets - priv->last_octets) & GENMASK_ULL(47, 0);
	delta_dropped = (dropped - priv->last_dropped) & GENMASK_ULL(47, 0);

	priv->last_packets = packets;
	priv->last_octets = octets;
	priv->last_dropped = dropped;

	spin_unlock(&priv->stat_lock);

	atomic64_add(delta_packets, &priv->rx_packets);
	atomic64_add(delta_octets, &priv->rx_octets);
	atomic64_add(delta_dropped, &priv->rx_dropped);

	s->rx_packets = atomic64_read(&priv->rx_packets);
	s->rx_bytes = atomic64_read(&priv->rx_octets);
	s->rx_dropped = atomic64_read(&priv->rx_dropped);
	s->rx_errors = atomic64_read(&priv->rx_errors);
	s->rx_length_errors = atomic64_read(&priv->rx_length_errors);
	s->rx_crc_errors = atomic64_read(&priv->rx_crc_errors);

	s->tx_packets = atomic64_read(&priv->tx_packets);
	s->tx_bytes = atomic64_read(&priv->tx_octets);
	s->tx_dropped = atomic64_read(&priv->tx_dropped);
}

static int octeon3_eth_set_mac_address(struct net_device *netdev, void *addr)
{
	int r = eth_mac_addr(netdev, addr);

	if (r)
		return r;

	bgx_port_set_rx_filtering(netdev);

	return 0;
}

static u64 octeon3_cyclecounter_read(const struct cyclecounter *cc)
{
	struct octeon3_ethernet *priv;
	u64 count;

	priv = container_of(cc, struct octeon3_ethernet, cc);
	count = oct_csr_read(MIO_PTP_CLOCK_HI(priv->node));
	return count;
}

static int octeon3_bgx_hwtstamp(struct net_device *netdev, int en)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	u64 data;

	switch (bgx_port_get_mode(priv->node, priv->interface, priv->index)) {
	case PORT_MODE_RGMII:
	case PORT_MODE_SGMII:
		data = oct_csr_read(BGX_GMP_GMI_RX_FRM_CTL(priv->node,
							   priv->interface,
							   priv->index));
		if (en)
			data |= BGX_GMP_GMI_RX_FRM_CTL_PTP_MODE;
		else
			data &= ~BGX_GMP_GMI_RX_FRM_CTL_PTP_MODE;
		oct_csr_write(data, BGX_GMP_GMI_RX_FRM_CTL(priv->node,
							   priv->interface,
							   priv->index));
		break;

	case PORT_MODE_XAUI:
	case PORT_MODE_RXAUI:
	case PORT_MODE_10G_KR:
	case PORT_MODE_XLAUI:
	case PORT_MODE_40G_KR4:
	case PORT_MODE_XFI:
		data = oct_csr_read(BGX_SMU_RX_FRM_CTL(priv->node,
						       priv->interface,
						       priv->index));
		if (en)
			data |= BGX_GMP_GMI_RX_FRM_CTL_PTP_MODE;
		else
			data &= ~BGX_GMP_GMI_RX_FRM_CTL_PTP_MODE;
		oct_csr_write(data, BGX_SMU_RX_FRM_CTL(priv->node,
						       priv->interface,
						       priv->index));
		break;

	default:
		/* No timestamp support*/
		return -EOPNOTSUPP;
	}

	return 0;
}

static int octeon3_pki_hwtstamp(struct net_device *netdev, int en)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	int skip = en ? 8 : 0;

	octeon3_pki_set_ptp_skip(priv->node, priv->pknd, skip);

	return 0;
}

static int octeon3_ioctl_hwtstamp(struct net_device *netdev, struct ifreq *rq,
				  int cmd)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct hwtstamp_config config;
	u64 data;
	int en;

	/* The PTP block should be enabled */
	data = oct_csr_read(MIO_PTP_CLOCK_CFG(priv->node));
	if (!(data & MIO_PTP_CLOCK_CFG_PTP_EN)) {
		netdev_err(netdev, "Error: PTP clock not enabled\n");
		return -EOPNOTSUPP;
	}

	if (copy_from_user(&config, rq->ifr_data, sizeof(config)))
		return -EFAULT;

	if (config.flags) /* reserved for future extensions */
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		priv->tx_timestamp_hw = 0;
		break;
	case HWTSTAMP_TX_ON:
		priv->tx_timestamp_hw = 1;
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		priv->rx_timestamp_hw = 0;
		en = 0;
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_SOME:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		priv->rx_timestamp_hw = 1;
		en = 1;
		break;
	default:
		return -ERANGE;
	}

	octeon3_bgx_hwtstamp(netdev, en);
	octeon3_pki_hwtstamp(netdev, en);

	priv->cc.read = octeon3_cyclecounter_read;
	priv->cc.mask = CYCLECOUNTER_MASK(64);
	/* Ptp counter is always in nsec */
	priv->cc.mult = 1;
	priv->cc.shift = 0;
	timecounter_init(&priv->tc, &priv->cc, ktime_to_ns(ktime_get_real()));

	return 0;
}

static int octeon3_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct octeon3_ethernet	*priv;
	int neg_ppb = 0;
	u64 comp, diff;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);

	if (ppb < 0) {
		ppb = -ppb;
		neg_ppb = 1;
	}

	/* The part per billion (ppb) is a delta from the base frequency */
	comp = (NSEC_PER_SEC << 32) / octeon_get_io_clock_rate();

	diff = comp;
	diff *= ppb;
	diff = div_u64(diff, 1000000000ULL);

	comp = neg_ppb ? comp - diff : comp + diff;

	oct_csr_write(comp, MIO_PTP_CLOCK_COMP(priv->node));

	return 0;
}

static int octeon3_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct octeon3_ethernet	*priv;
	unsigned long flags;
	s64 now;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);

	spin_lock_irqsave(&priv->ptp_lock, flags);
	now = timecounter_read(&priv->tc);
	now += delta;
	timecounter_init(&priv->tc, &priv->cc, now);
	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

static int octeon3_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct octeon3_ethernet	*priv;
	unsigned long flags;
	u32 remainder;
	u64 ns;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);

	spin_lock_irqsave(&priv->ptp_lock, flags);
	ns = timecounter_read(&priv->tc);
	spin_unlock_irqrestore(&priv->ptp_lock, flags);
	ts->tv_sec = div_u64_rem(ns, 1000000000ULL, &remainder);
	ts->tv_nsec = remainder;

	return 0;
}

static int octeon3_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct octeon3_ethernet	*priv;
	unsigned long flags;
	u64 ns;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);
	ns = timespec64_to_ns(ts);

	spin_lock_irqsave(&priv->ptp_lock, flags);
	timecounter_init(&priv->tc, &priv->cc, ns);
	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

static int octeon3_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

static int octeon3_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	int rc;

	switch (cmd) {
	case SIOCSHWTSTAMP:
		rc = octeon3_ioctl_hwtstamp(netdev, ifr, cmd);
		break;

	default:
		rc = bgx_port_do_ioctl(netdev, ifr, cmd);
		break;
	}

	return rc;
}

static const struct net_device_ops octeon3_eth_netdev_ops = {
	.ndo_init		= octeon3_eth_bgx_ndo_init,
	.ndo_uninit		= octeon3_eth_ndo_uninit,
	.ndo_open		= octeon3_eth_bgx_ndo_open,
	.ndo_stop		= octeon3_eth_bgx_ndo_stop,
	.ndo_start_xmit		= octeon3_eth_ndo_start_xmit,
	.ndo_get_stats64	= octeon3_eth_ndo_get_stats64,
	.ndo_set_rx_mode	= bgx_port_set_rx_filtering,
	.ndo_set_mac_address	= octeon3_eth_set_mac_address,
	.ndo_change_mtu		= octeon3_eth_ndo_change_mtu,
	.ndo_do_ioctl		= octeon3_ioctl,
};

static int octeon3_eth_probe(struct platform_device *pdev)
{
	struct octeon3_ethernet *priv;
	struct net_device *netdev;
	int r;

	struct mac_platform_data *pd = dev_get_platdata(&pdev->dev);

	r = octeon3_eth_global_init(pd->numa_node, pdev);
	if (r)
		return r;

	dev_info(&pdev->dev, "Probing %d-%d:%d\n", pd->numa_node, pd->interface,
		 pd->port);
	netdev = alloc_etherdev(sizeof(struct octeon3_ethernet));
	if (!netdev) {
		dev_err(&pdev->dev, "Failed to allocated ethernet device\n");
		return -ENOMEM;
	}

	/* Using transmit queues degrades performance significantly */
	if (!use_tx_queues)
		netdev->tx_queue_len = 0;

	SET_NETDEV_DEV(netdev, &pdev->dev);
	dev_set_drvdata(&pdev->dev, netdev);

	if (pd->mac_type == BGX_MAC)
		bgx_port_set_netdev(pdev->dev.parent, netdev);
	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	priv->mac_type = pd->mac_type;
	INIT_LIST_HEAD(&priv->list);
	priv->node = pd->numa_node;

	mutex_lock(&octeon3_eth_node[priv->node].device_list_lock);
	list_add_tail_rcu(&priv->list,
			  &octeon3_eth_node[priv->node].device_list);
	mutex_unlock(&octeon3_eth_node[priv->node].device_list_lock);

	priv->index = pd->port;
	priv->interface = pd->interface;
	spin_lock_init(&priv->stat_lock);

	if (pd->src_type == XCV)
		snprintf(netdev->name, IFNAMSIZ, "rgmii%d", pd->port);

	if (priv->mac_type == BGX_MAC)
		netdev->netdev_ops = &octeon3_eth_netdev_ops;

	if (register_netdev(netdev) < 0) {
		dev_err(&pdev->dev, "Failed to register ethernet device\n");
		list_del(&priv->list);
		free_netdev(netdev);
	}

	spin_lock_init(&priv->ptp_lock);
	priv->ptp_info.owner = THIS_MODULE;
	snprintf(priv->ptp_info.name, 16, "octeon3 ptp");
	priv->ptp_info.max_adj = 250000000;
	priv->ptp_info.n_alarm = 0;
	priv->ptp_info.n_ext_ts = 0;
	priv->ptp_info.n_per_out = 0;
	priv->ptp_info.pps = 0;
	priv->ptp_info.adjfreq = octeon3_adjfreq;
	priv->ptp_info.adjtime = octeon3_adjtime;
	priv->ptp_info.gettime64 = octeon3_gettime;
	priv->ptp_info.settime64 = octeon3_settime;
	priv->ptp_info.enable = octeon3_enable;
	priv->ptp_clock = ptp_clock_register(&priv->ptp_info, &pdev->dev);

	netdev_info(netdev, "Registered\n");
	return 0;
}

/* octeon3_eth_global_exit - Free all the used resources and restore the
 *			     hardware to the default state.
 * @node: Node to free/reset.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
static int octeon3_eth_global_exit(int node)
{
	struct octeon3_ethernet_node *oen = octeon3_eth_node + node;
	int i;

	/* Free the tx_complete irq */
	octeon3_sso_irq_set(node, oen->tx_complete_grp, false);
	irq_set_affinity_hint(oen->tx_irq, NULL);
	free_irq(oen->tx_irq, oen);
	irq_dispose_mapping(oen->tx_irq);
	oen->tx_irq = 0;

	/* Stop the worker threads */
	for (i = 0; i < ARRAY_SIZE(oen->workers); i++)
		kthread_stop(oen->workers[i].task);

	/* Shutdown pki */
	octeon3_pki_shutdown(node);
	octeon_fpa3_release_pool(node, oen->pki_packet_pool);
	kfree(oen->pki_packet_pool_stack);

	/* Shutdown pko */
	octeon3_pko_exit_global(node);
	for (;;) {
		void **w;

		w = octeon_fpa3_alloc(node, oen->pko_aura);
		if (!w)
			break;
		kmem_cache_free(octeon3_eth_sso_pko_cache, w);
	}
	octeon_fpa3_release_aura(node, oen->pko_aura);
	octeon_fpa3_release_pool(node, oen->pko_pool);
	kfree(oen->pko_pool_stack);

	/* Shutdown sso */
	octeon3_sso_shutdown(node, oen->sso_aura);
	octeon3_sso_free_groups(node, &oen->tx_complete_grp, 1);
	for (;;) {
		void **w;

		w = octeon_fpa3_alloc(node, oen->sso_aura);
		if (!w)
			break;
		kmem_cache_free(octeon3_eth_sso_pko_cache, w);
	}
	octeon_fpa3_release_aura(node, oen->sso_aura);
	octeon_fpa3_release_pool(node, oen->sso_pool);
	kfree(oen->sso_pool_stack);

	return 0;
}

static int octeon3_eth_remove(struct platform_device *pdev)
{
	struct mac_platform_data *pd = dev_get_platdata(&pdev->dev);
	struct net_device *netdev = dev_get_drvdata(&pdev->dev);
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_ethernet_node *oen;
	int node = priv->node;

	oen = octeon3_eth_node + node;

	ptp_clock_unregister(priv->ptp_clock);
	unregister_netdev(netdev);
	if (pd->mac_type == BGX_MAC)
		bgx_port_set_netdev(pdev->dev.parent, NULL);
	dev_set_drvdata(&pdev->dev, NULL);

	/* Free all resources when there are no more devices */
	mutex_lock(&octeon3_eth_init_mutex);
	mutex_lock(&oen->device_list_lock);
	list_del_rcu(&priv->list);
	if (oen->init_done && list_empty(&oen->device_list)) {
		int	i;

		for (i = 0; i < MAX_NAPIS_PER_NODE; i++) {
			napi_disable(&napi_wrapper[node][i].napi);
			netif_napi_del(&napi_wrapper[node][i].napi);
		}

		oen->init_done = false;
		oen->napi_init_done = false;
		octeon3_eth_global_exit(node);
	}

	mutex_unlock(&oen->device_list_lock);
	mutex_unlock(&octeon3_eth_init_mutex);
	free_netdev(netdev);

	return 0;
}

static void octeon3_eth_shutdown(struct platform_device *pdev)
{
	octeon3_eth_remove(pdev);
}

static struct platform_driver octeon3_eth_driver = {
	.probe		= octeon3_eth_probe,
	.remove		= octeon3_eth_remove,
	.shutdown       = octeon3_eth_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "ethernet-mac-pki",
	},
};

static int __init octeon3_eth_init(void)
{
	if (rx_contexts <= 0)
		rx_contexts = 1;
	if (rx_contexts > MAX_RX_CONTEXTS)
		rx_contexts = MAX_RX_CONTEXTS;

	return platform_driver_register(&octeon3_eth_driver);
}
module_init(octeon3_eth_init);

static void __exit octeon3_eth_exit(void)
{
	platform_driver_unregister(&octeon3_eth_driver);

	/* Destroy the memory cache used by sso and pko */
	kmem_cache_destroy(octeon3_eth_sso_pko_cache);
}
module_exit(octeon3_eth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@caviumnetworks.com>");
MODULE_DESCRIPTION("Cavium, Inc. PKI/PKO Ethernet driver.");
