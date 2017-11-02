/*
 * Copyright (c) 2017 Cavium, Inc.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/rculist.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_vlan.h>
#include <linux/rio_drv.h>
#include <linux/rio_ids.h>
#include <linux/net_tstamp.h>
#include <linux/timecounter.h>
#include <linux/ptp_clock_kernel.h>

#include <asm/octeon/octeon.h>

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

#define MAX_TX_QUEUE_DEPTH 512
#define SSO_INTSN_EXE 0x61
#define MAX_RX_QUEUES 32

#define SKB_PTR_OFFSET		0

#define MAX_CORES		48
#define FPA3_NUM_AURAS		1024

#define USE_ASYNC_IOBDMA	1
#define SCR_SCRATCH		0ull
#define SSO_NO_WAIT		0ull
#define DID_TAG_SWTAG		0x60ull
#define IOBDMA_SENDSINGLE	0xffffffffffffa200ull

/* Values for the value of wqe word2 [ERRLEV] */
#define PKI_ERRLEV_LA		0x01

/* Values for the value of wqe word2 [OPCODE] */
#define PKI_OPCODE_NONE		0x00
#define PKI_OPCODE_JABBER	0x02
#define PKI_OPCODE_FCS		0x07

/* Values for the layer type in the wqe */
#define PKI_LTYPE_IP4		0x08
#define PKI_LTYPE_IP6		0x0a
#define PKI_LTYPE_TCP		0x10
#define PKI_LTYPE_UDP		0x11
#define PKI_LTYPE_SCTP		0x12

/* Registers are accessed via xkphys */
#define SSO_BASE			0x1670000000000ull
#define SSO_ADDR(node)			(SET_XKPHYS + NODE_OFFSET(node) +      \
					 SSO_BASE)
#define GRP_OFFSET(grp)			((grp) << 16)
#define GRP_ADDR(n, g)			(SSO_ADDR(n) + GRP_OFFSET(g))
#define SSO_GRP_AQ_CNT(n, g)		(GRP_ADDR(n, g)		   + 0x20000700)

#define MIO_PTP_BASE			0x1070000000000ull
#define MIO_PTP_ADDR(node)		(SET_XKPHYS + NODE_OFFSET(node) +      \
					 MIO_PTP_BASE)
#define MIO_PTP_CLOCK_CFG(node)		(MIO_PTP_ADDR(node)		+ 0xf00)
#define MIO_PTP_CLOCK_HI(node)		(MIO_PTP_ADDR(node)		+ 0xf10)
#define MIO_PTP_CLOCK_COMP(node)	(MIO_PTP_ADDR(node)		+ 0xf18)

struct octeon3_ethernet;

struct octeon3_rx {
	struct napi_struct	napi;
	struct octeon3_ethernet *parent;
	int rx_grp;
	int rx_irq;
	cpumask_t rx_affinity_hint;
} ____cacheline_aligned_in_smp;

