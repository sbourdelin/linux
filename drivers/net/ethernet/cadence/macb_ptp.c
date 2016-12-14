/*
 * 1588 PTP support for GEM device.
 *
 * Copyright (C) 2016 Microchip Technology
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/time64.h>
#include <linux/ptp_classify.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/net_tstamp.h>

#include "macb.h"

#define  GEM_PTP_TIMER_NAME "gem-ptp-timer"

static inline void gem_tsu_get_time(struct macb *bp,
				    struct timespec64 *ts)
{
	u64 sec, sech, secl;

	spin_lock(&bp->tsu_clk_lock);

	/* GEM's internal time */
	sech = gem_readl(bp, TSH);
	secl = gem_readl(bp, TSL);
	ts->tv_nsec = gem_readl(bp, TN);
	ts->tv_sec = (sech << 32) | secl;

	/* minimize error */
	sech = gem_readl(bp, TSH);
	secl = gem_readl(bp, TSL);
	sec = (sech << 32) | secl;
	if (ts->tv_sec != sec) {
		ts->tv_sec = sec;
		ts->tv_nsec = gem_readl(bp, TN);
	}

	spin_unlock(&bp->tsu_clk_lock);
}

static inline void gem_tsu_set_time(struct macb *bp,
				    const struct timespec64 *ts)
{
	u32 ns, sech, secl;
	s64 word_mask = 0xffffffff;

	sech = (u32)ts->tv_sec;
	secl = (u32)ts->tv_sec;
	ns = ts->tv_nsec;
	if (ts->tv_sec > word_mask)
		sech = (ts->tv_sec >> 32);

	spin_lock(&bp->tsu_clk_lock);

	/* TSH doesn't latch the time and no atomicity! */
	gem_writel(bp, TN, 0); /* clear to avoid overflow */
	gem_writel(bp, TSH, sech);
	gem_writel(bp, TSL, secl);
	gem_writel(bp, TN, ns);

	spin_unlock(&bp->tsu_clk_lock);
}

static int gem_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct macb *bp = container_of(ptp, struct macb, ptp_caps);
	u32 word, diff;
	u64 adj, rate;
	int neg_adj = 0;

	if (scaled_ppm < 0) {
		neg_adj = 1;
		scaled_ppm = -scaled_ppm;
	}
	rate = scaled_ppm;

	/* word: unused(8bit) | ns(8bit) | fractions(16bit) */
	word = (bp->ns_incr << 16) + bp->subns_incr;

	adj = word;
	adj *= rate;
	adj += 500000UL << 16;
	adj >>= 16; /* remove fractions */
	diff = div_u64(adj, 1000000UL);
	word = neg_adj ? word - diff : word + diff;

	spin_lock(&bp->tsu_clk_lock);

	gem_writel(bp, TISUBN, GEM_BF(SUBNSINCR, (word & 0xffff)));
	gem_writel(bp, TI, GEM_BF(NSINCR, (word >> 16)));

	spin_unlock(&bp->tsu_clk_lock);
	return 0;
}

static int gem_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct macb *bp = container_of(ptp, struct macb, ptp_caps);
	struct timespec64 now, then = ns_to_timespec64(delta);
	u32 adj, sign = 0;

	if (delta < 0) {
		delta = -delta;
		sign = 1;
	}

	if (delta > 0x3FFFFFFF) {
		gem_tsu_get_time(bp, &now);

		if (sign)
			now = timespec64_sub(now, then);
		else
			now = timespec64_add(now, then);

		gem_tsu_set_time(bp, (const struct timespec64 *)&now);
	} else {
		adj = delta;
		if (sign)
			adj |= GEM_BIT(ADDSUB);

		gem_writel(bp, TA, adj);
	}

	return 0;
}

static int gem_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct macb *bp = container_of(ptp, struct macb, ptp_caps);

	gem_tsu_get_time(bp, ts);

	return 0;
}

static int gem_ptp_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct macb *bp = container_of(ptp, struct macb, ptp_caps);

	gem_tsu_set_time(bp, ts);

	return 0;
}

static int gem_ptp_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

static struct ptp_clock_info gem_ptp_caps_template = {
	.owner		= THIS_MODULE,
	.name		= GEM_PTP_TIMER_NAME,
	.max_adj	= 0,
	.n_alarm	= 0,
	.n_ext_ts	= 0,
	.n_per_out	= 0,
	.n_pins		= 0,
	.pps		= 0,
	.adjfine	= gem_ptp_adjfine,
	.adjtime	= gem_ptp_adjtime,
	.gettime64	= gem_ptp_gettime,
	.settime64	= gem_ptp_settime,
	.enable		= gem_ptp_enable,
};

static void gem_ptp_init_timer(struct macb *bp)
{
	struct timespec64 now;
	u32 rem = 0;

	getnstimeofday64(&now);
	gem_tsu_set_time(bp, (const struct timespec64 *)&now);

	bp->ns_incr = div_u64_rem(NSEC_PER_SEC, bp->tsu_rate, &rem);
	if (rem) {
		u64 adj = rem;

		adj <<= 16; /* 16 bits nsec fragments */
		bp->subns_incr = div_u64(adj, bp->tsu_rate);
	} else {
		bp->subns_incr = 0;
	}

	gem_writel(bp, TISUBN, GEM_BF(SUBNSINCR, bp->subns_incr));
	gem_writel(bp, TI, GEM_BF(NSINCR, bp->ns_incr));
	gem_writel(bp, TA, 0);
}

