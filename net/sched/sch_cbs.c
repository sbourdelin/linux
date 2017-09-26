/*
 * net/sched/sch_cbs.c	Credit Based Shaper
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Vinicius Costa Gomes <vinicius.gomes@intel.com>
 *
 */

/* Credit Based Shaper (CBS)
   =========================

   This is a simple rate-limiting shaper aimed at TSN applications on
   systems with known traffic workloads.

   Its algorithm is defined by the IEEE 802.1Q-2014 Specification,
   Section 8.6.8.2, and explained in more detail in the Annex L of the
   same specification.

   There are four tunables to be considered:

	'idleslope': Idleslope is the rate of credits that is
	accumulated (in kilobits per second) when there is at least
	one packet waiting for transmission. Packets are transmitted
	when the current value of credits is equal or greater than
	zero. When there is no packet to be transmitted the amount of
	credits is set to zero. This is the main tunable of the CBS
	algorithm.

	'sendslope':
	Sendslope is the rate of credits that is depleted (it should be a
	negative number of kilobits per second) when a transmission is
	ocurring. It can be calculated as follows, (IEEE 802.1Q-2014 Section
	8.6.8.2 item g):

	sendslope = idleslope - port_transmit_rate

	'hicredit': Hicredit defines the maximum amount of credits (in
	bytes) that can be accumulated. Hicredit depends on the
	characteristics of interfering traffic,
	'max_interference_size' is the maximum size of any burst of
	traffic that can delay the transmission of a frame that is
	available for transmission for this traffic class, (IEEE
	802.1Q-2014 Annex L, Equation L-3):

	hicredit = max_interference_size * (idleslope / port_transmit_rate)

	'locredit': Locredit is the minimum amount of credits that can
	be reached. It is a function of the traffic flowing through
	this qdisc (IEEE 802.1Q-2014 Annex L, Equation L-2):

	locredit = max_frame_size * (sendslope / port_transmit_rate)
*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/netlink.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>

struct cbs_sched_data {
	s32 queue;
	s32 locredit;
	s32 hicredit;
	s32 sendslope;
	s32 idleslope;
};

static int cbs_enqueue(struct sk_buff *skb, struct Qdisc *sch,
		       struct sk_buff **to_free)
{
	return qdisc_enqueue_tail(skb, sch);
}

static const struct nla_policy cbs_policy[TCA_CBS_MAX + 1] = {
	[TCA_CBS_PARMS]	= { .len = sizeof(struct tc_cbs_qopt) },
};

static int cbs_change(struct Qdisc *sch, struct nlattr *opt)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct tc_cbs_qopt_offload cbs = { };
	struct nlattr *tb[TCA_CBS_MAX + 1];
	const struct net_device_ops *ops;
	struct tc_cbs_qopt *qopt;
	struct net_device *dev;
	int err;

	err = nla_parse_nested(tb, TCA_CBS_MAX, opt, cbs_policy, NULL);
	if (err < 0)
		return err;

	err = -EINVAL;
	if (!tb[TCA_CBS_PARMS])
		goto done;

	qopt = nla_data(tb[TCA_CBS_PARMS]);

	dev = qdisc_dev(sch);
	ops = dev->netdev_ops;

	cbs.queue = q->queue;
	cbs.enable = 1;
	cbs.hicredit = qopt->hicredit;
	cbs.locredit = qopt->locredit;
	cbs.idleslope = qopt->idleslope;
	cbs.sendslope = qopt->sendslope;

	err = -EOPNOTSUPP;
	if (!ops->ndo_setup_tc)
		goto done;

	err = ops->ndo_setup_tc(dev, TC_SETUP_CBS, &cbs);
	if (err < 0)
		goto done;

	q->hicredit = cbs.hicredit;
	q->locredit = cbs.locredit;
	q->idleslope = cbs.idleslope;
	q->sendslope = cbs.sendslope;

done:
	return err;
}

static int cbs_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct net_device *dev = qdisc_dev(sch);

	if (!opt)
		return -EINVAL;

	/* FIXME: this means that we can only install this qdisc
	 * "under" mqprio. Do we need a more generic way to retrieve
	 * the queue, or do we pass the netdev_queue to the driver?
	 */
	q->queue = TC_H_MIN(sch->parent) - 1 - netdev_get_num_tc(dev);

	return cbs_change(sch, opt);
}

static void cbs_destroy(struct Qdisc *sch)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct tc_cbs_qopt_offload cbs = { };
	const struct net_device_ops *ops;
	struct net_device *dev;
	int err;

	q->hicredit = 0;
	q->locredit = 0;
	q->idleslope = 0;
	q->sendslope = 0;

	dev = qdisc_dev(sch);
	ops = dev->netdev_ops;

	if (!ops->ndo_setup_tc)
		return;

	cbs.queue = q->queue;
	cbs.enable = 0;

	err = ops->ndo_setup_tc(dev, TC_SETUP_CBS, &cbs);
	if (err < 0)
		pr_warn("Couldn't reset queue %d to default values\n",
			cbs.queue);
}

static int cbs_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct nlattr *nest;
	struct tc_cbs_qopt opt;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;

	opt.hicredit = q->hicredit;
	opt.locredit = q->locredit;
	opt.sendslope = q->sendslope;
	opt.idleslope = q->idleslope;

	if (nla_put(skb, TCA_CBS_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;

	return nla_nest_end(skb, nest);

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -1;
}

static struct Qdisc_ops cbs_qdisc_ops __read_mostly = {
	.next		=	NULL,
	.id		=	"cbs",
	.priv_size	=	sizeof(struct cbs_sched_data),
	.enqueue	=	cbs_enqueue,
	.dequeue	=	qdisc_dequeue_head,
	.peek		=	qdisc_peek_dequeued,
	.init		=	cbs_init,
	.reset		=	qdisc_reset_queue,
	.destroy	=	cbs_destroy,
	.change		=	cbs_change,
	.dump		=	cbs_dump,
	.owner		=	THIS_MODULE,
};

static int __init cbs_module_init(void)
{
	return register_qdisc(&cbs_qdisc_ops);
}

static void __exit cbs_module_exit(void)
{
	unregister_qdisc(&cbs_qdisc_ops);
}
module_init(cbs_module_init)
module_exit(cbs_module_exit)
MODULE_LICENSE("GPL");
