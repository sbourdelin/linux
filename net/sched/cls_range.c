// SPDX-License-Identifier: GPL-2.0
/* net/sched/cls_range.c		Range classifier
 *
 * Copyright (c) 2018 Amritha Nambiar <amritha.nambiar@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include <net/pkt_cls.h>
#include <net/flow_dissector.h>

struct range_flow_key {
	int	indev_ifindex;
	struct flow_dissector_key_control control;
	struct flow_dissector_key_basic basic;
	struct flow_dissector_key_ports tp;

	/* Additional range fields should be added last */
	struct flow_dissector_key_ports tp_min;
	struct flow_dissector_key_ports tp_max;
} __aligned(BITS_PER_LONG / 8); /* Ensure that we can do comparisons as longs */

struct range_flow_mask {
	struct list_head filters; /* list of filters having this mask */
	struct list_head list; /* masks list */
	struct range_flow_key key;
	struct flow_dissector dissector;
};

struct cls_range_head {
	struct list_head filters;
	struct list_head masks;
	struct rcu_work rwork;
	struct idr handle_idr;
};

struct cls_range_filter {
	struct range_flow_mask *mask;
	struct range_flow_key key;
	struct range_flow_key mkey;
	struct list_head flist; /* filters list in head */
	struct list_head list; /* filters list in mask */
	struct tcf_exts exts;
	struct tcf_result res;
	u32 handle;
	u32 flags;
	struct rcu_work rwork;
};

struct range_params {
	__be16 min_mask;
	__be16 max_mask;
	__be16 min_val;
	__be16 max_val;
};

enum range_port {
	RANGE_PORT_DST,
	RANGE_PORT_SRC
};

static void range_set_masked_key(struct range_flow_key *key,
				 struct range_flow_mask *mask,
				 struct range_flow_key *mkey)
{
	unsigned char *ckey, *cmask, *cmkey;
	int i;

	ckey = (unsigned char *)key;
	cmask = (unsigned char *)&mask->key;
	cmkey = (unsigned char *)mkey;

	for (i = 0; i < sizeof(struct range_flow_key);
	     i += sizeof(unsigned char))
		*cmkey++ = *ckey++ & *cmask++;
}

static int range_compare_params(struct range_params *range,
				struct cls_range_filter *filter,
				struct range_flow_key *key,
				enum range_port port)
{
	if (port == RANGE_PORT_DST) {
		range->min_mask = htons(filter->mask->key.tp_min.dst);
		range->max_mask = htons(filter->mask->key.tp_max.dst);
		range->min_val = htons(filter->key.tp_min.dst);
		range->max_val = htons(filter->key.tp_max.dst);

		if (range->min_mask && range->max_mask) {
			if (htons(key->tp.dst) < range->min_val ||
			    htons(key->tp.dst) > range->max_val)
				return -1;
		}
	} else {
		range->min_mask = htons(filter->mask->key.tp_min.src);
		range->max_mask = htons(filter->mask->key.tp_max.src);
		range->min_val = htons(filter->key.tp_min.src);
		range->max_val = htons(filter->key.tp_max.src);

		if (range->min_mask && range->max_mask) {
			if (htons(key->tp.src) < range->min_val ||
			    htons(key->tp.src) > range->max_val)
				return -1;
		}
	}
	return 0;
}

#define RANGE_KEY_MEMBER_OFFSET(member) offsetof(struct range_flow_key, member)

static struct cls_range_filter *range_lookup(struct cls_range_head *head,
					     struct range_flow_key *key,
					     struct range_flow_key *mkey,
					     bool is_skb)
{
	struct cls_range_filter *filter, *next_filter;
	struct range_params range;
	int ret;
	size_t cmp_size;

	list_for_each_entry_safe(filter, next_filter, &head->filters, flist) {
		if (!is_skb) {
			/* Existing filter comparison */
			cmp_size = sizeof(filter->mkey);
		} else {
			/* skb classification */
			ret = range_compare_params(&range, filter, key,
						   RANGE_PORT_DST);
			if (ret < 0)
				continue;

			ret = range_compare_params(&range, filter, key,
						   RANGE_PORT_SRC);
			if (ret < 0)
				continue;

			/* skb does not have min and max values */
			cmp_size = RANGE_KEY_MEMBER_OFFSET(tp_min);
		}
		if (!memcmp(mkey, &filter->mkey, cmp_size))
			return filter;
	}
	return NULL;
}

