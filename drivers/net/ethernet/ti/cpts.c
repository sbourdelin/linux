/*
 * TI Common Platform Time Sync
 *
 * Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <linux/err.h>
#include <linux/if.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_classify.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>

#include "cpts.h"

struct cpts_skb_cb_data {
	u32 skb_mtype_seqid;
	unsigned long tmo;
};

#define cpts_read32(c, r)	readl_relaxed(&c->reg->r)
#define cpts_write32(c, v, r)	writel_relaxed(v, &c->reg->r)

static int cpts_event_port(struct cpts_event *event)
{
	return (event->high >> PORT_NUMBER_SHIFT) & PORT_NUMBER_MASK;
}

static int event_expired(struct cpts_event *event)
{
	return time_after(jiffies, event->tmo);
}

static int event_type(struct cpts_event *event)
{
	return (event->high >> EVENT_TYPE_SHIFT) & EVENT_TYPE_MASK;
}

static int cpts_fifo_pop(struct cpts *cpts, u32 *high, u32 *low)
{
	u32 r = cpts_read32(cpts, intstat_raw);

	if (r & TS_PEND_RAW) {
		*high = cpts_read32(cpts, event_high);
		*low  = cpts_read32(cpts, event_low);
		cpts_write32(cpts, EVENT_POP, event_pop);
		return 0;
	}
	return -1;
}

static int cpts_purge_events(struct cpts *cpts)
{
	struct list_head *this, *next;
	struct cpts_event *event;
	int removed = 0;

	list_for_each_safe(this, next, &cpts->events) {
		event = list_entry(this, struct cpts_event, list);
		if (event_expired(event)) {
			list_del_init(&event->list);
			list_add(&event->list, &cpts->pool);
			cpts->event_tmo++;
			dev_dbg(cpts->dev, "purge: event tmo:: high:%08X low:%08x\n",
				event->high, event->low);
			++removed;
		}
	}

	if (removed)
		dev_dbg(cpts->dev, "event pool cleaned up %d\n", removed);
	return removed ? 0 : -1;
}

static u64 cpts_systim_read_irq(const struct cyclecounter *cc)
{
	u64 val;
	unsigned long flags;
	struct cpts *cpts = container_of(cc, struct cpts, cc);

	spin_lock_irqsave(&cpts->lock, flags);
	val = cpts->cur_timestamp;
	spin_unlock_irqrestore(&cpts->lock, flags);

	return val;
}

/* PTP clock operations */

static void cpts_ptp_update_time(struct cpts *cpts)
{
	reinit_completion(&cpts->ts_push_complete);

	cpts_write32(cpts, TS_PUSH, ts_push);
	wait_for_completion_interruptible_timeout(&cpts->ts_push_complete, HZ);
}

static int cpts_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	u64 adj;
	u32 diff, mult;
	int neg_adj = 0;
	struct cpts *cpts = container_of(ptp, struct cpts, info);

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}

	mutex_lock(&cpts->ptp_clk_mutex);

	mult = cpts->cc_mult;
	adj = mult;
	adj *= ppb;
	diff = div_u64(adj, 1000000000ULL);

	cpts_ptp_update_time(cpts);

	timecounter_read(&cpts->tc);

	cpts->cc.mult = neg_adj ? mult - diff : mult + diff;
	mutex_unlock(&cpts->ptp_clk_mutex);

	return 0;
}

static int cpts_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct cpts *cpts = container_of(ptp, struct cpts, info);

	mutex_lock(&cpts->ptp_clk_mutex);
	timecounter_adjtime(&cpts->tc, delta);
	mutex_unlock(&cpts->ptp_clk_mutex);

	return 0;
}

static int cpts_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	u64 ns;
	struct cpts *cpts = container_of(ptp, struct cpts, info);

	mutex_lock(&cpts->ptp_clk_mutex);
	cpts_ptp_update_time(cpts);

	ns = timecounter_read(&cpts->tc);

	*ts = ns_to_timespec64(ns);
	mutex_unlock(&cpts->ptp_clk_mutex);

	return 0;
}

