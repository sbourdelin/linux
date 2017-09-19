/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  SMC Rendezvous to determine SMC-capability of the peer
 *
 *  Copyright IBM Corp. 2017
 *
 *  Author(s):  Hans Wippel <hwippel@linux.vnet.ibm.com>
 *              Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/tcp.h>

#include "smc.h"
#include "smc_rv.h"

#define TCPOLEN_SMC			8
#define TCPOLEN_SMC_BASE		6
#define TCPOLEN_SMC_ALIGNED		2

static const char TCPOPT_SMC_MAGIC[4] = {'\xe2', '\xd4', '\xc3', '\xd9'};

/* in TCP header, replace EOL option and remaining header bytes with NOPs */
static bool smc_rv_replace_eol_option(struct sk_buff *skb)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	int opt_bytes = tcp_optlen(skb);
	unsigned char *buf;
	int i = 0;

	buf = (unsigned char *)(tcph + 1);
	/* Parse TCP options. Based on tcp_parse_options in tcp_input.c */
	while (i < opt_bytes) {
		switch (buf[i]) {
		/* one byte options */
		case TCPOPT_EOL:
			/* replace remaining bytes with NOPs */
			while (i < opt_bytes) {
				buf[i] = TCPOPT_NOP;
				i++;
			}
			return true;
		case TCPOPT_NOP:
			i++;
			continue;
		default:
			/* multi-byte options */
			if (buf[i + 1] < 2 || i + buf[i + 1] > opt_bytes)
				return false; /* bad option */
			i += buf[i + 1];
			continue;
		}
	}
	return true;
}

/* check if TCP header contains SMC option */
static bool smc_rv_has_smc_option(struct sk_buff *skb)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	int opt_bytes = tcp_optlen(skb);
	unsigned char *buf;
	int i = 0;

	buf = (unsigned char *)(tcph + 1);
	/* Parse TCP options. Based on tcp_parse_options in tcp_input.c */
	while (i < opt_bytes) {
		switch (buf[i]) {
		/* one byte options */
		case TCPOPT_EOL:
			return 0;
		case TCPOPT_NOP:
			i++;
			continue;
		default:
			/* multi-byte options */
			if (buf[i + 1] < 2)
				return 0; /* bad option */
			/* check for SMC rendezvous option */
			if (buf[i] == TCPOPT_EXP &&
			    buf[i + 1] == TCPOLEN_SMC_BASE &&
			    (opt_bytes - i >= TCPOLEN_SMC_BASE) &&
			    !memcmp(&buf[i + 2], TCPOPT_SMC_MAGIC,
						sizeof(TCPOPT_SMC_MAGIC)))
				return true;
			i += buf[i + 1];
			continue;
		}
	}

	return false;
}

/* Add SMC option to TCP header. Note: This assumes that there are no data after
 * the TCP header.
 */