static int range_classify(struct sk_buff *skb, const struct tcf_proto *tp,
			  struct tcf_result *res)
{
	struct cls_range_head *head = rcu_dereference_bh(tp->root);
	struct cls_range_filter *f;
	struct range_flow_mask *mask;
	struct range_flow_key skb_key, skb_mkey;

	list_for_each_entry_rcu(mask, &head->masks, list) {
		skb_key.indev_ifindex = skb->skb_iif;
		skb_key.basic.n_proto = skb->protocol;
		skb_flow_dissect(skb, &mask->dissector, &skb_key, 0);

		range_set_masked_key(&skb_key, mask, &skb_mkey);
		f = range_lookup(head, &skb_key, &skb_mkey, true);
		if (f && !tc_skip_sw(f->flags)) {
			*res = f->res;
			return tcf_exts_exec(skb, &f->exts, res);
		}
	}
	return -1;
}

static int range_init(struct tcf_proto *tp)
{
	struct cls_range_head *head;

	head = kzalloc(sizeof(*head), GFP_KERNEL);
	if (!head)
		return -ENOBUFS;

	INIT_LIST_HEAD_RCU(&head->masks);
	rcu_assign_pointer(tp->root, head);
	idr_init(&head->handle_idr);
	INIT_LIST_HEAD_RCU(&head->filters);

	return 0;
}

static void range_mask_free(struct range_flow_mask *mask)
{
	if (!list_empty(&mask->filters))
		return;

	list_del_rcu(&mask->list);
	kfree(mask);
}

static void __range_destroy_filter(struct cls_range_filter *f)
{
	tcf_exts_destroy(&f->exts);
	tcf_exts_put_net(&f->exts);
	kfree(f);
}

static void range_destroy_filter_work(struct work_struct *work)
{
	struct cls_range_filter *f = container_of(to_rcu_work(work),
						struct cls_range_filter, rwork);

	rtnl_lock();
	__range_destroy_filter(f);
	rtnl_unlock();
}

static void __range_delete(struct tcf_proto *tp, struct cls_range_filter *f,
			   struct netlink_ext_ack *extack)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);

	idr_remove(&head->handle_idr, f->handle);
	list_del_rcu(&f->list);
	range_mask_free(f->mask);
	tcf_unbind_filter(tp, &f->res);
	if (tcf_exts_get_net(&f->exts))
		tcf_queue_work(&f->rwork, range_destroy_filter_work);
	else
		__range_destroy_filter(f);
}

static int range_list_remove(struct cls_range_head *head,
			     struct cls_range_filter *filter)
{
	if (!range_lookup(head, &filter->key, &filter->mkey, false))
		return -EINVAL;

	list_del_rcu(&filter->flist);
	return 0;
}

static int range_delete(struct tcf_proto *tp, void *arg, bool *last,
			struct netlink_ext_ack *extack)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);
	struct cls_range_filter *f = arg;

	if (!tc_skip_sw(f->flags))
		range_list_remove(head, f);

	__range_delete(tp, f, extack);
	*last = list_empty(&head->masks);
	return 0;
}

static void range_destroy(struct tcf_proto *tp, struct netlink_ext_ack *extack)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);
	struct range_flow_mask *mask, *next_mask;
	struct cls_range_filter *f, *next;

	list_for_each_entry_safe(mask, next_mask, &head->masks, list) {
		list_for_each_entry_safe(f, next, &mask->filters, list) {
			if (!tc_skip_sw(f->flags))
				range_list_remove(head, f);
			__range_delete(tp, f, extack);
		}
	}
	idr_destroy(&head->handle_idr);

	kfree(head);
}

