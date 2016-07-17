/*
 * copyright Jamal Hadi Salim (2016)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Jamal Hadi Salim <jhs@mojatatu.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>

#include <linux/tc_act/tc_skbmod.h>
#include <net/tc_act/tc_skbmod.h>

#define SKBMOD_TAB_MASK     15

static int skbmod_net_id;

static int tcf_skbmod_run(struct sk_buff *skb, const struct tc_action *a,
			  struct tcf_result *res)
{
	struct tcf_skbmod *d = a->priv;

	spin_lock(&d->tcf_lock);
	tcf_lastuse_update(&d->tcf_tm);
	bstats_update(&d->tcf_bstats, skb);

	if (d->flags & SKBMOD_F_DMAC)
		ether_addr_copy(eth_hdr(skb)->h_dest, d->eth_dst);
	if (d->flags & SKBMOD_F_SMAC)
		ether_addr_copy(eth_hdr(skb)->h_source, d->eth_src);
	if (d->flags & SKBMOD_F_ETYPE)
		eth_hdr(skb)->h_proto = d->eth_type;

	spin_unlock(&d->tcf_lock);
	return d->tcf_action;
}

static const struct nla_policy skbmod_policy[TCA_SKBMOD_MAX + 1] = {
	[TCA_SKBMOD_PARMS]		= { .len = sizeof(struct tc_skbmod) },
	[TCA_SKBMOD_DMAC]		= { .len = ETH_ALEN },
	[TCA_SKBMOD_SMAC]		= { .len = ETH_ALEN },
	[TCA_SKBMOD_ETYPE]		= { .type = NLA_U16 },
};

static int tcf_skbmod_init(struct net *net, struct nlattr *nla,
			    struct nlattr *est, struct tc_action *a,
			    int ovr, int bind)
{
	struct tc_action_net *tn = net_generic(net, skbmod_net_id);
	struct nlattr *tb[TCA_SKBMOD_MAX + 1];
	struct tc_skbmod *parm;
	struct tcf_skbmod *d;
	u32 flags = 0;
	u8 *daddr = NULL;
	u8 *saddr = NULL;
	u16 eth_type = 0;

	bool exists = false;
	int ret = 0, err;

	if (nla == NULL)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_SKBMOD_MAX, nla, skbmod_policy);
	if (err < 0)
		return err;

	if (tb[TCA_SKBMOD_PARMS] == NULL)
		return -EINVAL;

	/*Allow zero valued mac */
	if (tb[TCA_SKBMOD_DMAC]) {
		daddr = nla_data(tb[TCA_SKBMOD_DMAC]);
		flags |= SKBMOD_F_DMAC;
	}

	if (tb[TCA_SKBMOD_SMAC]) {
		saddr = nla_data(tb[TCA_SKBMOD_SMAC]);
		flags |= SKBMOD_F_SMAC;
	}

	if (tb[TCA_SKBMOD_ETYPE]) {
		eth_type = nla_get_u16(tb[TCA_SKBMOD_ETYPE]);
		flags |= SKBMOD_F_ETYPE;
	}

	if (!flags) {
		return -EINVAL;
	}

	parm = nla_data(tb[TCA_SKBMOD_PARMS]);

	exists = tcf_hash_check(tn, parm->index, a, bind);
	if (exists && bind)
		return 0;

	if (!exists) {
		ret = tcf_hash_create(tn, parm->index, est, a,
				      sizeof(*d), bind, false);
		if (ret)
			return ret;

		d = to_skbmod(a);
		ret = ACT_P_CREATED;
	} else {
		d = to_skbmod(a);
		tcf_hash_release(a, bind);
		if (!ovr)
			return -EEXIST;
	}

	spin_lock_bh(&d->tcf_lock);

	d->flags = flags;
	if (flags & SKBMOD_F_DMAC)
		ether_addr_copy(d->eth_dst, daddr);
	if (flags & SKBMOD_F_SMAC)
		ether_addr_copy(d->eth_src, saddr);
	if (flags & SKBMOD_F_ETYPE)
		d->eth_type = htons(eth_type);

	d->tcf_action = parm->action;

	spin_unlock_bh(&d->tcf_lock);

	if (ret == ACT_P_CREATED)
		tcf_hash_insert(tn, a);
	return ret;
}