static int cpts_ptp_settime(struct ptp_clock_info *ptp,
			    const struct timespec64 *ts)
{
	u64 ns;
	struct cpts *cpts = container_of(ptp, struct cpts, info);

	mutex_lock(&cpts->ptp_clk_mutex);
	ns = timespec64_to_ns(ts);

	cpts_ptp_update_time(cpts);

	timecounter_init(&cpts->tc, &cpts->cc, ns);
	mutex_unlock(&cpts->ptp_clk_mutex);

	return 0;
}

/* HW TS */
static int cpts_extts_enable(struct cpts *cpts, u32 index, int on)
{
	u32 v;

	if (index >= cpts->info.n_ext_ts)
		return -ENXIO;

	if (((cpts->hw_ts_enable & BIT(index)) >> index) == on)
		return 0;

	mutex_lock(&cpts->ptp_clk_mutex);

	v = cpts_read32(cpts, control);
	if (on) {
		v |= BIT(8 + index);
		cpts->hw_ts_enable |= BIT(index);
	} else {
		v &= ~BIT(8 + index);
		cpts->hw_ts_enable &= ~BIT(index);
	}
	cpts_write32(cpts, v, control);

	mutex_unlock(&cpts->ptp_clk_mutex);

	return 0;
}

static int cpts_ptp_enable(struct ptp_clock_info *ptp,
			   struct ptp_clock_request *rq, int on)
{
	struct cpts *cpts = container_of(ptp, struct cpts, info);