static void *range_get(struct tcf_proto *tp, u32 handle)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);

	return idr_find(&head->handle_idr, handle);
}

static const struct nla_policy range_policy[TCA_RANGE_MAX + 1] = {
	[TCA_RANGE_UNSPEC]		= { .type = NLA_UNSPEC },
	[TCA_RANGE_CLASSID]		= { .type = NLA_U32 },
	[TCA_RANGE_INDEV]		= { .type = NLA_STRING,
					    .len = IFNAMSIZ },
	[TCA_RANGE_KEY_ETH_TYPE]	= { .type = NLA_U16 },
	[TCA_RANGE_KEY_IP_PROTO]	= { .type = NLA_U8 },
	[TCA_RANGE_KEY_PORT_SRC_MIN]	= { .type = NLA_U16 },
	[TCA_RANGE_KEY_PORT_SRC_MAX]	= { .type = NLA_U16 },
	[TCA_RANGE_KEY_PORT_DST_MIN]	= { .type = NLA_U16 },
	[TCA_RANGE_KEY_PORT_DST_MAX]	= { .type = NLA_U16 },
	[TCA_RANGE_FLAGS]		= { .type = NLA_U32 },
};

static void range_set_key_val(struct nlattr **tb, void *val, int val_type,
			      void *mask, int mask_type, int len)
{
	if (!tb[val_type])
		return;
	memcpy(val, nla_data(tb[val_type]), len);
	if (mask_type == TCA_RANGE_UNSPEC || !tb[mask_type])
		memset(mask, 0xff, len);
	else
		memcpy(mask, nla_data(tb[mask_type]), len);
}

static int range_set_key(struct net *net, struct nlattr **tb,
			 struct cls_range_filter *f,
			 struct range_flow_mask *f_mask,
			 struct netlink_ext_ack *extack)
{
	__be16 ethertype;
	struct range_flow_key *key = &f->key;
	struct range_flow_key *mask = &f_mask->key;

#ifdef CONFIG_NET_CLS_IND
	if (tb[TCA_RANGE_INDEV]) {
		int err = tcf_change_indev(net, tb[TCA_RANGE_INDEV], extack);

		if (err < 0)
			return err;
		key->indev_ifindex = err;
		mask->indev_ifindex = 0xffffffff;
	}
#endif
	if (tb[TCA_RANGE_KEY_ETH_TYPE]) {
		ethertype = nla_get_be16(tb[TCA_RANGE_KEY_ETH_TYPE]);

		if (!eth_type_vlan(ethertype)) {
			key->basic.n_proto = ethertype;
			mask->basic.n_proto = cpu_to_be16(~0);
		}
	}

	if (key->basic.n_proto != htons(ETH_P_IP) &&
	    key->basic.n_proto != htons(ETH_P_IPV6))
		return -EINVAL;

	range_set_key_val(tb, &key->basic.ip_proto,
			  TCA_RANGE_KEY_IP_PROTO, &mask->basic.ip_proto,
			  TCA_RANGE_UNSPEC,
			  sizeof(key->basic.ip_proto));

	if (key->basic.ip_proto != IPPROTO_TCP &&
	    key->basic.ip_proto != IPPROTO_UDP &&
	    key->basic.ip_proto != IPPROTO_SCTP)
		return -EINVAL;

	range_set_key_val(tb, &key->tp_min.dst,
			  TCA_RANGE_KEY_PORT_DST_MIN, &mask->tp_min.dst,
			  TCA_RANGE_UNSPEC, sizeof(key->tp_min.dst));
	range_set_key_val(tb, &key->tp_max.dst,
			  TCA_RANGE_KEY_PORT_DST_MAX, &mask->tp_max.dst,
			  TCA_RANGE_UNSPEC, sizeof(key->tp_max.dst));
	range_set_key_val(tb, &key->tp_min.src,
			  TCA_RANGE_KEY_PORT_SRC_MIN, &mask->tp_min.src,
			  TCA_RANGE_UNSPEC, sizeof(key->tp_min.src));
	range_set_key_val(tb, &key->tp_max.src,
			  TCA_RANGE_KEY_PORT_SRC_MAX, &mask->tp_max.src,
			  TCA_RANGE_UNSPEC, sizeof(key->tp_max.src));
	return 0;
}

