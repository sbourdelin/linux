/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/dst_metadata.h>
#include <net/ip_tunnels.h>

#include <linux/netfilter/xt_tunnel.h>
#include <linux/netfilter/x_tables.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wenxu <wenxu@ucloud.cn>");
MODULE_DESCRIPTION("Xtables: packet tunnel match");
MODULE_ALIAS("ipt_tunnel");
MODULE_ALIAS("ip6t_tunnel");

static bool
tunnel_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_tunnel_mtinfo *info = par->matchinfo;
	struct ip_tunnel_info *tun_info;
	u32 key;

	tun_info = skb_tunnel_info(skb);
	if (tun_info) {
		key = ntohl(tunnel_id_to_key32(tun_info->key.tun_id));
		return ((key & info->mask) == info->key) ^ info->invert;
	}

	return info->invert;
}

static struct xt_match tunnel_mt_reg __read_mostly = {
	.name           = "tunnel",
	.revision       = 0,
	.family         = NFPROTO_UNSPEC,
	.match          = tunnel_mt,
	.matchsize      = sizeof(struct xt_tunnel_mtinfo),
	.hooks          = ((1 << NF_INET_PRE_ROUTING) |
					  (1 << NF_INET_POST_ROUTING) |
					  (1 << NF_INET_LOCAL_OUT) |
					  (1 << NF_INET_FORWARD)),
	.me             = THIS_MODULE,
};

static int __init tunnel_mt_init(void)
{
	return xt_register_match(&tunnel_mt_reg);
}

static void __exit tunnel_mt_exit(void)
{
	xt_unregister_match(&tunnel_mt_reg);
}

module_init(tunnel_mt_init);
module_exit(tunnel_mt_exit);