static int smc_rv_add_smc_option(struct sk_buff *skb)
{
	unsigned char smc_opt[] = {TCPOPT_NOP, TCPOPT_NOP,
				   TCPOPT_EXP, TCPOLEN_SMC_BASE,
				   TCPOPT_SMC_MAGIC[0], TCPOPT_SMC_MAGIC[1],
				   TCPOPT_SMC_MAGIC[2], TCPOPT_SMC_MAGIC[3]};
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	int tcplen = 0;

	if (skb_tailroom(skb) < TCPOLEN_SMC)
		return -EFAULT;

	if (((tcph->doff << 2) - sizeof(*tcph) + TCPOLEN_SMC) >
							MAX_TCP_OPTION_SPACE)
		return -EFAULT;

	if (smc_rv_has_smc_option(skb))
		return -EFAULT;

	if (!smc_rv_replace_eol_option(skb))
		return -EFAULT;

	iph->tot_len = cpu_to_be16(be16_to_cpu(iph->tot_len) + TCPOLEN_SMC);
	iph->check = 0;
	iph->check = ip_fast_csum(iph, iph->ihl);
	skb_put_data(skb, smc_opt, TCPOLEN_SMC);
	tcph->doff += TCPOLEN_SMC_ALIGNED;
	tcplen = (skb->len - ip_hdrlen(skb));
	tcph->check = 0;
	tcph->check = tcp_v4_check(tcplen, iph->saddr, iph->daddr,
				   csum_partial(tcph, tcplen, 0));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

/* return an smc socket with certain source and destination */
static struct smc_sock *smc_rv_lookup_connecting_smc(struct net *net,
						     __be32 dest_addr,
						     __be16 dest_port,
						     __be32 source_addr,
						     __be16 source_port)
{
	struct smc_sock *smc = NULL;
	struct hlist_head *head;
	struct socket *clcsock;
	struct sock *sk;

	read_lock(&smc_proto.h.smc_hash->lock);
	head = &smc_proto.h.smc_hash->ht;

	if (hlist_empty(head))
		goto out;

	sk_for_each(sk, head) {
		if (!net_eq(sock_net(sk), net))
			continue;
		if (sk->sk_state != SMC_INIT)
			continue;
		clcsock = smc_sk(sk)->clcsock;
		if (!clcsock)
			continue;
		if (source_port != htons(clcsock->sk->sk_num))
			continue;
		if (source_addr != clcsock->sk->sk_rcv_saddr)
			continue;
		if (dest_port != clcsock->sk->sk_dport)
			continue;
		if (dest_addr == clcsock->sk->sk_daddr) {
			smc = smc_sk(sk);
			break;
		}
	}

out:
	read_unlock(&smc_proto.h.smc_hash->lock);
	return smc;
}

/* for netfilter smc_rv_hook_out_clnt (outgoing SYN):
 * check if there exists a connecting smc socket with certain source and
 * destination
 */
static bool smc_rv_exists_connecting_smc(struct net *net,
					 __be32 dest_addr,
					 __be16 dest_port,
					 __be32 source_addr,
					 __be16 source_port)
{
	return (smc_rv_lookup_connecting_smc(net, dest_addr, dest_port,
					     source_addr, source_port) ?
								true : false);
}

/* for netfilter smc_rv_hook_in_clnt (incoming SYN ACK):
 * enable SMC-capability for the corresponding socket
 */
static void smc_rv_accepting_smc_peer(struct net *net,
				      __be32 dest_addr,
				      __be16 dest_port,
				      __be32 source_addr,
				      __be16 source_port)
{
	struct smc_sock *smc;

	smc = smc_rv_lookup_connecting_smc(net, dest_addr, dest_port,
					   source_addr, source_port);
	if (smc)
		/* connection is SMC-capable */
		smc->use_fallback = false;
}

/* return an smc socket listening on a certain port */
static struct smc_sock *smc_rv_lookup_listen_socket(struct net *net,
						    __be32 listen_addr,
						    __be16 listen_port)
{
	struct smc_sock *smc = NULL;
	struct hlist_head *head;
	struct socket *clcsock;
	struct sock *sk;

	read_lock(&smc_proto.h.smc_hash->lock);
	head = &smc_proto.h.smc_hash->ht;

	if (hlist_empty(head))
		goto out;

	sk_for_each(sk, head) {
		if (!net_eq(sock_net(sk), net))
			continue;
		if (sk->sk_state != SMC_LISTEN)
			continue;
		clcsock = smc_sk(sk)->clcsock;
		if (listen_port != htons(clcsock->sk->sk_num))
			continue;
		if (!listen_addr || !clcsock->sk->sk_rcv_saddr ||
		    listen_addr == clcsock->sk->sk_rcv_saddr) {
			smc = smc_sk(sk);
			break;
		}
	}

out:
	read_unlock(&smc_proto.h.smc_hash->lock);
	return smc;
}

/* for netfilter smc_rv_hook_in_serv (incoming SYN):
 * save addr and port of connecting smc peer
 */
static void smc_rv_connecting_smc_peer(struct net *net,
				       __be32 listen_addr,
				       __be16 listen_port,
				       __be32 peer_addr,
				       __be16 peer_port)
{
	struct smc_listen_pending *pnd;
	struct smc_sock *lsmc;
	unsigned long flags;
	int i;

	lsmc = smc_rv_lookup_listen_socket(net, listen_addr, listen_port);
	if (!lsmc)
		return;

	spin_lock_irqsave(&lsmc->listen_pends_lock, flags);
	for (i = 0; i < 2 * lsmc->sk.sk_max_ack_backlog; i++) {
		pnd = lsmc->listen_pends + i;
		/* either use an unused entry or reuse an outdated entry */
		if (!pnd->used ||
		    jiffies_to_msecs(get_jiffies_64() - pnd->time) >
						SMC_LISTEN_PEND_VALID_TIME) {
			pnd->used = true;
			pnd->addr = peer_addr;
			pnd->port = peer_port;
			pnd->time = get_jiffies_64();
			break;
		}
	}
	spin_unlock_irqrestore(&lsmc->listen_pends_lock, flags);
}

/* for netfilter smc_rv_hook_out_serv (outgoing SYN/ACK):
 * remove listen_pends entry of connecting smc peer in case of a problem
 */
static void smc_rv_remove_smc_peer(struct net *net,
				   __be32 listen_addr,
				   __be16 listen_port,
				   __be32 peer_addr,
				   __be16 peer_port)
{
	struct smc_listen_pending *pnd;
	struct smc_sock *lsmc;
	unsigned long flags;
	int i;

	lsmc = smc_rv_lookup_listen_socket(net, listen_addr, listen_port);
	if (!lsmc)
		return;

	spin_lock_irqsave(&lsmc->listen_pends_lock, flags);
	for (i = 0; i < 2 * lsmc->sk.sk_max_ack_backlog; i++) {
		pnd = lsmc->listen_pends + i;
		if (pnd->used &&
		    pnd->addr == peer_addr &&
		    pnd->port == peer_port &&
		    jiffies_to_msecs(get_jiffies_64() - pnd->time) <=
						SMC_LISTEN_PEND_VALID_TIME) {
			pnd->used = false;
			break;
		}
	}
	spin_unlock_irqrestore(&lsmc->listen_pends_lock, flags);
}

/* for netfilter smc_rv_hook_out_serv (outgoing SYN ACK):
 * check if there has been a connecting smc peer
 */
static bool smc_rv_exists_connecting_smc_peer(struct net *net,
					      __be32 listen_addr,
					      __be16 listen_port,
					      __be32 peer_addr,
					      __be16 peer_port)
{
	struct smc_listen_pending *pnd;
	struct smc_sock *lsmc;
	unsigned long flags;
	int i;

	lsmc = smc_rv_lookup_listen_socket(net, listen_addr, listen_port);
	if (!lsmc)
		return false;

	spin_lock_irqsave(&lsmc->listen_pends_lock, flags);
	for (i = 0; i < 2 * lsmc->sk.sk_max_ack_backlog; i++) {
		pnd = lsmc->listen_pends + i;
		if (pnd->used &&
		    pnd->addr == peer_addr &&
		    pnd->port == peer_port &&
		    jiffies_to_msecs(get_jiffies_64() - pnd->time) <=
						SMC_LISTEN_PEND_VALID_TIME) {
			spin_unlock_irqrestore(&lsmc->listen_pends_lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&lsmc->listen_pends_lock, flags);
	return false;
}

/* Netfilter hooks */

/* netfilter hook for incoming packets (client) */
static unsigned int smc_rv_hook_in_clnt(void *priv, struct sk_buff *skb,
					const struct nf_hook_state *state)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph;

	if (skb_headlen(skb) - sizeof(*iph) < sizeof(*tcph))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	/* Local SMC client, incoming SYN,ACK from server
	 * check if there really is a local SMC client
	 * and tell the client connection if the server is SMC capable
	 */
	if (tcph->syn == 1 && tcph->ack == 1) {
		/* check for experimental option */
		if (!smc_rv_has_smc_option(skb))
			return NF_ACCEPT;
		/* add info about server SMC capability */
		smc_rv_accepting_smc_peer(state->net, iph->saddr, tcph->source,
					  iph->daddr, tcph->dest);
	}
	return NF_ACCEPT;
}

/* netfilter hook for incoming packets (server) */
static unsigned int smc_rv_hook_in_serv(void *priv, struct sk_buff *skb,
					const struct nf_hook_state *state)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph;

	if (skb_headlen(skb) - sizeof(*iph) < sizeof(*tcph))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	/* Local SMC Server, incoming SYN request from client
	 * check if there is a local SMC server
	 * and tell the server if there is a new SMC capable client
	 */
	if (tcph->syn == 1 && tcph->ack == 0) {
		/* check for experimental option */
		if (!smc_rv_has_smc_option(skb))
			return NF_ACCEPT;
		/* add info about new client SMC capability */
		smc_rv_connecting_smc_peer(state->net, iph->daddr, tcph->dest,
					   iph->saddr, tcph->source);
	}
	return NF_ACCEPT;
}

/* netfilter hook for outgoing packets (client) */
static unsigned int smc_rv_hook_out_clnt(void *priv, struct sk_buff *skb,
					 const struct nf_hook_state *state)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph;

	if (skb_headlen(skb) - sizeof(*iph) < sizeof(*tcph))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	/* Local SMC client, outgoing SYN request to server
	 * add TCP experimental option if there really is a local SMC client
	 */
	if (tcph->syn == 1 && tcph->ack == 0) {
		/* check for local SMC client */
		if (!smc_rv_exists_connecting_smc(state->net,
						  iph->daddr, tcph->dest,
						  iph->saddr, tcph->source))
			return NF_ACCEPT;
		/* add experimental option */
		smc_rv_add_smc_option(skb);
	}
	return NF_ACCEPT;
}

