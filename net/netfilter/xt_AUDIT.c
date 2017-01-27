/*
 * Creates audit record for dropped/accepted packets
 *
 * (C) 2010-2011 Thomas Graf <tgraf@redhat.com>
 * (C) 2010-2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/audit.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_arp.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_AUDIT.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <net/ipv6.h>
#include <net/ip.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomas Graf <tgraf@redhat.com>");
MODULE_DESCRIPTION("Xtables: creates audit records for dropped/accepted packets");
MODULE_ALIAS("ipt_AUDIT");
MODULE_ALIAS("ip6t_AUDIT");
MODULE_ALIAS("ebt_AUDIT");
MODULE_ALIAS("arpt_AUDIT");

struct nfpkt_par {
	int ipv;
	int iptrunc;
	const void *saddr;
	const void *daddr;
	u16 ipid;
	u8 proto;
	u8 frag;
	int ptrunc;
	u16 sport;
	u16 dport;
	u8 icmpt;
	u8 icmpc;
};

static void audit_proto(struct audit_buffer *ab, struct sk_buff *skb,
			unsigned int proto, unsigned int offset, struct nfpkt_par *apar)
{
	switch (proto) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
	case IPPROTO_DCCP:
	case IPPROTO_SCTP: {
		const __be16 *pptr;
		__be16 _ports[2];

		pptr = skb_header_pointer(skb, offset, sizeof(_ports), _ports);
		if (pptr == NULL) {
			apar->ptrunc = 1;
			return;
		}
		apar->sport = ntohs(pptr[0]);
		apar->dport = ntohs(pptr[1]);

		}
		break;

	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6: {
		const u8 *iptr;
		u8 _ih[2];

		iptr = skb_header_pointer(skb, offset, sizeof(_ih), &_ih);
		if (iptr == NULL) {
			apar->ptrunc = 1;
			return;
		}
		apar->icmpt = iptr[0];
		apar->icmpc = iptr[1];

		}
		break;
	}
}

static void audit_ip4(struct audit_buffer *ab, struct sk_buff *skb, struct nfpkt_par *apar)
{
	struct iphdr _iph;
	const struct iphdr *ih;

	apar->ipv = 4;
	ih = skb_header_pointer(skb, 0, sizeof(_iph), &_iph);
	if (!ih) {
		apar->iptrunc = 1;
		return;
	}

	apar->saddr = &ih->saddr;
	apar->daddr = &ih->daddr;
	apar->ipid = ntohs(ih->id);
	apar->proto = ih->protocol;

	if (ntohs(ih->frag_off) & IP_OFFSET) {
		apar->frag = 1;
		return;
	}

	audit_proto(ab, skb, ih->protocol, ih->ihl * 4, apar);
}

static void audit_ip6(struct audit_buffer *ab, struct sk_buff *skb, struct nfpkt_par *apar)
{
	struct ipv6hdr _ip6h;
	const struct ipv6hdr *ih;
	u8 nexthdr;
	__be16 frag_off;
	int offset;

	apar->ipv = 6;
	ih = skb_header_pointer(skb, skb_network_offset(skb), sizeof(_ip6h), &_ip6h);
	if (!ih) {
		apar->iptrunc = 1;
		return;
	}

	nexthdr = ih->nexthdr;
	offset = ipv6_skip_exthdr(skb, skb_network_offset(skb) + sizeof(_ip6h),
				  &nexthdr, &frag_off);

	apar->saddr = &ih->saddr;
	apar->daddr = &ih->daddr;
	apar->proto = nexthdr;

	if (offset)
		audit_proto(ab, skb, nexthdr, offset, apar);
}

static unsigned int
audit_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_audit_info *info = par->targinfo;
	struct audit_buffer *ab;
	struct nfpkt_par apar = {
		-1, -1, NULL, NULL, -1, -1, -1, -1, -1, -1, -1, -1
	};

	if (audit_enabled == 0)
		goto errout;

	ab = audit_log_start(NULL, GFP_ATOMIC, AUDIT_NETFILTER_PKT);
	if (ab == NULL)
		goto errout;

	audit_log_format(ab, "action=%hhu hook=%u len=%u inif=%s outif=%s",
			 info->type, par->hooknum, skb->len,
			 par->in ? par->in->name : "?",
			 par->out ? par->out->name : "?");

	audit_log_format(ab, " mark=%#x", skb->mark ?: -1);

	if (skb->dev && skb->dev->type == ARPHRD_ETHER) {
		audit_log_format(ab, " smac=%pM dmac=%pM macproto=0x%04x",
				 eth_hdr(skb)->h_source, eth_hdr(skb)->h_dest,
				 ntohs(eth_hdr(skb)->h_proto));

		if (par->family == NFPROTO_BRIDGE) {
			switch (eth_hdr(skb)->h_proto) {
			case htons(ETH_P_IP):
				audit_ip4(ab, skb, &apar);
				break;

			case htons(ETH_P_IPV6):
				audit_ip6(ab, skb, &apar);
				break;
			}
		}
	} else {
		audit_log_format(ab, " smac=? dmac=? macproto=0xffff");
	}

	switch (par->family) {
	case NFPROTO_IPV4:
		audit_ip4(ab, skb, &apar);
		break;

	case NFPROTO_IPV6:
		audit_ip6(ab, skb, &apar);
		break;
	}

	switch (apar.ipv) {
	case 4:
		audit_log_format(ab, " trunc=%d saddr=%pI4 daddr=%pI4 ipid=%hu proto=%hhu frag=%d",
			 apar.iptrunc, apar.saddr, apar.daddr, apar.ipid, apar.proto, apar.frag);
		break;
	case 6:
		audit_log_format(ab, " trunc=%d saddr=%pI6c daddr=%pI6c ipid=-1 proto=%hhu frag=-1",
			 apar.iptrunc, apar.saddr, apar.daddr, apar.proto);
		break;
	default:
		audit_log_format(ab, " trunc=-1 saddr=? daddr=? ipid=-1 proto=-1 frag=-1");
	}
	audit_log_format(ab, " trunc=%d sport=%hu dport=%hu icmptype=%hhu icmpcode=%hhu",
		apar.ptrunc, apar.sport, apar.dport, apar.icmpt, apar.icmpc);

#ifdef CONFIG_NETWORK_SECMARK
	if (skb->secmark)
		audit_log_secctx(ab, skb->secmark);
#endif

	audit_log_end(ab);

errout:
	return XT_CONTINUE;
}

static unsigned int
audit_tg_ebt(struct sk_buff *skb, const struct xt_action_param *par)
{
	audit_tg(skb, par);
	return EBT_CONTINUE;
}

static int audit_tg_check(const struct xt_tgchk_param *par)
{
	const struct xt_audit_info *info = par->targinfo;

	if (info->type > XT_AUDIT_TYPE_MAX) {
		pr_info("Audit type out of range (valid range: 0..%hhu)\n",
			XT_AUDIT_TYPE_MAX);
		return -ERANGE;
	}

	return 0;
}

static struct xt_target audit_tg_reg[] __read_mostly = {
	{
		.name		= "AUDIT",
		.family		= NFPROTO_UNSPEC,
		.target		= audit_tg,
		.targetsize	= sizeof(struct xt_audit_info),
		.checkentry	= audit_tg_check,
		.me		= THIS_MODULE,
	},
	{
		.name		= "AUDIT",
		.family		= NFPROTO_BRIDGE,
		.target		= audit_tg_ebt,
		.targetsize	= sizeof(struct xt_audit_info),
		.checkentry	= audit_tg_check,
		.me		= THIS_MODULE,
	},
};

static int __init audit_tg_init(void)
{
	return xt_register_targets(audit_tg_reg, ARRAY_SIZE(audit_tg_reg));
}

static void __exit audit_tg_exit(void)
{
	xt_unregister_targets(audit_tg_reg, ARRAY_SIZE(audit_tg_reg));
}

module_init(audit_tg_init);
module_exit(audit_tg_exit);