static void gem_ptp_clear_timer(struct macb *bp)
{
	bp->ns_incr = 0;
	bp->subns_incr = 0;

	gem_writel(bp, TISUBN, GEM_BF(SUBNSINCR, 0));
	gem_writel(bp, TI, GEM_BF(NSINCR, 0));
	gem_writel(bp, TA, 0);
}

/* While GEM can timestamp PTP packets, it does not mark the RX descriptor
 * to identify them. UDP packets must be parsed to identify PTP packets.
 *
 * Note: Inspired from drivers/net/ethernet/ti/cpts.c
 */
static int gem_get_ptp_peer(struct sk_buff *skb, int ptp_class)
{
	unsigned int offset = 0;
	u8 *msgtype, *data = skb->data;

	/* PTP frames are rare! */
	if (likely(ptp_class == PTP_CLASS_NONE))
		return -1;

	if (ptp_class & PTP_CLASS_VLAN)
		offset += VLAN_HLEN;

	switch (ptp_class & PTP_CLASS_PMASK) {
	case PTP_CLASS_IPV4:
		offset += ETH_HLEN + IPV4_HLEN(data + offset) + UDP_HLEN;
	break;
	case PTP_CLASS_IPV6:
		offset += ETH_HLEN + IP6_HLEN + UDP_HLEN;
	break;
	case PTP_CLASS_L2:
		offset += ETH_HLEN;
		break;

	/* something went wrong! */
	default:
		return -1;
	}

	if (skb->len + ETH_HLEN < offset + OFF_PTP_SEQUENCE_ID)
		return -1;

	if (unlikely(ptp_class & PTP_CLASS_V1))
		msgtype = data + offset + OFF_PTP_CONTROL;
	else
		msgtype = data + offset;

	return (*msgtype) & 0x2;
}

static void gem_ptp_tx_hwtstamp(struct macb *bp, struct sk_buff *skb,
				int peer_ev)
{
	struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);
	struct timespec64 ts;
	u64 ns;

	/* PTP Peer Event Frame packets */
	if (peer_ev) {
		ts.tv_sec = gem_readl(bp, PEFTSL);
		ts.tv_nsec = gem_readl(bp, PEFTN);

	/* PTP Event Frame packets */
	} else {
		ts.tv_sec = gem_readl(bp, EFTSL);
		ts.tv_nsec = gem_readl(bp, EFTN);
	}
	ns = timespec64_to_ns(&ts);

	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ns_to_ktime(ns);
	skb_tstamp_tx(skb, skb_hwtstamps(skb));
}

static void gem_ptp_rx_hwtstamp(struct macb *bp, struct sk_buff *skb,
				int peer_ev)
{
	struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);
	struct timespec64 ts;
	u64 ns;

	if (peer_ev) {
		/* PTP Peer Event Frame packets */
		ts.tv_sec = gem_readl(bp, PEFRSL);
		ts.tv_nsec = gem_readl(bp, PEFRN);
	} else {
		/* PTP Event Frame packets */
		ts.tv_sec = gem_readl(bp, EFRSL);
		ts.tv_nsec = gem_readl(bp, EFRN);
	}
	ns = timespec64_to_ns(&ts);

	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ns_to_ktime(ns);
}

/* no static, GEM PTP interface functions */
void gem_ptp_txstamp(struct macb *bp, struct sk_buff *skb)
{
	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
		int class = ptp_classify_raw(skb);
		int peer;

		peer = gem_get_ptp_peer(skb, class);
		if (peer < 0)
			return;

		/* Timestamp this packet */
		gem_ptp_tx_hwtstamp(bp, skb, peer);
	}
}

void gem_ptp_rxstamp(struct macb *bp, struct sk_buff *skb)
{
	int class, peer;

	__skb_push(skb, ETH_HLEN);
	class = ptp_classify_raw(skb);
	__skb_pull(skb, ETH_HLEN);

	peer = gem_get_ptp_peer(skb, class);
	if (peer < 0)
		return;

	gem_ptp_rx_hwtstamp(bp, skb, peer);
}

void gem_ptp_init(struct net_device *ndev)
{
	struct macb *bp = netdev_priv(ndev);

	spin_lock_init(&bp->tsu_clk_lock);
	bp->ptp_caps = gem_ptp_caps_template;

	/* nominal frequency and maximum adjustment in ppb */
	bp->tsu_rate = bp->ptp_info->get_tsu_rate(bp);
	bp->ptp_caps.max_adj = bp->ptp_info->get_ptp_max_adj();

	gem_ptp_init_timer(bp);

	bp->ptp_clock = ptp_clock_register(&bp->ptp_caps, NULL);
	if (IS_ERR(&bp->ptp_clock)) {
		bp->ptp_clock = NULL;
		pr_err("ptp clock register failed\n");
		return;
	}

	dev_info(&bp->pdev->dev, "%s ptp clock registered.\n",
		 GEM_PTP_TIMER_NAME);
}

void gem_ptp_remove(struct net_device *ndev)
{
	struct macb *bp = netdev_priv(ndev);

	if (bp->ptp_clock)
		ptp_clock_unregister(bp->ptp_clock);

	gem_ptp_clear_timer(bp);

	dev_info(&bp->pdev->dev, "%s ptp clock unregistered.\n",
		 GEM_PTP_TIMER_NAME);
}