struct octeon3_ethernet {
	struct bgx_port_netdev_priv bgx_priv; /* Must be first element. */
	struct list_head list;
	struct net_device *netdev;
	enum octeon3_mac_type mac_type;
	struct octeon3_rx rx_cxt[MAX_RX_QUEUES];
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

static int wait_pko_response;
module_param(wait_pko_response, int, 0644);
MODULE_PARM_DESC(wait_pko_response, "Wait for response after each pko command.");

static int num_packet_buffers = 768;
module_param(num_packet_buffers, int, 0444);
MODULE_PARM_DESC(num_packet_buffers,
		 "Number of packet buffers to allocate per port.");

static int packet_buffer_size = 2048;
module_param(packet_buffer_size, int, 0444);
MODULE_PARM_DESC(packet_buffer_size, "Size of each RX packet buffer.");

static int rx_queues = 1;
module_param(rx_queues, int, 0444);
MODULE_PARM_DESC(rx_queues, "Number of RX threads per port.");

int ilk0_lanes = 1;
module_param(ilk0_lanes, int, 0444);
MODULE_PARM_DESC(ilk0_lanes, "Number of SerDes lanes used by ILK link 0.");

int ilk1_lanes = 1;
module_param(ilk1_lanes, int, 0444);
MODULE_PARM_DESC(ilk1_lanes, "Number of SerDes lanes used by ILK link 1.");

static struct octeon3_ethernet_node octeon3_eth_node[MAX_NODES];
static struct kmem_cache *octeon3_eth_sso_pko_cache;

/**
 * Reads a 64 bit value from the processor local scratchpad memory.
 *
 * @param offset byte offset into scratch pad to read
 *
 * @return value read
 */
static inline u64 scratch_read64(u64 offset)
{
	return *(u64 *)((long)SCRATCH_BASE + offset);
}

/**
 * Write a 64 bit value to the processor local scratchpad memory.
 *
 * @param offset byte offset into scratch pad to write
 @ @praram value to write
 */
static inline void scratch_write64(u64 offset, u64 value)
{
	*(u64 *)((long)SCRATCH_BASE + offset) = value;
}

static int get_pki_chan(int node, int interface, int index)
{
	int	pki_chan;

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
		cpu = cpumask_next(octeon3_eth_node[node].next_cpu_irq_affinity, cpu_online_mask);
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
	u64		node = cvmx_get_node_num();
	u64		addr;
	u64		response;
	struct wr_ret	r;

	/* See SSO_GET_WORK_LD_S for the address to read */
	addr = 1ull << 63;
	addr |= BIT(48);
	addr |= DID_TAG_SWTAG << 40;
	addr |= node << 36;
	addr |= BIT(30);
	addr |= BIT(29);
	addr |= octeon3_eth_lgrp_to_ggrp(node, grp) << 4;
	addr |= SSO_NO_WAIT << 3;
	response = __raw_readq((void __iomem *)addr);

	/* See SSO_GET_WORK_RTN_S for the format of the response */
	r.grp = (response & GENMASK_ULL(57, 48)) >> 48;
	if (response & BIT(63))
		r.work = NULL;
	else
		r.work = phys_to_virt(response & GENMASK_ULL(41, 0));

	return r;
}

/**
 * octeon3_core_get_work_async - Request work via a iobdma command. Doesn't wait
 *				 for the response.
 *
 * @grp: Group to request work for.
 */
static inline void octeon3_core_get_work_async(unsigned int grp)
{
	u64	data;
	u64	node = cvmx_get_node_num();

	/* See SSO_GET_WORK_DMA_S for the command structure */
	data = SCR_SCRATCH << 56;
	data |= 1ull << 48;
	data |= DID_TAG_SWTAG << 40;
	data |= node << 36;
	data |= 1ull << 30;
	data |= 1ull << 29;
	data |= octeon3_eth_lgrp_to_ggrp(node, grp) << 4;
	data |= SSO_NO_WAIT << 3;

	__raw_writeq(data, (void __iomem *)IOBDMA_SENDSINGLE);
}

/**
 * octeon3_core_get_response_async - Read the request work response. Must be
 *				     called after calling
 *				     octeon3_core_get_work_async().
 *
 * Returns work queue entry.
 */
static inline struct wr_ret octeon3_core_get_response_async(void)
{
	struct wr_ret	r;
	u64		response;

	CVMX_SYNCIOBDMA;
	response = scratch_read64(SCR_SCRATCH);

	/* See SSO_GET_WORK_RTN_S for the format of the response */
	r.grp = (response & GENMASK_ULL(57, 48)) >> 48;
	if (response & BIT(63))
		r.work = NULL;
	else
		r.work = phys_to_virt(response & GENMASK_ULL(41, 0));

	return r;
}

static void octeon3_eth_replenish_rx(struct octeon3_ethernet *priv, int count)
{
	struct sk_buff *skb;
	int i;

	for (i = 0; i < count; i++) {
		void **buf;

		skb = __alloc_skb(packet_buffer_size, GFP_ATOMIC, 0, priv->node);
		if (!skb)
			break;
		buf = (void **)PTR_ALIGN(skb->head, 128);
		buf[SKB_PTR_OFFSET] = skb;
		octeon_fpa3_free(priv->node, priv->pki_aura, buf);
	}
}

static bool octeon3_eth_tx_complete_runnable(struct octeon3_ethernet_worker *worker)
{
	return atomic_read(&worker->kick) != 0 || kthread_should_stop();
}

static int octeon3_eth_replenish_all(struct octeon3_ethernet_node *oen)
{
	int pending = 0;
	int batch_size = 32;
	struct octeon3_ethernet *priv;

	rcu_read_lock();
	list_for_each_entry_rcu(priv, &oen->device_list, list) {
		int amount = atomic64_sub_if_positive(batch_size, &priv->buffers_needed);

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
	struct skb_shared_hwtstamps	shts;
	u64				hwts;
	u64				ns;

	hwts = *((u64 *)(skb->cb) + 1);
	ns = timecounter_cyc2time(&priv->tc, hwts);
	memset(&shts, 0, sizeof(shts));
	shts.hwtstamp = ns_to_ktime(ns);
	skb_tstamp_tx(skb, &shts);

	return 0;
}

static int octeon3_eth_tx_complete_worker(void *data)
{
	struct octeon3_ethernet_worker *worker = data;
	struct octeon3_ethernet_node *oen = worker->oen;
	int backlog;
	int order = worker->order;
	int tx_complete_stop_thresh = order * 100;
	int backlog_stop_thresh = order == 0 ? 31 : order * 80;
	u64 aq_cnt;
	int i;

	while (!kthread_should_stop()) {
		wait_event_interruptible(worker->queue, octeon3_eth_tx_complete_runnable(worker));
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
				if (unlikely(netif_queue_stopped(tx_netdev)) &&
				    atomic64_read(&tx_priv->tx_backlog) < MAX_TX_QUEUE_DEPTH)
					netif_wake_queue(tx_netdev);
				skb = container_of((void *)work, struct sk_buff, cb);
				if (unlikely(tx_priv->tx_timestamp_hw) &&
				    unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS))
					octeon3_eth_tx_complete_hwtstamp(tx_priv, skb);
				dev_kfree_skb(skb);
			}

			aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(oen->node, oen->tx_complete_grp));
			aq_cnt &= GENMASK_ULL(32, 0);
			if ((backlog > backlog_stop_thresh || aq_cnt > tx_complete_stop_thresh) &&
			    order < ARRAY_SIZE(oen->workers) - 1) {
				atomic_set(&oen->workers[order + 1].kick, 1);
				wake_up(&oen->workers[order + 1].queue);
			}
		} while (!need_resched() &&
			 (backlog > backlog_stop_thresh ||
			  aq_cnt > tx_complete_stop_thresh));

		cond_resched();

		if (!octeon3_eth_tx_complete_runnable(worker))
			octeon3_sso_irq_set(oen->node, oen->tx_complete_grp, true);
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
	int i;
	int rv = 0;
	unsigned int sso_intsn;
	struct octeon3_ethernet_node *oen;

	mutex_lock(&octeon3_eth_init_mutex);

	oen = octeon3_eth_node + node;

	if (oen->init_done)
		goto done;

	/* CN78XX-P1.0 cannot un-initialize PKO, so get a module
	 * reference to prevent it from being unloaded.
	 */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_0))
		if (!try_module_get(THIS_MODULE))
			dev_err(&pdev->dev,
				"ERROR: Could not obtain module reference for CN78XX-P1.0\n");

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
				   &oen->pki_packet_pool_stack, 64 * num_packet_buffers);
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
		octeon3_eth_sso_pko_cache = kmem_cache_create("sso_pko", 4096, 128, 0, NULL);
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

	oen->tx_complete_grp = octeon3_sso_alloc_grp(node, -1);
	if (oen->tx_complete_grp < 0)
		goto done;

	sso_intsn = SSO_INTSN_EXE << 12 | oen->tx_complete_grp;
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
		oen->workers[i].task = kthread_create_on_node(octeon3_eth_tx_complete_worker,
							      oen->workers + i, node,
							      "oct3_eth/%d:%d", node, i);
		if (IS_ERR(oen->workers[i].task)) {
			rv = PTR_ERR(oen->workers[i].task);
			goto done;
		} else {
#ifdef CONFIG_NUMA
			set_cpus_allowed_ptr(oen->workers[i].task, cpumask_of_node(node));
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

/* Receive one packet.
 * returns the number of RX buffers consumed.
 */
static int octeon3_eth_rx_one(struct octeon3_rx *rx, bool is_async, bool req_next)
{
	int segments;
	int ret;
	unsigned int packet_len;
	struct wqe *work;
	u8 *data;
	int len_remaining;
	struct sk_buff *skb;
	union buf_ptr packet_ptr;
	struct wr_ret r;
	struct octeon3_ethernet *priv = rx->parent;

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
			segment_size = (segments == 1) ? len_remaining : packet_ptr.size;
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
				next_skb->next = skb_shinfo(current_skb)->frag_list;
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

		napi_gro_receive(&rx->napi, skb);
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
	int rx_count = 0;
	struct octeon3_rx *cxt;
	struct octeon3_ethernet *priv;
	u64 aq_cnt;
	int n = 0;
	int n_bufs = 0;
	u64 old_scratch;

	cxt = container_of(napi, struct octeon3_rx, napi);
	priv = cxt->parent;

	/* Get the amount of work pending */
	aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(priv->node, cxt->rx_grp));
	aq_cnt &= GENMASK_ULL(32, 0);

	if (likely(USE_ASYNC_IOBDMA)) {
		/* Save scratch in case userspace is using it */
		CVMX_SYNCIOBDMA;
		old_scratch = scratch_read64(SCR_SCRATCH);

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
		octeon3_sso_irq_set(cxt->parent->node, cxt->rx_grp, true);
	}

	if (likely(USE_ASYNC_IOBDMA)) {
		/* Restore the scratch area */
		scratch_write64(SCR_SCRATCH, old_scratch);
	}

	return rx_count;
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

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;

	if (priv->ptp_clock)
		info->phc_index = ptp_clock_index(priv->ptp_clock);
	else
		info->phc_index = -1;

	info->tx_types = (1 << HWTSTAMP_TX_OFF) | (1 << HWTSTAMP_TX_ON);

	info->rx_filters = (1 << HWTSTAMP_FILTER_NONE) | (1 << HWTSTAMP_FILTER_ALL);

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
		int fifo_size;
		int max_mtu = 1500;
		struct octeon3_ethernet *priv = netdev_priv(netdev);

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
		fifo_size = octeon3_pko_get_fifo_size(priv->node, priv->interface,
						      priv->index, priv->mac_type);

		switch (fifo_size) {
		case 2560:
			max_mtu = 1920 - ETH_HLEN - ETH_FCS_LEN - (2 * VLAN_HLEN);
			break;

		case 5120:
			max_mtu = 4480 - ETH_HLEN - ETH_FCS_LEN - (2 * VLAN_HLEN);
			break;

		case 10240:
			max_mtu = 9600 - ETH_HLEN - ETH_FCS_LEN - (2 * VLAN_HLEN);
			break;

		default:
			break;
		}
		if (new_mtu > max_mtu) {
			netdev_warn(netdev,
				    "Maximum MTU supported is %d", max_mtu);
			return -EINVAL;
		}
	}
	return bgx_port_change_mtu(netdev, new_mtu);
}