/* netfilter hook for outgoing packets (server) */
static unsigned int smc_rv_hook_out_serv(void *priv, struct sk_buff *skb,
					 const struct nf_hook_state *state)
{
	struct tcphdr *tcph = tcp_hdr(skb);
	struct iphdr *iph;

	if (skb_headlen(skb) - sizeof(*iph) < sizeof(*tcph))
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	/* Local SMC server, outgoing SYN,ACK to client
	 * add TCP experimental option if there really is a local SMC server
	 */
	if (tcph->syn == 1 && tcph->ack == 1) {
		/* check if client's SYN contained the experimental option */
		if (!smc_rv_exists_connecting_smc_peer(state->net,
						       iph->saddr, tcph->source,
						       iph->daddr, tcph->dest))
			return NF_ACCEPT;
		/* add experimental option */
		if (smc_rv_add_smc_option(skb) < 0)
			smc_rv_remove_smc_peer(state->net,
					       iph->saddr, tcph->source,
					       iph->daddr, tcph->dest);
	}
	return NF_ACCEPT;
}

static struct nf_hook_ops smc_nfho_ops_clnt[] = {
	{
		.hook = smc_rv_hook_in_clnt,
		.hooknum = NF_INET_PRE_ROUTING,
		.pf = PF_INET,
		.priority = NF_IP_PRI_FIRST,
	},
	{
		.hook = smc_rv_hook_out_clnt,
		.hooknum = NF_INET_POST_ROUTING,
		.pf = PF_INET,
		.priority = NF_IP_PRI_FIRST,
	},
};

