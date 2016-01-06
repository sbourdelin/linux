/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
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

#ifndef RXE_H
#define RXE_H

#include <linux/skbuff.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_pack.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_addr.h>

#include <uapi/rdma/ib_user_rvt.h>

#define IB_PHYS_STATE_LINK_UP		(5)

#define ROCE_V2_UDP_DPORT 	(4791)
#define ROCE_V2_UDP_SPORT 	(0xC000)

struct rvt_dev;

/* callbacks from ib_rvt to network interface layer */
struct rvt_ifc_ops {
	void (*release)(struct rvt_dev *rvt);
	__be64 (*node_guid)(struct rvt_dev *rvt);
	__be64 (*port_guid)(struct rvt_dev *rvt, unsigned int port_num);
	__be16 (*port_speed)(struct rvt_dev *rvt, unsigned int port_num);
	struct device *(*dma_device)(struct rvt_dev *rvt);
	int (*mcast_add)(struct rvt_dev *rvt, union ib_gid *mgid);
	int (*mcast_delete)(struct rvt_dev *rvt, union ib_gid *mgid);
	int (*create_flow)(struct rvt_dev *rvt, void **ctx, void *rvt_ctx);
	void (*destroy_flow)(struct rvt_dev *rdev, void *ctx);
	int (*send)(struct rvt_dev *rdev, struct rvt_av *av,
		    struct sk_buff *skb, void *flow);
	int (*loopback)(struct sk_buff *skb);
	struct sk_buff *(*alloc_sendbuf)(struct rvt_dev *rdev, struct rvt_av *av, int paylen);
	char *(*parent_name)(struct rvt_dev *rvt, unsigned int port_num);
	enum rdma_link_layer (*link_layer)(struct rvt_dev *rvt,
					   unsigned int port_num);
	struct net_device *(*get_netdev)(struct rvt_dev *rvt,
					 unsigned int port_num);
};

#define RVT_POOL_ALIGN		(16)
#define RVT_POOL_CACHE_FLAGS	(0)

enum rvt_pool_flags {
	RVT_POOL_ATOMIC		= BIT(0),
	RVT_POOL_INDEX		= BIT(1),
	RVT_POOL_KEY		= BIT(2),
};

enum rvt_elem_type {
	RVT_TYPE_UC,
	RVT_TYPE_PD,
	RVT_TYPE_AH,
	RVT_TYPE_SRQ,
	RVT_TYPE_QP,
	RVT_TYPE_CQ,
	RVT_TYPE_MR,
	RVT_TYPE_MW,
	RVT_TYPE_FMR,
	RVT_TYPE_MC_GRP,
	RVT_TYPE_MC_ELEM,
	RVT_NUM_TYPES,		/* keep me last */
};

enum rvt_pool_state {
	rvt_pool_invalid,
	rvt_pool_valid,
};

struct rvt_pool_entry {
	struct rvt_pool		*pool;
	struct kref		ref_cnt;
	struct list_head	list;

	/* only used if indexed or keyed */
	struct rb_node		node;
	u32			index;
};

struct rvt_pool {
	struct rvt_dev		*rvt;
	spinlock_t              pool_lock; /* pool spinlock */
	size_t			elem_size;
	struct kref		ref_cnt;
	void			(*cleanup)(void *obj);
	enum rvt_pool_state	state;
	enum rvt_pool_flags	flags;
	enum rvt_elem_type	type;

	unsigned int		max_elem;
	atomic_t		num_elem;

	/* only used if indexed or keyed */
	struct rb_root		tree;
	unsigned long		*table;
	size_t			table_size;
	u32			max_index;
	u32			min_index;
	u32			last;
	size_t			key_offset;
	size_t			key_size;
};

struct rvt_port {
	struct ib_port_attr	attr;
	u16			*pkey_tbl;
	__be64			port_guid;
	__be64			subnet_prefix;
	spinlock_t		port_lock;
	unsigned int		mtu_cap;
	/* special QPs */
	u32			qp_smi_index;
	u32			qp_gsi_index;
};

struct rvt_dev {
	struct ib_device	ib_dev;
	struct ib_device_attr	attr;
	int			max_ucontext;
	int			max_inline_data;
	struct kref		ref_cnt;
	struct mutex	usdev_lock;

	struct rvt_ifc_ops	*ifc_ops;


	int			xmit_errors;

	struct rvt_pool		uc_pool;
	struct rvt_pool		pd_pool;
	struct rvt_pool		ah_pool;
	struct rvt_pool		srq_pool;
	struct rvt_pool		qp_pool;
	struct rvt_pool		cq_pool;
	struct rvt_pool		mr_pool;
	struct rvt_pool		mw_pool;
	struct rvt_pool		fmr_pool;
	struct rvt_pool		mc_grp_pool;
	struct rvt_pool		mc_elem_pool;

	spinlock_t		pending_lock;
	struct list_head	pending_mmaps;

	spinlock_t		mmap_offset_lock;
	int			mmap_offset;

	u8			num_ports;
	struct rvt_port		*port;
};

struct rvt_dev* rvt_alloc_device(size_t size);
int rvt_register_device(struct rvt_dev *rdev, 
			struct rvt_ifc_ops *ops,
			unsigned int mtu);
int rvt_unregister_device(struct rvt_dev *rdev);

int rvt_set_mtu(struct rvt_dev *rvt, unsigned int dev_mtu,
		unsigned int port_num);

int rvt_rcv(struct sk_buff *skb, struct rvt_dev *rdev, u8 port_num);

void rvt_dev_put(struct rvt_dev *rvt);

void rvt_send_done(void *rvt_ctx);

#endif /* RXE_H */
