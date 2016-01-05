/*
 * net/tipc/netlink.h: Include file for TIPC netlink code
 *
 * Copyright (c) 2014, Ericsson AB
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _TIPC_NETLINK_H
#define _TIPC_NETLINK_H

#include <net/netlink.h>

extern struct genl_family tipc_genl_family;

static const struct nla_policy tipc_nl_policy[TIPC_NLA_MAX + 1] = {
	[TIPC_NLA_UNSPEC]		= { .type = NLA_UNSPEC, },
	[TIPC_NLA_BEARER]		= { .type = NLA_NESTED, },
	[TIPC_NLA_SOCK]			= { .type = NLA_NESTED, },
	[TIPC_NLA_PUBL]			= { .type = NLA_NESTED, },
	[TIPC_NLA_LINK]			= { .type = NLA_NESTED, },
	[TIPC_NLA_MEDIA]		= { .type = NLA_NESTED, },
	[TIPC_NLA_NODE]			= { .type = NLA_NESTED, },
	[TIPC_NLA_NET]			= { .type = NLA_NESTED, },
	[TIPC_NLA_NAME_TABLE]		= { .type = NLA_NESTED, }
};

static const struct nla_policy tipc_nl_sock_policy[TIPC_NLA_SOCK_MAX + 1] = {
	[TIPC_NLA_SOCK_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_SOCK_ADDR]		= { .type = NLA_U32 },
	[TIPC_NLA_SOCK_REF]		= { .type = NLA_U32 },
	[TIPC_NLA_SOCK_CON]		= { .type = NLA_NESTED },
	[TIPC_NLA_SOCK_HAS_PUBL]	= { .type = NLA_FLAG }
};

static const struct nla_policy tipc_nl_net_policy[TIPC_NLA_NET_MAX + 1] = {
	[TIPC_NLA_NET_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_NET_ID]		= { .type = NLA_U32 }
};

static const struct nla_policy tipc_nl_node_policy[TIPC_NLA_NODE_MAX + 1] = {
	[TIPC_NLA_NODE_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_NODE_ADDR]		= { .type = NLA_U32 },
	[TIPC_NLA_NODE_UP]		= { .type = NLA_FLAG }
};

static const struct nla_policy tipc_nl_link_policy[TIPC_NLA_LINK_MAX + 1] = {
	[TIPC_NLA_LINK_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_LINK_NAME]		= { .type = NLA_STRING,
					    .len = TIPC_MAX_LINK_NAME },
	[TIPC_NLA_LINK_MTU]		= { .type = NLA_U32 },
	[TIPC_NLA_LINK_BROADCAST]	= { .type = NLA_FLAG },
	[TIPC_NLA_LINK_UP]		= { .type = NLA_FLAG },
	[TIPC_NLA_LINK_ACTIVE]		= { .type = NLA_FLAG },
	[TIPC_NLA_LINK_PROP]		= { .type = NLA_NESTED },
	[TIPC_NLA_LINK_STATS]		= { .type = NLA_NESTED },
	[TIPC_NLA_LINK_RX]		= { .type = NLA_U32 },
	[TIPC_NLA_LINK_TX]		= { .type = NLA_U32 }
};

/* Properties valid for media, bearar and link */
static const struct nla_policy tipc_nl_prop_policy[TIPC_NLA_PROP_MAX + 1] = {
	[TIPC_NLA_PROP_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_PROP_PRIO]		= { .type = NLA_U32 },
	[TIPC_NLA_PROP_TOL]		= { .type = NLA_U32 },
	[TIPC_NLA_PROP_WIN]		= { .type = NLA_U32 }
};

static const struct nla_policy
tipc_nl_bearer_policy[TIPC_NLA_BEARER_MAX + 1]	= {
	[TIPC_NLA_BEARER_UNSPEC]	= { .type = NLA_UNSPEC },
	[TIPC_NLA_BEARER_NAME]		= { .type = NLA_STRING,
					    .len = TIPC_MAX_BEARER_NAME },
	[TIPC_NLA_BEARER_PROP]		= { .type = NLA_NESTED },
	[TIPC_NLA_BEARER_DOMAIN]	= { .type = NLA_U32 }
};

static const struct nla_policy tipc_nl_media_policy[TIPC_NLA_MEDIA_MAX + 1] = {
	[TIPC_NLA_MEDIA_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_MEDIA_NAME]		= { .type = NLA_STRING },
	[TIPC_NLA_MEDIA_PROP]		= { .type = NLA_NESTED }
};

static const struct nla_policy tipc_nl_udp_policy[TIPC_NLA_UDP_MAX + 1] = {
	[TIPC_NLA_UDP_UNSPEC]	= { .type = NLA_UNSPEC },
	[TIPC_NLA_UDP_LOCAL]	= { .type = NLA_BINARY,
				    .len = sizeof(struct sockaddr_storage)},
	[TIPC_NLA_UDP_REMOTE]	= { .type = NLA_BINARY,
				    .len = sizeof(struct sockaddr_storage)},
};

static const struct nla_policy
tipc_nl_name_table_policy[TIPC_NLA_NAME_TABLE_MAX + 1] = {
	[TIPC_NLA_NAME_TABLE_UNSPEC]	= { .type = NLA_UNSPEC },
	[TIPC_NLA_NAME_TABLE_PUBL]	= { .type = NLA_NESTED }
};

int tipc_nlmsg_parse(const struct nlmsghdr *nlh, struct nlattr ***buf);

struct tipc_nl_msg {
	struct sk_buff *skb;
	u32 portid;
	u32 seq;
};

int tipc_netlink_start(void);
int tipc_netlink_compat_start(void);
void tipc_netlink_stop(void);
void tipc_netlink_compat_stop(void);

#endif
