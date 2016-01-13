/*
 * cxgbi_lro.h: Chelsio iSCSI LRO functions for T4/5 iSCSI driver.
 *
 * Copyright (c) 2015 Chelsio Communications, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * Written by: Karen Xie (kxie@chelsio.com)
 */

#ifndef	__CXGBI_LRO_H__
#define	__CXGBI_LRO_H__

#include <linux/errno.h>
#include <linux/types.h>

enum {
	CXGBI_LRO_CB_USED	= (1 << 0),
};

#define LRO_FLUSH_TOTALLEN_MAX	65535
struct cxgbi_rx_lro_cb {
	struct cxgbi_sock *csk;
	__u32 pdu_totallen;
	u8 pdu_cnt;
	u8 flags;
};

struct cxgbi_rx_pdu_cb {
	unsigned long flags;
	unsigned int seq;
	__u32 ddigest;
	__u32 pdulen;
	u32 frags;
};

#define LRO_SKB_MAX_HEADROOM  \
		(sizeof(struct cxgbi_rx_lro_cb) + \
		 MAX_SKB_FRAGS * sizeof(struct cxgbi_rx_pdu_cb))

#define LRO_SKB_MIN_HEADROOM  \
		(sizeof(struct cxgbi_rx_lro_cb) + \
		 sizeof(struct cxgbi_rx_pdu_cb))

#define cxgbi_skb_rx_lro_cb(skb)	((struct cxgbi_rx_lro_cb *)skb->head)
#define cxgbi_skb_rx_pdu_cb(skb, i)	\
	((struct cxgbi_rx_pdu_cb *)(skb->head + sizeof(struct cxgbi_rx_lro_cb) \
					+ i * sizeof(struct cxgbi_rx_pdu_cb)))

static inline void cxgbi_rx_cb_set_flag(struct cxgbi_rx_pdu_cb *cb,
					int flag)
{
	__set_bit(flag, &cb->flags);
}

static inline void cxgbi_rx_cb_clear_flag(struct cxgbi_rx_pdu_cb *cb,
					  int flag)
{
	__clear_bit(flag, &cb->flags);
}

static inline int cxgbi_rx_cb_test_flag(struct cxgbi_rx_pdu_cb *cb,
					int flag)
{
	return test_bit(flag, &cb->flags);
}

void cxgbi_lro_skb_dump(struct sk_buff *);

#endif	/*__CXGBI_LRO_H__*/