	switch (rq->type) {
	case PTP_CLK_REQ_EXTTS:
		return cpts_extts_enable(cpts, rq->extts.index, on);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static struct ptp_clock_info cpts_info = {
	.owner		= THIS_MODULE,
	.name		= "CTPS timer",
	.max_adj	= 1000000,
	.n_ext_ts	= 0,
	.n_pins		= 0,
	.pps		= 0,
	.adjfreq	= cpts_ptp_adjfreq,
	.adjtime	= cpts_ptp_adjtime,
	.gettime64	= cpts_ptp_gettime,
	.settime64	= cpts_ptp_settime,
	.enable		= cpts_ptp_enable,
};

static void cpts_overflow_check(struct work_struct *work)
{
	struct cpts *cpts = container_of(work, struct cpts, overflow_work.work);
	struct timespec64 ts;
	unsigned long flags;

	cpts_ptp_gettime(&cpts->info, &ts);

	pr_debug("cpts overflow check at %lld.%09lu\n", ts.tv_sec, ts.tv_nsec);
	queue_delayed_work(cpts->workwq, &cpts->overflow_work,
			   cpts->ov_check_period);

	spin_lock_irqsave(&cpts->lock, flags);
	cpts_purge_events(cpts);
	spin_unlock_irqrestore(&cpts->lock, flags);

	queue_work(cpts->workwq, &cpts->ts_work);
}

static irqreturn_t cpts_misc_interrupt(int irq, void *dev_id)
{
	struct ptp_clock_event pevent;
	struct cpts *cpts = dev_id;
	unsigned long flags;
	int i, type = -1;
	u32 hi, lo;
	struct cpts_event *event;
	bool wake = false;

	spin_lock_irqsave(&cpts->lock, flags);

	for (i = 0; i < CPTS_FIFO_DEPTH; i++) {
		if (cpts_fifo_pop(cpts, &hi, &lo))
			break;

		if (list_empty(&cpts->pool) && cpts_purge_events(cpts)) {
			dev_err(cpts->dev, "event pool empty\n");
			spin_unlock_irqrestore(&cpts->lock, flags);
			return IRQ_HANDLED;
		}

		event = list_first_entry(&cpts->pool, struct cpts_event, list);
		event->high = hi;
		event->low = lo;
		type = event_type(event);
		dev_dbg(cpts->dev, "CPTS_EV: %d high:%08X low:%08x\n",
			type, event->high, event->low);
		switch (type) {
		case CPTS_EV_PUSH:
			cpts->cur_timestamp = lo;
			complete(&cpts->ts_push_complete);
			break;
		case CPTS_EV_TX:
		case CPTS_EV_RX:
			event->tmo = jiffies + msecs_to_jiffies(1000);
			event->timestamp = timecounter_cyc2time(&cpts->tc,
								event->low);
			list_del_init(&event->list);
			list_add_tail(&event->list, &cpts->events);

			wake = true;
			break;
		case CPTS_EV_ROLL:
		case CPTS_EV_HALF:
			break;
		case CPTS_EV_HW:
			pevent.timestamp = timecounter_cyc2time(&cpts->tc, event->low);
			pevent.type = PTP_CLOCK_EXTTS;
			pevent.index = cpts_event_port(event) - 1;
			ptp_clock_event(cpts->clock, &pevent);
			break;
		default:
			dev_err(cpts->dev, "unknown event type\n");
			break;
		}
	}

	spin_unlock_irqrestore(&cpts->lock, flags);
	if (wake)
		queue_work(cpts->workwq, &cpts->ts_work);

	return IRQ_HANDLED;
}

static int cpts_skb_get_mtype_seqid(struct sk_buff *skb, u32 *mtype_seqid)
{
	unsigned int ptp_class = ptp_classify_raw(skb);
	u8 *msgtype, *data = skb->data;
	unsigned int offset = 0;
	u16 *seqid;

	if (ptp_class == PTP_CLASS_NONE)
		return 0;

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
	default:
		return 0;
	}

	if (skb->len + ETH_HLEN < offset + OFF_PTP_SEQUENCE_ID + sizeof(*seqid))
		return 0;

	if (unlikely(ptp_class & PTP_CLASS_V1))
		msgtype = data + offset + OFF_PTP_CONTROL;
	else
		msgtype = data + offset;

	seqid = (u16 *)(data + offset + OFF_PTP_SEQUENCE_ID);
	*mtype_seqid = (*msgtype & MESSAGE_TYPE_MASK) << MESSAGE_TYPE_SHIFT;
	*mtype_seqid |= (ntohs(*seqid) & SEQUENCE_ID_MASK) << SEQUENCE_ID_SHIFT;

	return 1;
}

static u64 cpts_find_ts(struct cpts *cpts, u32 skb_mtype_seqid)
{
	u64 ns = 0;
	struct cpts_event *event;
	struct list_head *this, *next;
	unsigned long flags;
	u32 mtype_seqid;

	spin_lock_irqsave(&cpts->lock, flags);
	list_for_each_safe(this, next, &cpts->events) {
		event = list_entry(this, struct cpts_event, list);
		if (event_expired(event)) {
			list_del_init(&event->list);
			list_add(&event->list, &cpts->pool);
			cpts->event_tmo++;
			dev_dbg(cpts->dev, "%s: event tmo: high:%08X low:%08x\n",
				__func__, event->high, event->low);
			continue;
		}
		mtype_seqid = event->high &
			      ((MESSAGE_TYPE_MASK << MESSAGE_TYPE_SHIFT) |
			       (SEQUENCE_ID_MASK << SEQUENCE_ID_SHIFT) |
			       (EVENT_TYPE_MASK << EVENT_TYPE_SHIFT));

		if (mtype_seqid == skb_mtype_seqid) {
			ns = event->timestamp;
			list_del_init(&event->list);
			list_add(&event->list, &cpts->pool);
			break;
		}
	}
	spin_unlock_irqrestore(&cpts->lock, flags);

	return ns;
}

static void cpts_ts_work(struct work_struct *work)
{
	struct cpts *cpts = container_of(work, struct cpts, ts_work);
	struct sk_buff *skb, *tmp;
	struct sk_buff_head tempq;

	spin_lock_bh(&cpts->txq.lock);
	skb_queue_walk_safe(&cpts->txq, skb, tmp) {
		struct skb_shared_hwtstamps ssh;
		u64 ns;
		struct cpts_skb_cb_data *skb_cb =
			(struct cpts_skb_cb_data *)skb->cb;

		ns = cpts_find_ts(cpts, skb_cb->skb_mtype_seqid);
		if (ns) {
			__skb_unlink(skb, &cpts->txq);
			memset(&ssh, 0, sizeof(ssh));
			ssh.hwtstamp = ns_to_ktime(ns);
			skb->tstamp = 0;
			skb_tstamp_tx(skb, &ssh);
			consume_skb(skb);
		} else if (time_after(jiffies, skb_cb->tmo)) {
			/* timeout any expired skbs over 1s */
			dev_err(cpts->dev,
				"expiring tx timestamp mtype seqid %08x\n",
				skb_cb->skb_mtype_seqid);
			__skb_unlink(skb, &cpts->txq);
			kfree_skb(skb);
			cpts->tx_tmo++;
		}
	}
	spin_unlock_bh(&cpts->txq.lock);

	__skb_queue_head_init(&tempq);
	spin_lock_bh(&cpts->rxq.lock);
	skb_queue_walk_safe(&cpts->rxq, skb, tmp) {
		struct skb_shared_hwtstamps *ssh;
		u64 ns;
		struct cpts_skb_cb_data *skb_cb =
			(struct cpts_skb_cb_data *)skb->cb;

		ns = cpts_find_ts(cpts, skb_cb->skb_mtype_seqid);
		if (ns) {
			__skb_unlink(skb, &cpts->rxq);
			ssh = skb_hwtstamps(skb);
			memset(ssh, 0, sizeof(*ssh));
			ssh->hwtstamp = ns_to_ktime(ns);
			__skb_queue_tail(&tempq, skb);
		} else if (time_after(jiffies, skb_cb->tmo)) {
			/* timeout any expired skbs over 1s */
			dev_err(cpts->dev,
				"expiring rx timestamp mtype seqid %08x\n",
				skb_cb->skb_mtype_seqid);
			__skb_unlink(skb, &cpts->rxq);
			__skb_queue_tail(&tempq, skb);
			cpts->fail_rx++;
		}
	}
	spin_unlock_bh(&cpts->rxq.lock);

	local_bh_disable();
	while ((skb = __skb_dequeue(&tempq)))
		netif_receive_skb(skb);
	local_bh_enable();
}

int cpts_rx_timestamp(struct cpts *cpts, struct sk_buff *skb)
{
	struct cpts_skb_cb_data *skb_cb = (struct cpts_skb_cb_data *)skb->cb;
	struct skb_shared_hwtstamps *ssh;
	int ret;
	u64 ns;

	if (!cpts->rx_enable)
		return 0;

	ret = cpts_skb_get_mtype_seqid(skb, &skb_cb->skb_mtype_seqid);
	if (!ret)
		return 0;

	skb_cb->skb_mtype_seqid |= (CPTS_EV_RX << EVENT_TYPE_SHIFT);

	dev_dbg(cpts->dev, "%s mtype seqid %08x\n",
		__func__, skb_cb->skb_mtype_seqid);

	ns = cpts_find_ts(cpts, skb_cb->skb_mtype_seqid);
	if (!ns) {
		skb_cb->tmo = jiffies + msecs_to_jiffies(1000);
		skb_queue_tail(&cpts->rxq, skb);
		queue_work(cpts->workwq, &cpts->ts_work);
		dev_dbg(cpts->dev, "%s push skb\n", __func__);
		return 1;
	}

	ssh = skb_hwtstamps(skb);
	memset(ssh, 0, sizeof(*ssh));
	ssh->hwtstamp = ns_to_ktime(ns);
	return 0;
}
EXPORT_SYMBOL_GPL(cpts_rx_timestamp);

void cpts_tx_timestamp(struct cpts *cpts, struct sk_buff *skb)
{
	struct cpts_skb_cb_data *skb_cb = (struct cpts_skb_cb_data *)skb->cb;
	struct skb_shared_hwtstamps ssh;
	int ret;
	u64 ns;

	if (!(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS))
		return;

	ret = cpts_skb_get_mtype_seqid(skb, &skb_cb->skb_mtype_seqid);
	if (!ret)
		return;

	skb_cb->skb_mtype_seqid |= (CPTS_EV_TX << EVENT_TYPE_SHIFT);

	dev_dbg(cpts->dev, "%s mtype seqid %08x\n",
		__func__, skb_cb->skb_mtype_seqid);

	ns = cpts_find_ts(cpts, skb_cb->skb_mtype_seqid);
	if (!ns) {
		skb_get(skb);
		skb_cb->tmo = jiffies + msecs_to_jiffies(1000);
		skb_queue_tail(&cpts->txq, skb);
		queue_work(cpts->workwq, &cpts->ts_work);
		dev_dbg(cpts->dev, "%s skb push\n", __func__);
		return;
	}

	memset(&ssh, 0, sizeof(ssh));
	ssh.hwtstamp = ns_to_ktime(ns);
	skb_tstamp_tx(skb, &ssh);
}
EXPORT_SYMBOL_GPL(cpts_tx_timestamp);

int cpts_register(struct cpts *cpts)
{
	int err, i;

	skb_queue_head_init(&cpts->txq);
	skb_queue_head_init(&cpts->rxq);

	INIT_LIST_HEAD(&cpts->events);
	INIT_LIST_HEAD(&cpts->pool);
	for (i = 0; i < CPTS_MAX_EVENTS; i++)
		list_add(&cpts->pool_data[i].list, &cpts->pool);

	clk_enable(cpts->refclk);

	cpts_write32(cpts, CPTS_EN, control);
	cpts_write32(cpts, TS_PEND_EN, int_enable);

	cpts_ptp_update_time(cpts);
	timecounter_init(&cpts->tc, &cpts->cc, ktime_to_ns(ktime_get_real()));

	cpts->clock = ptp_clock_register(&cpts->info, cpts->dev);
	if (IS_ERR(cpts->clock)) {
		err = PTR_ERR(cpts->clock);
		cpts->clock = NULL;
		goto err_ptp;
	}
	cpts->phc_index = ptp_clock_index(cpts->clock);

	queue_delayed_work(cpts->workwq, &cpts->overflow_work,
			   cpts->ov_check_period);
	return 0;

err_ptp:
	clk_disable(cpts->refclk);
	return err;
}
EXPORT_SYMBOL_GPL(cpts_register);

void cpts_unregister(struct cpts *cpts)
{
	if (WARN_ON(!cpts->clock))
		return;

	cancel_delayed_work_sync(&cpts->overflow_work);

	ptp_clock_unregister(cpts->clock);
	cpts->clock = NULL;

	cpts_write32(cpts, 0, int_enable);
	cpts_write32(cpts, 0, control);
	cancel_work_sync(&cpts->ts_work);

	/* Drop all packet */
	skb_queue_purge(&cpts->txq);
	skb_queue_purge(&cpts->rxq);

	clk_disable(cpts->refclk);
}
EXPORT_SYMBOL_GPL(cpts_unregister);

static void cpts_calc_mult_shift(struct cpts *cpts)
{
	u64 frac, maxsec, ns;
	u32 freq;

	freq = clk_get_rate(cpts->refclk);

	/* Calc the maximum number of seconds which we can run before
	 * wrapping around.
	 */
	maxsec = cpts->cc.mask;
	do_div(maxsec, freq);
	/* limit conversation rate to 10 sec as higher values will produce
	 * too small mult factors and so reduce the conversion accuracy
	 */
	if (maxsec > 10)
		maxsec = 10;

	/* Calc overflow check period (maxsec / 2) */
	cpts->ov_check_period = (HZ * maxsec) / 2;

	dev_info(cpts->dev, "cpts: overflow check period %lu (jiffies)\n",
		 cpts->ov_check_period);

	if (cpts->cc.mult || cpts->cc.shift)
		return;

	clocks_calc_mult_shift(&cpts->cc.mult, &cpts->cc.shift,
			       freq, NSEC_PER_SEC, maxsec);

	frac = 0;
	ns = cyclecounter_cyc2ns(&cpts->cc, freq, cpts->cc.mask, &frac);

	dev_info(cpts->dev,
		 "CPTS: ref_clk_freq:%u calc_mult:%u calc_shift:%u error:%lld nsec/sec\n",
		 freq, cpts->cc.mult, cpts->cc.shift, (ns - NSEC_PER_SEC));
}

static int cpts_of_parse(struct cpts *cpts, struct device_node *node)
{
	int ret = -EINVAL;
	u32 prop;

	if (!of_property_read_u32(node, "cpts_clock_mult", &prop))
		cpts->cc.mult = prop;

	if (!of_property_read_u32(node, "cpts_clock_shift", &prop))
		cpts->cc.shift = prop;

	if ((cpts->cc.mult && !cpts->cc.shift) ||
	    (!cpts->cc.mult && cpts->cc.shift))
		goto of_error;

	if (!of_property_read_u32(node, "cpts-ext-ts-inputs", &prop))
		cpts->ext_ts_inputs = prop;

	return 0;

of_error:
	dev_err(cpts->dev, "CPTS: Missing property in the DT.\n");
	return ret;
}

struct cpts *cpts_create(struct device *dev, void __iomem *regs,
			 struct device_node *node, int irq)
{
	struct cpts *cpts;
	int ret;