static int octeon3_eth_common_ndo_init(struct net_device *netdev, int extra_skip)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_ethernet_node *oen = octeon3_eth_node + priv->node;
	int pki_chan, dq;
	int base_rx_grp[MAX_RX_QUEUES];
	int r, i;
	int aura;

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

	r = octeon3_sso_alloc_grp_range(priv->node, -1, rx_queues, false, base_rx_grp);
	if (r) {
		dev_err(netdev->dev.parent, "Failed to allocated SSO group\n");
		return -ENODEV;
	}
	for (i = 0; i < rx_queues; i++) {
		priv->rx_cxt[i].rx_grp = base_rx_grp[i];
		priv->rx_cxt[i].parent = priv;

		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X))
			octeon3_sso_pass1_limit(priv->node, priv->rx_cxt[i].rx_grp);
	}
	priv->num_rx_cxt = rx_queues;

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

	/* Register ethtool methods */
	netdev->ethtool_ops = &octeon3_ethtool_ops;

	return 0;
}

static int octeon3_eth_bgx_ndo_init(struct net_device *netdev)
{
	struct octeon3_ethernet	*priv = netdev_priv(netdev);
	const u8		*mac;
	int			r;

	priv->pknd = bgx_port_get_pknd(priv->node, priv->interface, priv->index);
	octeon3_eth_common_ndo_init(netdev, 0);

	/* Padding and FCS are done in BGX */
	r = octeon3_pko_set_mac_options(priv->node, priv->interface, priv->index,
					priv->mac_type, false, false, 0);
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
	int			grp[MAX_RX_QUEUES];
	int			i;

	/* Shutdwon pki for this interface */
	octeon3_pki_port_shutdown(priv->node, priv->pknd);
	octeon_fpa3_release_aura(priv->node, priv->pki_aura);
	aura2bufs_needed[priv->node][priv->pki_aura] = NULL;

	/* Shutdown pko for this interface */
	octeon3_pko_interface_uninit(priv->node, &priv->pko_queue, 1);

	/* Free the receive contexts sso groups */
	for (i = 0; i < rx_queues; i++)
		grp[i] = priv->rx_cxt[i].rx_grp;
	octeon3_sso_free_grp_range(priv->node, grp, rx_queues);
}