static int tcf_skbmod_dump(struct sk_buff *skb, struct tc_action *a,
			    int bind, int ref)
{
	unsigned char *b = skb_tail_pointer(skb);
	struct tcf_skbmod *d = a->priv;
	struct tc_skbmod opt = {
		.index   = d->tcf_index,
		.refcnt  = d->tcf_refcnt - ref,
		.bindcnt = d->tcf_bindcnt - bind,
		.action  = d->tcf_action,
	};
	struct tcf_t t;

	if (nla_put(skb, TCA_SKBMOD_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;
	if ((d->flags & SKBMOD_F_DMAC) &&
	    nla_put(skb, TCA_SKBMOD_DMAC, ETH_ALEN, d->eth_dst))
		goto nla_put_failure;
	if ((d->flags & SKBMOD_F_SMAC) &&
	    nla_put(skb, TCA_SKBMOD_SMAC, ETH_ALEN, d->eth_src))
		goto nla_put_failure;
	if ((d->flags & SKBMOD_F_ETYPE) &&
	    nla_put_u16(skb, TCA_SKBMOD_ETYPE, ntohs(d->eth_type)))
		goto nla_put_failure;

	tcf_tm_dump(&t, &d->tcf_tm);
	if (nla_put_64bit(skb, TCA_SKBMOD_TM, sizeof(t), &t, TCA_SKBMOD_PAD))
		goto nla_put_failure;
	return skb->len;

nla_put_failure:
	nlmsg_trim(skb, b);
	return -1;
}

static int tcf_skbmod_walker(struct net *net, struct sk_buff *skb,
			      struct netlink_callback *cb, int type,
			      struct tc_action *a)
{
	struct tc_action_net *tn = net_generic(net, skbmod_net_id);

	return tcf_generic_walker(tn, skb, cb, type, a);
}

static int tcf_skbmod_search(struct net *net, struct tc_action *a, u32 index)
{
	struct tc_action_net *tn = net_generic(net, skbmod_net_id);

	return tcf_hash_search(tn, a, index);
}

static struct tc_action_ops act_skbmod_ops = {
	.kind		=	"skbmod",
	.type		=	TCA_ACT_SKBMOD,
	.owner		=	THIS_MODULE,
	.act		=	tcf_skbmod_run,
	.dump		=	tcf_skbmod_dump,
	.init		=	tcf_skbmod_init,
	.walk		=	tcf_skbmod_walker,
	.lookup		=	tcf_skbmod_search,
};

static __net_init int skbmod_init_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, skbmod_net_id);

	return tc_action_net_init(tn, &act_skbmod_ops, SKBMOD_TAB_MASK);
}

static void __net_exit skbmod_exit_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, skbmod_net_id);

	tc_action_net_exit(tn);
}

static struct pernet_operations skbmod_net_ops = {
	.init = skbmod_init_net,
	.exit = skbmod_exit_net,
	.id   = &skbmod_net_id,
	.size = sizeof(struct tc_action_net),
};

MODULE_AUTHOR("Jamal Hadi Salim, <jhs@mojatatu.com>");
MODULE_DESCRIPTION("SKB data mod-ing");
MODULE_LICENSE("GPL");

static int __init skbmod_init_module(void)
{
	return tcf_register_action(&act_skbmod_ops, &skbmod_net_ops);
}

static void __exit skbmod_cleanup_module(void)
{
	tcf_unregister_action(&act_skbmod_ops, &skbmod_net_ops);
}

module_init(skbmod_init_module);
module_exit(skbmod_cleanup_module);