static struct nf_hook_ops smc_nfho_ops_serv[] = {
	{
		.hook = smc_rv_hook_in_serv,
		.hooknum = NF_INET_PRE_ROUTING,
		.pf = PF_INET,
		.priority = NF_IP_PRI_FIRST,
	},
	{
		.hook = smc_rv_hook_out_serv,
		.hooknum = NF_INET_POST_ROUTING,
		.pf = PF_INET,
		.priority = NF_IP_PRI_FIRST,
	},
};

struct smc_nf_hook smc_nfho_clnt = {
	.refcount = 0,
	.hook = &smc_nfho_ops_clnt[0],
};

struct smc_nf_hook smc_nfho_serv = {
	.refcount = 0,
	.hook = &smc_nfho_ops_serv[0],
};

int smc_rv_nf_register_hook(struct net *net, struct smc_nf_hook *nfho)
{
	int rc = 0;

	mutex_lock(&nfho->nf_hook_mutex);
	if (!(nfho->refcount++)) {
		rc = nf_register_net_hooks(net, nfho->hook, 2);
		if (rc)
			nfho->refcount--;
	}
	mutex_unlock(&nfho->nf_hook_mutex);
	return rc;
}

void smc_rv_nf_unregister_hook(struct net *net, struct smc_nf_hook *nfho)
{
	mutex_lock(&nfho->nf_hook_mutex);
	if (!(--nfho->refcount))
		nf_unregister_net_hooks(net, nfho->hook, 2);
	mutex_unlock(&nfho->nf_hook_mutex);
}

void __init smc_rv_init(void)
{
	mutex_init(&smc_nfho_clnt.nf_hook_mutex);
	mutex_init(&smc_nfho_serv.nf_hook_mutex);
}
