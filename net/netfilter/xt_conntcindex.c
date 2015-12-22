/*
 * xt_conntcindex - Netfilter module to operate on connection tc_index marks
 *
 * Copyright (C) 2015 Allied Telesis Labs NZ
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Heavily based on xt_connmark.c
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_ecache.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_conntcindex.h>

MODULE_AUTHOR("Luuk Paulussen <luuk.paulussen@alliedtelesis.co.nz>");
MODULE_DESCRIPTION("Xtables: connection tc_index mark operations");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_CONNTCINDEX");
MODULE_ALIAS("ip6t_CONNTCINDEX");
MODULE_ALIAS("ipt_conntcindex");
MODULE_ALIAS("ip6t_conntcindex");

static unsigned int
conntcindex_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_conntcindex_tginfo1 *info = par->targinfo;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	u32 newmark;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct)
		return XT_CONTINUE;

	switch (info->mode) {
	case XT_CONNTCINDEX_SET:
		newmark = (ct->tc_index & ~info->ctmask) ^ info->ctmark;
		if (ct->tc_index != newmark) {
			ct->tc_index = newmark;
			nf_conntrack_event_cache(IPCT_TCINDEX, ct);
		}
		break;
	case XT_CONNTCINDEX_SAVE:
		newmark = (ct->tc_index & ~info->ctmask) ^
			  (skb->tc_index & info->nfmask);
		if (ct->tc_index != newmark) {
			ct->tc_index = newmark;
			nf_conntrack_event_cache(IPCT_TCINDEX, ct);
		}
		break;
	case XT_CONNTCINDEX_RESTORE:
		newmark = (skb->tc_index & ~info->nfmask) ^
			  (ct->tc_index & info->ctmask);
		skb->tc_index = newmark;
		break;
	}

	return XT_CONTINUE;
}

static int conntcindex_tg_check(const struct xt_tgchk_param *par)
{
	int ret;

	ret = nf_ct_l3proto_try_module_get(par->family);
	if (ret < 0)
		pr_info("cannot load conntrack support for proto=%u\n",
			par->family);
	return ret;
}

static void conntcindex_tg_destroy(const struct xt_tgdtor_param *par)
{
	nf_ct_l3proto_module_put(par->family);
}

static bool
conntcindex_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_conntcindex_mtinfo1 *info = par->matchinfo;
	enum ip_conntrack_info ctinfo;
	const struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct)
		return false;

	return ((ct->tc_index & info->mask) == info->mark) ^ info->invert;
}

static int conntcindex_mt_check(const struct xt_mtchk_param *par)
{
	int ret;

	ret = nf_ct_l3proto_try_module_get(par->family);
	if (ret < 0)
		pr_info("cannot load conntrack support for proto=%u\n",
			par->family);
	return ret;
}

static void conntcindex_mt_destroy(const struct xt_mtdtor_param *par)
{
	nf_ct_l3proto_module_put(par->family);
}

static struct xt_target conntcindex_tg_reg __read_mostly = {
	.name           = "CONNTCINDEX",
	.revision       = 1,
	.family         = NFPROTO_UNSPEC,
	.checkentry     = conntcindex_tg_check,
	.target         = conntcindex_tg,
	.targetsize     = sizeof(struct xt_conntcindex_tginfo1),
	.destroy        = conntcindex_tg_destroy,
	.me             = THIS_MODULE,
};

static struct xt_match conntcindex_mt_reg __read_mostly = {
	.name           = "conntcindex",
	.revision       = 1,
	.family         = NFPROTO_UNSPEC,
	.checkentry     = conntcindex_mt_check,
	.match          = conntcindex_mt,
	.matchsize      = sizeof(struct xt_conntcindex_mtinfo1),
	.destroy        = conntcindex_mt_destroy,
	.me             = THIS_MODULE,
};

static int __init conntcindex_mt_init(void)
{
	int ret;

	ret = xt_register_target(&conntcindex_tg_reg);
	if (ret < 0)
		return ret;
	ret = xt_register_match(&conntcindex_mt_reg);
	if (ret < 0) {
		xt_unregister_target(&conntcindex_tg_reg);
		return ret;
	}
	return 0;
}

static void __exit conntcindex_mt_exit(void)
{
	xt_unregister_match(&conntcindex_mt_reg);
	xt_unregister_target(&conntcindex_tg_reg);
}

module_init(conntcindex_mt_init);
module_exit(conntcindex_mt_exit);