#define RANGE_KEY_SET(keys, cnt, id, member)				\
	do {								\
		keys[cnt].key_id = id;					\
		keys[cnt].offset = RANGE_KEY_MEMBER_OFFSET(member);	\
		cnt++;							\
	} while (0)

static void range_init_dissector(struct flow_dissector *dissector)
{
	struct flow_dissector_key keys[FLOW_DISSECTOR_KEY_MAX];
	size_t cnt = 0;

	RANGE_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_CONTROL, control);
	RANGE_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_BASIC, basic);
	RANGE_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_PORTS, tp);

	skb_flow_dissector_init(dissector, keys, cnt);
}

static int range_check_assign_mask(struct cls_range_head *head,
				   struct cls_range_filter *fnew,
				   struct cls_range_filter *fold,
				   struct range_flow_mask *mask)
{
	struct range_flow_mask *imask, *next_mask;
	struct range_flow_mask *newmask;

	list_for_each_entry_safe(imask, next_mask, &head->masks, list) {
		if (!memcmp(imask, mask, sizeof(struct range_flow_mask))) {
			/* mask exists */
			fnew->mask = imask;
			break;
		}
	}
	if (!fnew->mask) {
		if (fold)
			return -EINVAL;
		newmask = kzalloc(sizeof(*newmask), GFP_KERNEL);
		if (!newmask)
			return -ENOMEM;
		memcpy(newmask, mask, sizeof(struct range_flow_mask));

		range_init_dissector(&newmask->dissector);
		INIT_LIST_HEAD_RCU(&newmask->filters);
		list_add_tail_rcu(&newmask->list, &head->masks);
		fnew->mask = newmask;
	} else if (fold && fold->mask != fnew->mask) {
		return -EINVAL;
	}
	return 0;
}

static int range_set_parms(struct net *net, struct tcf_proto *tp,
			   struct cls_range_filter *f,
			   struct range_flow_mask *mask,
			   unsigned long base, struct nlattr **tb,
			   struct nlattr *est, bool ovr,
			   struct netlink_ext_ack *extack)
{
	int err;

	err = tcf_exts_validate(net, tp, tb, est, &f->exts, ovr, extack);
	if (err < 0)
		return err;

	if (tb[TCA_RANGE_CLASSID]) {
		f->res.classid = nla_get_u32(tb[TCA_RANGE_CLASSID]);
		tcf_bind_filter(tp, &f->res, base);
	}

	err = range_set_key(net, tb, f, mask, extack);
	if (err)
		return err;

	range_set_masked_key(&f->key, mask, &f->mkey);
	return 0;
}

