/* x_tables module for Identifier Locator Addressing (ILA) translation
 *
 * (C) 2015 by Tom Herbert <tom@herbertland.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ila.h>

#include <linux/netfilter/x_tables.h>

MODULE_AUTHOR("Tom Herbert <tom@herbertland.com>");
MODULE_DESCRIPTION("Xtables: ILA translation");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ip6t_ILA");
MODULE_ALIAS("ip6t_ILAIN");
MODULE_ALIAS("ip6t_ILAOUT");

static unsigned int
ila_tg_input(struct sk_buff *skb, const struct xt_action_param *par)
{
	ila_xlat_incoming(skb);

	return XT_CONTINUE;
}

static unsigned int
ila_tg_output(struct sk_buff *skb, const struct xt_action_param *par)
{
	ila_xlat_outgoing(skb);

	return XT_CONTINUE;
}

static int ila_tg_check(const struct xt_tgchk_param *par)
{
	return 0;
}

static struct xt_target ila_tg_reg[] __read_mostly = {
	{
		.name		= "ILAIN",
		.family		= NFPROTO_IPV6,
		.checkentry	= ila_tg_check,
		.target		= ila_tg_input,
		.targetsize	= 0,
		.table		= "mangle",
		.hooks		= (1 << NF_INET_PRE_ROUTING) |
				  (1 << NF_INET_LOCAL_IN),
		.me		= THIS_MODULE,
	},
	{
		.name		= "ILAOUT",
		.family		= NFPROTO_IPV6,
		.checkentry	= ila_tg_check,
		.target		= ila_tg_output,
		.targetsize	= 0,
		.table		= "mangle",
		.hooks		= (1 << NF_INET_POST_ROUTING) |
				  (1 << NF_INET_LOCAL_OUT),
		.me		= THIS_MODULE,
	},
};

static int __init ila_tg_init(void)
{
	return xt_register_targets(ila_tg_reg, ARRAY_SIZE(ila_tg_reg));
}

static void __exit ila_tg_exit(void)
{
	xt_unregister_targets(ila_tg_reg, ARRAY_SIZE(ila_tg_reg));
}

module_init(ila_tg_init);
module_exit(ila_tg_exit);