	cpts = devm_kzalloc(dev, sizeof(*cpts), GFP_KERNEL);
	if (!cpts)
		return ERR_PTR(-ENOMEM);

	cpts->dev = dev;
	cpts->reg = (struct cpsw_cpts __iomem *)regs;
	cpts->irq = irq;
	spin_lock_init(&cpts->lock);
	mutex_init(&cpts->ptp_clk_mutex);
	INIT_DELAYED_WORK(&cpts->overflow_work, cpts_overflow_check);
	cpts->workwq = alloc_ordered_workqueue("cpts_ptp",
					       WQ_MEM_RECLAIM | WQ_HIGHPRI);
	if (!cpts->workwq)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&cpts->ts_work, cpts_ts_work);
	init_completion(&cpts->ts_push_complete);

	ret = cpts_of_parse(cpts, node);
	if (ret)
		return ERR_PTR(ret);

	cpts->refclk = devm_clk_get(dev, "cpts");
	if (IS_ERR(cpts->refclk)) {
		dev_err(dev, "Failed to get cpts refclk\n");
		return ERR_PTR(PTR_ERR(cpts->refclk));
	}

	clk_prepare(cpts->refclk);

	cpts->cc.read = cpts_systim_read_irq;
	cpts->cc.mask = CLOCKSOURCE_MASK(32);
	cpts->info = cpts_info;

	if (cpts->ext_ts_inputs)
		cpts->info.n_ext_ts = cpts->ext_ts_inputs;

	cpts_calc_mult_shift(cpts);
	/* save cc.mult original value as it can be modified
	 * by cpts_ptp_adjfreq().
	 */
	cpts->cc_mult = cpts->cc.mult;

	ret = devm_request_irq(dev, irq, cpts_misc_interrupt,
			       IRQF_ONESHOT | IRQF_SHARED, dev_name(dev), cpts);
	if (ret < 0) {
		dev_err(dev, "error attaching irq (%d)\n", ret);
		return ERR_PTR(ret);
	}

	return cpts;
}
EXPORT_SYMBOL_GPL(cpts_create);

void cpts_release(struct cpts *cpts)
{
	if (!cpts)
		return;

	if (WARN_ON(!cpts->refclk))
		return;

	clk_unprepare(cpts->refclk);
}
EXPORT_SYMBOL_GPL(cpts_release);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI CPTS driver");
MODULE_AUTHOR("Richard Cochran <richardcochran@gmail.com>");