static int range_change(struct net *net, struct sk_buff *in_skb,
			struct tcf_proto *tp, unsigned long base, u32 handle,
			struct nlattr **tca, void **arg, bool ovr,
			struct netlink_ext_ack *extack)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);
	struct cls_range_filter *fold = *arg;
	struct cls_range_filter *fnew;
	struct nlattr **tb;
	struct range_flow_mask mask = {};
	int err;

	if (!tca[TCA_OPTIONS])
		return -EINVAL;

	tb = kcalloc(TCA_RANGE_MAX + 1, sizeof(struct nlattr *), GFP_KERNEL);
	if (!tb)
		return -ENOBUFS;

	err = nla_parse_nested(tb, TCA_RANGE_MAX, tca[TCA_OPTIONS],
			       range_policy, NULL);

	if (err < 0)
		goto errout_tb;

	if (fold && handle && fold->handle != handle) {
		err = -EINVAL;
		goto errout_tb;
	}

	fnew = kzalloc(sizeof(*fnew), GFP_KERNEL);
	if (!fnew) {
		err = -ENOBUFS;
		goto errout_tb;
	}

	err = tcf_exts_init(&fnew->exts, TCA_RANGE_ACT, 0);
	if (err < 0)
		goto errout;

	if (!handle) {
		handle = 1;
		err = idr_alloc_u32(&head->handle_idr, fnew, &handle,
				    INT_MAX, GFP_KERNEL);
	} else if (!fold) {
		/* user specifies a handle and it doesn't exist */
		err = idr_alloc_u32(&head->handle_idr, fnew, &handle,
				    handle, GFP_KERNEL);
	}
	if (err)
		goto errout;
	fnew->handle = handle;

	if (tb[TCA_RANGE_FLAGS]) {
		fnew->flags = nla_get_u32(tb[TCA_RANGE_FLAGS]);

		if (!tc_flags_valid(fnew->flags)) {
			err = -EINVAL;
			goto errout_idr;
		}
	}

	/* Only SW rules are supported now */
	if (tc_skip_sw(fnew->flags)) {
		err = -EINVAL;
		goto errout_idr;
	}

	err = range_set_parms(net, tp, fnew, &mask, base, tb, tca[TCA_RATE],
			      ovr, extack);
	if (err)
		goto errout_idr;

	err = range_check_assign_mask(head, fnew, fold, &mask);
	if (err)
		goto errout_idr;

	/* Add the rule into list for SW filters */
	if (!fold && range_lookup(head, &fnew->key, &fnew->mkey, false)) {
		err = -EEXIST;
		goto errout_mask;
	}
	list_add_tail_rcu(&fnew->flist, &head->filters);

	if (!tc_in_hw(fnew->flags))
		fnew->flags |= TCA_CLS_FLAGS_NOT_IN_HW;

	*arg = fnew;

	if (fold) {
		range_list_remove(head, fold);

		idr_replace(&head->handle_idr, fnew, fnew->handle);
		list_replace_rcu(&fold->list, &fnew->list);
		tcf_unbind_filter(tp, &fold->res);
		tcf_exts_get_net(&fold->exts);
		tcf_queue_work(&fold->rwork, range_destroy_filter_work);
	} else {
		list_add_tail_rcu(&fnew->list, &fnew->mask->filters);
	}

	kfree(tb);
	return 0;

errout_mask:
	range_mask_free(fnew->mask);
errout_idr:
	if (!fold)
		idr_remove(&head->handle_idr, fnew->handle);
errout:
	tcf_exts_destroy(&fnew->exts);
	kfree(fnew);
errout_tb:
	kfree(tb);
	return err;
}

static void range_walk(struct tcf_proto *tp, struct tcf_walker *arg)
{
	struct cls_range_head *head = rtnl_dereference(tp->root);
	struct cls_range_filter *f;

	arg->count = arg->skip;

	while ((f = idr_get_next_ul(&head->handle_idr, &arg->cookie)) != NULL) {
		if (arg->fn(tp, f, arg) < 0) {
			arg->stop = 1;
			break;
		}
		arg->cookie = f->handle + 1;
		arg->count++;
	}
}

static int range_dump_key_val(struct sk_buff *skb, void *val, int val_type,
			      void *mask, int mask_type, int len)
{
	int err;

	if (!memchr_inv(mask, 0, len))
		return 0;
	err = nla_put(skb, val_type, len, val);

	if (err)
		return err;
	if (mask_type != TCA_RANGE_UNSPEC) {
		err = nla_put(skb, mask_type, len, mask);
		if (err)
			return err;
	}
	return 0;
}

static int range_dump_key(struct sk_buff *skb, struct net *net,
			  struct range_flow_key *key,
			  struct range_flow_key *mask)
{
	if (mask->indev_ifindex) {
		struct net_device *dev;

		dev = __dev_get_by_index(net, key->indev_ifindex);
		if (dev && nla_put_string(skb, TCA_RANGE_INDEV, dev->name))
			goto nla_put_failure;
	}

	if (range_dump_key_val(skb, &key->basic.n_proto, TCA_RANGE_KEY_ETH_TYPE,
			       &mask->basic.n_proto, TCA_RANGE_UNSPEC,
			       sizeof(key->basic.n_proto)))
		goto nla_put_failure;