static irqreturn_t octeon3_eth_rx_handler(int irq, void *info)
{
	struct octeon3_rx *rx = info;

	/* Disarm the irq. */
	octeon3_sso_irq_set(rx->parent->node, rx->rx_grp, false);

	napi_schedule(&rx->napi);
	return IRQ_HANDLED;
}

static int octeon3_eth_common_ndo_open(struct net_device *netdev)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	struct octeon3_rx *rx;
	int i;
	int r;

	for (i = 0; i < priv->num_rx_cxt; i++) {
		unsigned int	sso_intsn;

		rx = priv->rx_cxt + i;
		sso_intsn = SSO_INTSN_EXE << 12 | rx->rx_grp;

		rx->rx_irq = irq_create_mapping(NULL, sso_intsn);
		if (!rx->rx_irq) {
			netdev_err(netdev,
				   "ERROR: Couldn't map hwirq: %x\n", sso_intsn);
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

		netif_napi_add(priv->netdev, &rx->napi,
			       octeon3_eth_napi, NAPI_POLL_WEIGHT);
		napi_enable(&rx->napi);

		/* Arm the irq. */
		octeon3_sso_irq_set(priv->node, rx->rx_grp, true);
	}
	octeon3_eth_replenish_rx(priv, priv->rx_buf_count);

	return 0;

err2:
	irq_dispose_mapping(rx->rx_irq);
err1:
	for (i--; i >= 0; i--) {
		rx = priv->rx_cxt + i;
		free_irq(rx->rx_irq, rx);
		irq_dispose_mapping(rx->rx_irq);
		napi_disable(&rx->napi);
		netif_napi_del(&rx->napi);
	}

	return r;
}

