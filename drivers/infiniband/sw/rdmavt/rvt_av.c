/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/if.h>
#include <linux/if_vlan.h>
#include <net/udp_tunnel.h>
#include <net/sch_generic.h>
#include <linux/netfilter.h>
#include <rdma/ib_addr.h>

#include "rvt_loc.h"

int rvt_av_chk_attr(struct rvt_dev *rvt, struct ib_ah_attr *attr)
{
	struct rvt_port *port;

	if (attr->port_num < 1 || attr->port_num > rvt->num_ports) {
		pr_info("rvt: invalid port_num = %d\n", attr->port_num);
		return -EINVAL;
	}

	port = &rvt->port[attr->port_num - 1];

	if (attr->ah_flags & IB_AH_GRH) {
		if (attr->grh.sgid_index > port->attr.gid_tbl_len) {
			pr_info("rvt: invalid sgid index = %d\n",
				attr->grh.sgid_index);
			return -EINVAL;
		}
	}

	return 0;
}

int rvt_av_from_attr(struct rvt_dev *rvt, u8 port_num,
		     struct rvt_av *av, struct ib_ah_attr *attr)
{
	memset(av, 0, sizeof(*av));
	memcpy(&av->grh, &attr->grh, sizeof(attr->grh));
	av->port_num = port_num;
	return 0;
}

int rvt_av_to_attr(struct rvt_dev *rvt, struct rvt_av *av,
		   struct ib_ah_attr *attr)
{
	memcpy(&attr->grh, &av->grh, sizeof(av->grh));
	attr->port_num = av->port_num;
	return 0;
}

int rvt_av_fill_ip_info(struct rvt_dev *rvt,
			struct rvt_av *av,
			struct ib_ah_attr *attr,
			struct ib_gid_attr *sgid_attr,
			union ib_gid *sgid)
{
	rdma_gid2ip(&av->sgid_addr._sockaddr, sgid);
	rdma_gid2ip(&av->dgid_addr._sockaddr, &attr->grh.dgid);
	av->network_type = ib_gid_to_network_type(sgid_attr->gid_type, sgid);

	return 0;
}

static struct rtable *rvt_find_route4(struct in_addr *saddr,
				      struct in_addr *daddr)
{
	struct rtable *rt;
	struct flowi4 fl = { { 0 } };

	memset(&fl, 0, sizeof(fl));
	memcpy(&fl.saddr, saddr, sizeof(*saddr));
	memcpy(&fl.daddr, daddr, sizeof(*daddr));
	fl.flowi4_proto = IPPROTO_UDP;

	rt = ip_route_output_key(&init_net, &fl);
	if (IS_ERR(rt)) {
		pr_err("no route to %pI4\n", &daddr->s_addr);
		return NULL;
	}

	return rt;
}

static struct dst_entry *rvt_find_route6(struct net_device *ndev,
					 struct in6_addr *saddr,
					 struct in6_addr *daddr)
{
	/* TODO get rid of ipv6_stub */
	/*
	struct dst_entry *ndst;
	struct flowi6 fl6 = { { 0 } };

	memset(&fl6, 0, sizeof(fl6));
	fl6.flowi6_oif = ndev->ifindex;
	memcpy(&fl6.saddr, saddr, sizeof(*saddr));
	memcpy(&fl6.daddr, daddr, sizeof(*daddr));
	fl6.flowi6_proto = IPPROTO_UDP;

	if (unlikely(ipv6_stub->ipv6_dst_lookup(sock_net(recv_sockets.sk6->sk),
					recv_sockets.sk6->sk, &ndst, &fl6))) {
		pr_err("no route to %pI6\n", daddr);
		goto put;
	}

	if (unlikely(ndst->error)) {
		pr_err("no route to %pI6\n", daddr);
		goto put;
	}

	return ndst;
put:
	dst_release(ndst);
	*/
	return NULL;
}

static void prepare_ipv4_hdr(struct rtable *rt, struct sk_buff *skb,
			     __be32 src, __be32 dst, __u8 proto,
			     __u8 tos, __u8 ttl, __be16 df, bool xnet)
{
	struct iphdr *iph;

	skb_scrub_packet(skb, xnet);

	skb_clear_hash(skb);
	skb_dst_set(skb, &rt->dst);
	memset(IPCB(skb), 0, sizeof(*IPCB(skb)));

	skb_push(skb, sizeof(struct iphdr));
	skb_reset_network_header(skb);

	iph = ip_hdr(skb);

	iph->version	=	IPVERSION;
	iph->ihl	=	sizeof(struct iphdr) >> 2;
	iph->frag_off	=	df;
	iph->protocol	=	proto;
	iph->tos	=	tos;
	iph->daddr	=	dst;
	iph->saddr	=	src;
	iph->ttl	=	ttl;
	__ip_select_ident(dev_net(rt->dst.dev), iph,
			  skb_shinfo(skb)->gso_segs ?: 1);
	iph->tot_len = htons(skb->len);
	ip_send_check(iph);
}