	if ((key->basic.n_proto != htons(ETH_P_IP) &&
	     key->basic.n_proto != htons(ETH_P_IPV6)) ||
	    (key->basic.ip_proto != IPPROTO_TCP &&
	     key->basic.ip_proto != IPPROTO_UDP &&
	     key->basic.ip_proto != IPPROTO_SCTP))
		return -EINVAL;

	if (range_dump_key_val(skb, &key->basic.ip_proto,
			       TCA_RANGE_KEY_IP_PROTO, &mask->basic.ip_proto,
			       TCA_RANGE_UNSPEC,
			       sizeof(key->basic.ip_proto)))
		goto nla_put_failure;

	if (range_dump_key_val(skb, &key->tp_min.src,
			       TCA_RANGE_KEY_PORT_SRC_MIN, &mask->tp_min.src,
			       TCA_RANGE_UNSPEC, sizeof(key->tp_min.src)) ||
	    range_dump_key_val(skb, &key->tp_max.src,
			       TCA_RANGE_KEY_PORT_SRC_MAX, &mask->tp_max.src,
			       TCA_RANGE_UNSPEC, sizeof(key->tp_max.src)) ||
	    range_dump_key_val(skb, &key->tp_min.dst,
			       TCA_RANGE_KEY_PORT_DST_MIN, &mask->tp_min.dst,
			       TCA_RANGE_UNSPEC, sizeof(key->tp_min.dst)) ||
	    range_dump_key_val(skb, &key->tp_max.dst,
			       TCA_RANGE_KEY_PORT_DST_MAX, &mask->tp_max.dst,
			       TCA_RANGE_UNSPEC, sizeof(key->tp_max.dst)))
		goto nla_put_failure;

	return 0;
nla_put_failure:
	return -EMSGSIZE;
}

static int range_dump(struct net *net, struct tcf_proto *tp, void *fh,
		      struct sk_buff *skb, struct tcmsg *t)
{
	struct cls_range_filter *f = fh;
	struct nlattr *nest;
	struct range_flow_key *key, *mask;

	if (!f)
		return skb->len;

	t->tcm_handle = f->handle;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;

	if (f->res.classid &&
	    nla_put_u32(skb, TCA_RANGE_CLASSID, f->res.classid))
		goto nla_put_failure;

	key = &f->key;
	mask = &f->mask->key;

	if (range_dump_key(skb, net, key, mask))
		goto nla_put_failure;

	if (f->flags && nla_put_u32(skb, TCA_RANGE_FLAGS, f->flags))
		goto nla_put_failure;

	if (tcf_exts_dump(skb, &f->exts))
		goto nla_put_failure;

	nla_nest_end(skb, nest);

	if (tcf_exts_dump_stats(skb, &f->exts) < 0)
		goto nla_put_failure;

	return skb->len;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -1;
}

static void range_bind_class(void *fh, u32 classid, unsigned long cl)
{
	struct cls_range_filter *f = fh;

	if (f && f->res.classid == classid)
		f->res.class = cl;
}

static struct tcf_proto_ops cls_range_ops __read_mostly = {
	.kind		= "range",
	.classify	= range_classify,
	.init		= range_init,
	.destroy	= range_destroy,
	.get		= range_get,
	.change		= range_change,
	.delete		= range_delete,
	.walk		= range_walk,
	.dump		= range_dump,
	.bind_class	= range_bind_class,
	.owner		= THIS_MODULE,
};

static int __init cls_range_init(void)
{
	return register_tcf_proto_ops(&cls_range_ops);
}

static void __exit cls_range_exit(void)
{
	unregister_tcf_proto_ops(&cls_range_ops);
}

module_init(cls_range_init);
module_exit(cls_range_exit);

MODULE_AUTHOR("Amritha Nambiar <amritha.nambiar@intel.com>");
MODULE_DESCRIPTION("Range classifier");
MODULE_LICENSE("GPL");
