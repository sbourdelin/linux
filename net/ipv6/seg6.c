/*
 *  SR-IPv6 implementation
 *
 *  Author:
 *  David Lebrun <david.lebrun@uclouvain.be>
 *
 *
 *  This program is free software; you can redistribute it and/or
 *	  modify it under the terms of the GNU General Public License
 *	  as published by the Free Software Foundation; either version
 *	  2 of the License, or (at your option) any later version.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <linux/slab.h>

#include <net/ipv6.h>
#include <net/protocol.h>

#include <net/seg6.h>
#include <net/genetlink.h>
#include <linux/seg6.h>
#include <linux/seg6_genl.h>
#ifdef CONFIG_IPV6_SEG6_HMAC
#include <net/seg6_hmac.h>
#endif

static const struct nla_policy seg6_genl_policy[SEG6_ATTR_MAX + 1] = {
	[SEG6_ATTR_DST]				= { .type = NLA_BINARY,
		.len = sizeof(struct in6_addr) },
	[SEG6_ATTR_DSTLEN]			= { .type = NLA_S32, },
	[SEG6_ATTR_HMACKEYID]		= { .type = NLA_U32, },
	[SEG6_ATTR_SECRET]			= { .type = NLA_BINARY, },
	[SEG6_ATTR_SECRETLEN]		= { .type = NLA_U8, },
	[SEG6_ATTR_ALGID]			= { .type = NLA_U8, },
	[SEG6_ATTR_HMACINFO]		= { .type = NLA_NESTED, },
};

static struct genl_family seg6_genl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = 0,
	.name = SEG6_GENL_NAME,
	.version = SEG6_GENL_VERSION,
	.maxattr = SEG6_ATTR_MAX,
	.netnsok = true,
};

#ifdef CONFIG_IPV6_SEG6_HMAC

static int seg6_genl_sethmac(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct seg6_pernet_data *sdata;
	struct seg6_hmac_info *hinfo;
	u32 hmackeyid;
	char *secret;
	int err = 0;
	u8 algid;
	u8 slen;

	sdata = seg6_pernet(net);

	if (!info->attrs[SEG6_ATTR_HMACKEYID] ||
	    !info->attrs[SEG6_ATTR_SECRETLEN] ||
	    !info->attrs[SEG6_ATTR_ALGID])
		return -EINVAL;

	hmackeyid = nla_get_u32(info->attrs[SEG6_ATTR_HMACKEYID]);
	slen = nla_get_u8(info->attrs[SEG6_ATTR_SECRETLEN]);
	algid = nla_get_u8(info->attrs[SEG6_ATTR_ALGID]);

	if (hmackeyid == 0)
		return -EINVAL;

	if (slen > SEG6_HMAC_SECRET_LEN)
		return -EINVAL;

	mutex_lock(&sdata->lock);
	hinfo = seg6_hmac_info_lookup(net, hmackeyid);

	if (!slen) {
		if (!hinfo || seg6_hmac_info_del(net, hmackeyid, hinfo))
			err = -ENOENT;
		else
			kfree(hinfo);

		goto out_unlock;
	}

	if (!info->attrs[SEG6_ATTR_SECRET]) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (hinfo) {
		if (seg6_hmac_info_del(net, hmackeyid, hinfo)) {
			err = -ENOENT;
			goto out_unlock;
		}
		kfree(hinfo);
	}

	secret = (char *)nla_data(info->attrs[SEG6_ATTR_SECRET]);

	hinfo = kzalloc(sizeof(*hinfo), GFP_KERNEL);
	if (!hinfo) {
		err = -ENOMEM;
		goto out_unlock;
	}

	memcpy(hinfo->secret, secret, slen);
	hinfo->slen = slen;
	hinfo->alg_id = algid;
	hinfo->hmackeyid = hmackeyid;

	seg6_hmac_info_add(net, hmackeyid, hinfo);

out_unlock:
	mutex_unlock(&sdata->lock);
	return err;
}

#else

static int seg6_genl_sethmac(struct sk_buff *skb, struct genl_info *info)
{
	return -ENOTSUPP;
}

#endif

static int seg6_genl_set_tunsrc(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct in6_addr *val, *t_old, *t_new;
	struct seg6_pernet_data *sdata;

	sdata = seg6_pernet(net);

	if (!info->attrs[SEG6_ATTR_DST])
		return -EINVAL;

	val = nla_data(info->attrs[SEG6_ATTR_DST]);
	t_new = kmemdup(val, sizeof(*val), GFP_KERNEL);

	mutex_lock(&sdata->lock);

	t_old = sdata->tun_src;
	rcu_assign_pointer(sdata->tun_src, t_new);

	mutex_unlock(&sdata->lock);

	synchronize_net();
	kfree(t_old);

	return 0;
}

static int seg6_genl_get_tunsrc(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct in6_addr *tun_src;
	struct sk_buff *msg;
	void *hdr;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &seg6_genl_family, 0, SEG6_CMD_GET_TUNSRC);
	if (!hdr)
		goto free_msg;

	rcu_read_lock();
	tun_src = rcu_dereference(seg6_pernet(net)->tun_src);

	if (nla_put(msg, SEG6_ATTR_DST, sizeof(struct in6_addr), tun_src))
		goto nla_put_failure;

	rcu_read_unlock();

	genlmsg_end(msg, hdr);
	genlmsg_reply(msg, info);

	return 0;

nla_put_failure:
	rcu_read_unlock();
	genlmsg_cancel(msg, hdr);
free_msg:
	nlmsg_free(msg);
	return -ENOMEM;
}

#ifdef CONFIG_IPV6_SEG6_HMAC

static int __seg6_hmac_fill_info(struct seg6_hmac_info *hinfo,
				 struct sk_buff *msg)
{
	if (nla_put_u32(msg, SEG6_ATTR_HMACKEYID, hinfo->hmackeyid) ||
	    nla_put_u8(msg, SEG6_ATTR_SECRETLEN, hinfo->slen) ||
	    nla_put(msg, SEG6_ATTR_SECRET, hinfo->slen, hinfo->secret) ||
	    nla_put_u8(msg, SEG6_ATTR_ALGID, hinfo->alg_id))
		return -1;

	return 0;
}

static int __seg6_genl_dumphmac_element(struct seg6_hmac_info *hinfo,
					u32 portid, u32 seq, u32 flags,
					struct sk_buff *skb, u8 cmd)
{
	void *hdr;

	hdr = genlmsg_put(skb, portid, seq, &seg6_genl_family, flags, cmd);
	if (!hdr)
		return -ENOMEM;

	if (__seg6_hmac_fill_info(hinfo, skb) < 0)
		goto nla_put_failure;

	genlmsg_end(skb, hdr);
	return 0;

nla_put_failure:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

static int seg6_genl_dumphmac(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct seg6_pernet_data *sdata;
	struct seg6_hmac_info *hinfo;
	int idx = 0, ret;

	sdata = seg6_pernet(net);

	rcu_read_lock();
	list_for_each_entry_rcu(hinfo, &sdata->hmac_infos, list) {
		if (idx++ < cb->args[0])
			continue;

		ret = __seg6_genl_dumphmac_element(hinfo,
						   NETLINK_CB(cb->skb).portid,
						   cb->nlh->nlmsg_seq,
						   NLM_F_MULTI,
						   skb, SEG6_CMD_DUMPHMAC);
		if (ret)
			break;
	}
	rcu_read_unlock();

	cb->args[0] = idx;
	return skb->len;
}

#else

static int seg6_genl_dumphmac(struct sk_buff *skb, struct netlink_callback *cb)
{
	return -ENOTSUPP;
}

#endif

static const struct genl_ops seg6_genl_ops[] = {
	{
		.cmd	= SEG6_CMD_SETHMAC,
		.doit	= seg6_genl_sethmac,
		.policy	= seg6_genl_policy,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= SEG6_CMD_DUMPHMAC,
		.dumpit	= seg6_genl_dumphmac,
		.policy	= seg6_genl_policy,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= SEG6_CMD_SET_TUNSRC,
		.doit	= seg6_genl_set_tunsrc,
		.policy	= seg6_genl_policy,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= SEG6_CMD_GET_TUNSRC,
		.doit	= seg6_genl_get_tunsrc,
		.policy = seg6_genl_policy,
		.flags	= GENL_ADMIN_PERM,
	},
};

static int __net_init seg6_net_init(struct net *net)
{
	struct seg6_pernet_data *sdata;

	sdata = kzalloc(sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	mutex_init(&sdata->lock);

	sdata->tun_src = kzalloc(sizeof(*sdata->tun_src), GFP_KERNEL);
	if (!sdata->tun_src) {
		kfree(sdata);
		return -ENOMEM;
	}

	net->ipv6.seg6_data = sdata;

#ifdef CONFIG_IPV6_SEG6_HMAC
	seg6_hmac_net_init(net);
#endif

	return 0;
}

static void __net_exit seg6_net_exit(struct net *net)
{
	struct seg6_pernet_data *sdata = seg6_pernet(net);

#ifdef CONFIG_IPV6_SEG6_HMAC
	seg6_hmac_net_exit(net);
#endif

	kfree(sdata->tun_src);
	kfree(sdata);
}

static struct pernet_operations ip6_segments_ops = {
	.init = seg6_net_init,
	.exit = seg6_net_exit,
};

int __init seg6_init(void)
{
	int err = -ENOMEM;

	err = genl_register_family_with_ops(&seg6_genl_family, seg6_genl_ops);
	if (err)
		goto out;

	err = register_pernet_subsys(&ip6_segments_ops);
	if (err)
		goto out_unregister_genl;

	err = seg6_iptunnel_init();
	if (err)
		goto out_unregister_pernet;

#ifdef CONFIG_IPV6_SEG6_HMAC
	err = seg6_hmac_init();
	if (err)
		goto out_unregister_iptun;
#endif

	pr_info("Segment Routing with IPv6\n");

out:
	return err;
#ifdef CONFIG_IPV6_SEG6_HMAC
out_unregister_iptun:
	seg6_iptunnel_exit();
#endif
out_unregister_pernet:
	unregister_pernet_subsys(&ip6_segments_ops);
out_unregister_genl:
	genl_unregister_family(&seg6_genl_family);
	goto out;
}

void seg6_exit(void)
{
#ifdef CONFIG_IPV6_SEG6_HMAC
	seg6_hmac_exit();
#endif
	seg6_iptunnel_exit();
	unregister_pernet_subsys(&ip6_segments_ops);
	genl_unregister_family(&seg6_genl_family);
}