static void prepare_ipv6_hdr(struct dst_entry *dst, struct sk_buff *skb,
			     struct in6_addr *saddr, struct in6_addr *daddr,
			     __u8 proto, __u8 prio, __u8 ttl)
{
	struct ipv6hdr *ip6h;

	memset(&(IPCB(skb)->opt), 0, sizeof(IPCB(skb)->opt));
	IPCB(skb)->flags &= ~(IPSKB_XFRM_TUNNEL_SIZE | IPSKB_XFRM_TRANSFORMED
			    | IPSKB_REROUTED);
	skb_dst_set(skb, dst);

	__skb_push(skb, sizeof(*ip6h));
	skb_reset_network_header(skb);
	ip6h		  = ipv6_hdr(skb);
	ip6_flow_hdr(ip6h, prio, htonl(0));
	ip6h->payload_len = htons(skb->len);
	ip6h->nexthdr     = proto;
	ip6h->hop_limit   = ttl;
	ip6h->daddr	  = *daddr;
	ip6h->saddr	  = *saddr;
	ip6h->payload_len = htons(skb->len - sizeof(*ip6h));
}

static void prepare_udp_hdr(struct sk_buff *skb, __be16 src_port,
			    __be16 dst_port)
{
	struct udphdr *udph;

	__skb_push(skb, sizeof(*udph));
	skb_reset_transport_header(skb);
	udph = udp_hdr(skb);

	udph->dest = dst_port;
	udph->source = src_port;
	udph->len = htons(skb->len);
	udph->check = 0;
}

static int prepare4(struct sk_buff *skb, struct rvt_av *av)
{
	struct rtable *rt;
	bool xnet = false;
	__be16 df = htons(IP_DF);
	struct in_addr *saddr = &av->sgid_addr._sockaddr_in.sin_addr;
	struct in_addr *daddr = &av->dgid_addr._sockaddr_in.sin_addr;

	rt = rvt_find_route4(saddr, daddr);
	if (!rt) {
		pr_err("Host not reachable\n");
		return -EHOSTUNREACH;
	}

	prepare_udp_hdr(skb, htons(ROCE_V2_UDP_SPORT),
			htons(ROCE_V2_UDP_DPORT));

	prepare_ipv4_hdr(rt, skb, saddr->s_addr, daddr->s_addr, IPPROTO_UDP,
			 av->grh.traffic_class, av->grh.hop_limit, df, xnet);
	return 0;
}

static int prepare6(struct rvt_dev *rdev, struct sk_buff *skb, struct rvt_av *av)
{
	struct dst_entry *dst;
	struct in6_addr *saddr = &av->sgid_addr._sockaddr_in6.sin6_addr;
	struct in6_addr *daddr = &av->dgid_addr._sockaddr_in6.sin6_addr;
	struct net_device *ndev = rdev->ifc_ops->get_netdev ?
		rdev->ifc_ops->get_netdev(rdev, av->port_num) : NULL;

	if (!ndev)
		return -EHOSTUNREACH;

	dst = rvt_find_route6(ndev, saddr, daddr);
	if (!dst) {
		pr_err("Host not reachable\n");
		return -EHOSTUNREACH;
	}

	prepare_udp_hdr(skb, htons(ROCE_V2_UDP_SPORT),
			htons(ROCE_V2_UDP_DPORT));

	prepare_ipv6_hdr(dst, skb, saddr, daddr, IPPROTO_UDP,
			 av->grh.traffic_class,
			 av->grh.hop_limit);
	return 0;
}
int rvt_prepare(struct rvt_dev *rdev, struct rvt_pkt_info *pkt,
		   struct sk_buff *skb, u32 *crc)
{
	int err = 0;
	struct rvt_av *av = get_av(pkt);

	if (av->network_type == RDMA_NETWORK_IPV4)
		err = prepare4(skb, av);
	else if (av->network_type == RDMA_NETWORK_IPV6)
		err = prepare6(rdev, skb, av);

	*crc = rvt_icrc_hdr(pkt, skb);

	return err;
}
