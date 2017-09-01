/*
 * net/sched/sch_cbs.c	Credit Based Shaper
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Vininicius Costa Gomes <vinicius.gomes@intel.com>
 *
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
	struct Qdisc *qdisc; /* Inner qdisc, default - pfifo queue */
	s32 queue;
	s32 locredit;
	s32 hicredit;
	s32 sendslope;
	s32 idleslope;
};

static int cbs_enqueue(struct sk_buff *skb, struct Qdisc *sch,
		       struct sk_buff **to_free)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	int ret;

	ret = qdisc_enqueue(skb, q->qdisc, to_free);
	if (ret != NET_XMIT_SUCCESS) {
		if (net_xmit_drop_count(ret))
			qdisc_qstats_drop(sch);
		return ret;
	}

	qdisc_qstats_backlog_inc(sch, skb);
	sch->q.qlen++;
	return NET_XMIT_SUCCESS;
}

static struct sk_buff *cbs_dequeue(struct Qdisc *sch)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;

	skb = q->qdisc->ops->peek(q->qdisc);
	if (skb) {
		skb = qdisc_dequeue_peeked(q->qdisc);
		if (unlikely(!skb))
			return NULL;

		qdisc_qstats_backlog_dec(sch, skb);
		sch->q.qlen--;
		qdisc_bstats_update(sch, skb);

		return skb;
	}
	return NULL;
}

static void cbs_reset(struct Qdisc *sch)
{
	struct cbs_sched_data *q = qdisc_priv(sch);

	qdisc_reset(q->qdisc);
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

	/* FIXME: this means that we can only install this qdisc
	 * "under" mqprio. Do we need a more generic way to retrieve
	 * the queue, or do we pass the netdev_queue to the driver?
	 */
	cbs.queue = TC_H_MIN(sch->parent) - 1 - netdev_get_num_tc(dev);

	cbs.enable = 1;
	cbs.hicredit = qopt->hicredit;
	cbs.locredit = qopt->locredit;
	cbs.idleslope = qopt->idleslope;
	cbs.sendslope = qopt->sendslope;

	err = -ENOTSUPP;
	if (!ops->ndo_setup_tc)
		goto done;

	err = dev->netdev_ops->ndo_setup_tc(dev, TC_SETUP_CBS, &cbs);
	if (err < 0)
		goto done;

	q->queue = cbs.queue;
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

	if (!opt)
		return -EINVAL;

	q->qdisc = fifo_create_dflt(sch, &pfifo_qdisc_ops, 1024);
	qdisc_hash_add(q->qdisc, true);

	return cbs_change(sch, opt);
}

static void cbs_destroy(struct Qdisc *sch)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct tc_cbs_qopt_offload cbs = { };
	struct net_device *dev;
	int err;

	q->hicredit = 0;
	q->locredit = 0;
	q->idleslope = 0;
	q->sendslope = 0;

	dev = qdisc_dev(sch);

	cbs.queue = q->queue;
	cbs.enable = 0;

	err = dev->netdev_ops->ndo_setup_tc(dev, TC_SETUP_CBS, &cbs);
	if (err < 0)
		pr_warn("Couldn't reset queue %d to default values\n",
			cbs.queue);

	qdisc_destroy(q->qdisc);
}

static int cbs_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct cbs_sched_data *q = qdisc_priv(sch);
	struct nlattr *nest;
	struct tc_cbs_qopt opt;

	sch->qstats.backlog = q->qdisc->qstats.backlog;
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

static int cbs_dump_class(struct Qdisc *sch, unsigned long cl,
			  struct sk_buff *skb, struct tcmsg *tcm)
{
	struct cbs_sched_data *q = qdisc_priv(sch);

	tcm->tcm_handle |= TC_H_MIN(1);
	tcm->tcm_info = q->qdisc->handle;

	return 0;
}

static int cbs_graft(struct Qdisc *sch, unsigned long arg, struct Qdisc *new,
		     struct Qdisc **old)
{
	struct cbs_sched_data *q = qdisc_priv(sch);

	if (!new)
		new = &noop_qdisc;

	*old = qdisc_replace(sch, new, &q->qdisc);
	return 0;
}

static struct Qdisc *cbs_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct cbs_sched_data *q = qdisc_priv(sch);

	return q->qdisc;
}

static unsigned long cbs_find(struct Qdisc *sch, u32 classid)
{
	return 1;
}

static int cbs_delete(struct Qdisc *sch, unsigned long arg)
{
	return 0;
}

static void cbs_walk(struct Qdisc *sch, struct qdisc_walker *walker)
{
	if (!walker->stop) {
		if (walker->count >= walker->skip)
			if (walker->fn(sch, 1, walker) < 0) {
				walker->stop = 1;
				return;
			}
		walker->count++;
	}
}

static const struct Qdisc_class_ops cbs_class_ops = {
	.graft		=	cbs_graft,
	.leaf		=	cbs_leaf,
	.find		=	cbs_find,
	.delete		=	cbs_delete,
	.walk		=	cbs_walk,
	.dump		=	cbs_dump_class,
};

static struct Qdisc_ops cbs_qdisc_ops __read_mostly = {
	.next		=	NULL,
	.cl_ops		=	&cbs_class_ops,
	.id		=	"cbs",
	.priv_size	=	sizeof(struct cbs_sched_data),
	.enqueue	=	cbs_enqueue,
	.dequeue	=	cbs_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	cbs_init,
	.reset		=	cbs_reset,
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