static int octeon3_eth_bgx_ndo_open(struct net_device *netdev)
{
	int	rc;

	rc = octeon3_eth_common_ndo_open(netdev);
	if (rc == 0)
		rc = bgx_port_enable(netdev);

	return rc;
}

static int octeon3_eth_common_ndo_stop(struct net_device *netdev)
{
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	void **w;
	struct sk_buff *skb;
	struct octeon3_rx *rx;
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
		napi_disable(&rx->napi);
		netif_napi_del(&rx->napi);
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

static inline u64 build_pko_send_hdr_desc(struct sk_buff *skb)
{
	u64	send_hdr = 0;
	u8	l4_hdr = 0;
	u64	checksum_alg;

	/* See PKO_SEND_HDR_S in the HRM for the send header descriptor
	 * format.
	 */
#ifdef __LITTLE_ENDIAN
	send_hdr |= BIT(43);
#endif

	if (!OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
		/* Don't allocate to L2 */
		send_hdr |= BIT(42);
	}

	/* Don't automatically free to FPA */
	send_hdr |= BIT(40);

	send_hdr |= skb->len;

	if (skb->ip_summed != CHECKSUM_NONE &&
	    skb->ip_summed != CHECKSUM_UNNECESSARY) {
#ifndef BROKEN_SIMULATOR_CSUM
		switch (skb->protocol) {
		case htons(ETH_P_IP):
			send_hdr |= ETH_HLEN << 16;
			send_hdr |= BIT(45);
			l4_hdr = ip_hdr(skb)->protocol;
			send_hdr |= (ETH_HLEN + (4 * ip_hdr(skb)->ihl)) << 24;
			break;

		case htons(ETH_P_IPV6):
			l4_hdr = ipv6_hdr(skb)->nexthdr;
			send_hdr |= ETH_HLEN << 16;
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
				send_hdr &= ~GENMASK_ULL(31, 24);
				send_hdr |= l4ptr << 24;
				send_hdr |= checksum_alg << 46;
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
	u64	send_ext = 0;

	/* See PKO_SEND_EXT_S in the HRM for the send extended descriptor
	 * format.
	 */
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	send_ext |= (u64)PKO_SENDSUBDC_EXT << 44;
	send_ext |= 1ull << 40;
	send_ext |= BIT(39);
	send_ext |= ETH_HLEN << 16;

	return send_ext;
}

static inline u64 build_pko_send_tso(struct sk_buff *skb, uint mtu)
{
	u64	send_tso = 0;

	/* See PKO_SEND_TSO_S in the HRM for the send tso descriptor format */
	send_tso |= 12ull << 56;
	send_tso |= (u64)PKO_SENDSUBDC_TSO << 44;
	send_tso |= (skb_transport_offset(skb) + tcp_hdrlen(skb)) << 24;
	send_tso |= (mtu + ETH_HLEN) << 8;

	return send_tso;
}

static inline u64 build_pko_send_mem_sub(u64 addr)
{
	u64	send_mem = 0;

	/* See PKO_SEND_MEM_S in the HRM for the send mem descriptor format */
	send_mem |= (u64)PKO_SENDSUBDC_MEM << 44;
	send_mem |= (u64)MEMDSZ_B64 << 60;
	send_mem |= (u64)MEMALG_SUB << 56;
	send_mem |= 1ull << 48;
	send_mem |= addr;

	return send_mem;
}

static inline u64 build_pko_send_mem_ts(u64 addr)
{
	u64	send_mem = 0;

	/* See PKO_SEND_MEM_S in the HRM for the send mem descriptor format */
	send_mem |= 1ull << 62;
	send_mem |= (u64)PKO_SENDSUBDC_MEM << 44;
	send_mem |= (u64)MEMDSZ_B64 << 60;
	send_mem |= (u64)MEMALG_SETTSTMP << 56;
	send_mem |= addr;

	return send_mem;
}

static inline u64 build_pko_send_free(u64 addr)
{
	u64	send_free = 0;

	/* See PKO_SEND_FREE_S in the HRM for the send free descriptor format */
	send_free |= (u64)PKO_SENDSUBDC_FREE << 44;
	send_free |= addr;

	return send_free;
}

static inline u64 build_pko_send_work(int grp, u64 addr)
{
	u64	send_work = 0;

	/* See PKO_SEND_WORK_S in the HRM for the send work descriptor format */
	send_work |= (u64)PKO_SENDSUBDC_WORK << 44;
	send_work |= (u64)grp << 52;
	send_work |= 2ull << 50;
	send_work |= addr;

	return send_work;
}

static int octeon3_eth_ndo_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct sk_buff *skb_tmp;
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	u64 scr_off = LMTDMA_SCR_OFFSET;
	u64 pko_send_desc;
	u64 lmtdma_data;
	u64 aq_cnt = 0;
	struct octeon3_ethernet_node *oen;
	long backlog;
	int frag_count;
	u64 head_len;
	int i;
	u64 *dma_addr;
	void **work;
	unsigned int mss;
	int grp;

	frag_count = 0;
	if (skb_has_frag_list(skb))
		skb_walk_frags(skb, skb_tmp)
			frag_count++;

	/* Stop the queue if pko or sso are not keeping up */
	oen = octeon3_eth_node + priv->node;
	aq_cnt = oct_csr_read(SSO_GRP_AQ_CNT(oen->node, oen->tx_complete_grp));
	aq_cnt &= GENMASK_ULL(32, 0);
	backlog = atomic64_inc_return(&priv->tx_backlog);
	if (unlikely(backlog > MAX_TX_QUEUE_DEPTH || aq_cnt > 100000))
		netif_stop_queue(netdev);

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
	pko_send_desc = build_pko_send_hdr_desc(skb);
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
	pko_send_desc |= (u64)PKO_SENDSUBDC_GATHER << 45;
	head_len = skb_headlen(skb);
	if (head_len > 0) {
		pko_send_desc |= head_len << 48;
		pko_send_desc |= virt_to_phys(skb->data);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}
	for (i = 1; i <= skb_shinfo(skb)->nr_frags; i++) {
		struct skb_frag_struct *fs = skb_shinfo(skb)->frags + i - 1;

		pko_send_desc &= ~(GENMASK_ULL(63, 48) | GENMASK_ULL(41, 0));
		pko_send_desc |= (u64)fs->size << 48;
		pko_send_desc |= virt_to_phys((u8 *)page_address(fs->page.p) + fs->page_offset);
		scratch_write64(scr_off, pko_send_desc);
		scr_off += sizeof(pko_send_desc);
	}
	skb_walk_frags(skb, skb_tmp) {
		pko_send_desc &= ~(GENMASK_ULL(63, 48) | GENMASK_ULL(41, 0));
		pko_send_desc |= (u64)skb_tmp->len << 48;
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
	lmtdma_data = 0;
	lmtdma_data |= (u64)(LMTDMA_SCR_OFFSET >> 3) << 56;
	if (wait_pko_response)
		lmtdma_data |= 1ull << 48;
	lmtdma_data |= 0x51ull << 40;
	lmtdma_data |= (u64)priv->node << 36;
	lmtdma_data |= priv->pko_queue << 16;

	dma_addr = (u64 *)(LMTDMA_ORDERED_IO_ADDR | ((scr_off & 0x78) - 8));
	*dma_addr = lmtdma_data;

	preempt_enable();

	if (wait_pko_response) {
		u64	query_rtn;

		CVMX_SYNCIOBDMA;

		/* See PKO_QUERY_RTN_S in the HRM for the return format */
		query_rtn = scratch_read64(LMTDMA_SCR_OFFSET);
		query_rtn >>= 60;
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
	struct octeon3_ethernet *priv = netdev_priv(netdev);
	u64 packets, octets, dropped;
	u64 delta_packets, delta_octets, delta_dropped;

	spin_lock(&priv->stat_lock);

	octeon3_pki_get_stats(priv->node, priv->pknd, &packets, &octets, &dropped);

	delta_packets = (packets - priv->last_packets) & ((1ull << 48) - 1);
	delta_octets = (octets - priv->last_octets) & ((1ull << 48) - 1);
	delta_dropped = (dropped - priv->last_dropped) & ((1ull << 48) - 1);

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
	struct octeon3_ethernet	*priv;
	u64			count;

	priv = container_of(cc, struct octeon3_ethernet, cc);
	count = oct_csr_read(MIO_PTP_CLOCK_HI(priv->node));
	return count;
}

static int octeon3_bgx_hwtstamp(struct net_device *netdev, int en)
{
	struct octeon3_ethernet		*priv = netdev_priv(netdev);
	u64				data;

	switch (bgx_port_get_mode(priv->node, priv->interface, priv->index)) {
	case PORT_MODE_RGMII:
	case PORT_MODE_SGMII:
		data = oct_csr_read(BGX_GMP_GMI_RX_FRM_CTL(priv->node, priv->interface, priv->index));
		if (en)
			data |= BIT(12);
		else
			data &= ~BIT(12);
		oct_csr_write(data, BGX_GMP_GMI_RX_FRM_CTL(priv->node, priv->interface, priv->index));
		break;

	case PORT_MODE_XAUI:
	case PORT_MODE_RXAUI:
	case PORT_MODE_10G_KR:
	case PORT_MODE_XLAUI:
	case PORT_MODE_40G_KR4:
	case PORT_MODE_XFI:
		data = oct_csr_read(BGX_SMU_RX_FRM_CTL(priv->node, priv->interface, priv->index));
		if (en)
			data |= BIT(12);
		else
			data &= ~BIT(12);
		oct_csr_write(data, BGX_SMU_RX_FRM_CTL(priv->node, priv->interface, priv->index));
		break;

	default:
		/* No timestamp support*/
		return -EOPNOTSUPP;
	}

	return 0;
}

static int octeon3_pki_hwtstamp(struct net_device *netdev, int en)
{
	struct octeon3_ethernet		*priv = netdev_priv(netdev);
	int				skip = en ? 8 : 0;

	octeon3_pki_set_ptp_skip(priv->node, priv->pknd, skip);

	return 0;
}

static int octeon3_ioctl_hwtstamp(struct net_device *netdev,
				  struct ifreq *rq, int cmd)
{
	struct octeon3_ethernet		*priv = netdev_priv(netdev);
	u64				data;
	struct hwtstamp_config		config;
	int				en;

	/* The PTP block should be enabled */
	data = oct_csr_read(MIO_PTP_CLOCK_CFG(priv->node));
	if (!(data & BIT(0))) {
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
	u64			comp;
	u64			diff;
	int			neg_ppb = 0;

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
	s64			now;
	unsigned long		flags;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);

	spin_lock_irqsave(&priv->ptp_lock, flags);
	now = timecounter_read(&priv->tc);
	now += delta;
	timecounter_init(&priv->tc, &priv->cc, now);
	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

static int octeon3_gettime(struct ptp_clock_info *ptp, struct timespec *ts)
{
	struct octeon3_ethernet	*priv;
	u64			ns;
	u32			remainder;
	unsigned long		flags;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);

	spin_lock_irqsave(&priv->ptp_lock, flags);
	ns = timecounter_read(&priv->tc);
	spin_unlock_irqrestore(&priv->ptp_lock, flags);
	ts->tv_sec = div_u64_rem(ns, 1000000000ULL, &remainder);
	ts->tv_nsec = remainder;

	return 0;
}

static int octeon3_settime(struct ptp_clock_info *ptp,
			   const struct timespec *ts)
{
	struct octeon3_ethernet	*priv;
	u64			ns;
	unsigned long		flags;

	priv = container_of(ptp, struct octeon3_ethernet, ptp_info);
	ns = timespec_to_ns(ts);

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

	dev_info(&pdev->dev, "Probing %d-%d:%d\n",
		 pd->numa_node, pd->interface, pd->port);
	netdev = alloc_etherdev(sizeof(struct octeon3_ethernet));
	if (!netdev) {
		dev_err(&pdev->dev, "Failed to allocated ethernet device\n");
		return -ENOMEM;
	}

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
	list_add_tail_rcu(&priv->list, &octeon3_eth_node[priv->node].device_list);
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

/**
 * octeon3_eth_global_exit - Free all the used resources and restore the
 *			     hardware to the default state.
 * @node: Node to free/reset.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
static int octeon3_eth_global_exit(int node)
{
	struct octeon3_ethernet_node	*oen = octeon3_eth_node + node;
	int				i;

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
	octeon3_sso_free_grp(node, oen->tx_complete_grp);
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
	struct net_device		*netdev = dev_get_drvdata(&pdev->dev);
	struct octeon3_ethernet		*priv = netdev_priv(netdev);
	int				node = priv->node;
	struct octeon3_ethernet_node	*oen = octeon3_eth_node + node;
	struct mac_platform_data	*pd = dev_get_platdata(&pdev->dev);

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
		oen->init_done = false;
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
	if (rx_queues <= 0)
		rx_queues = 1;
	if (rx_queues > MAX_RX_QUEUES)
		rx_queues = MAX_RX_QUEUES;

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
