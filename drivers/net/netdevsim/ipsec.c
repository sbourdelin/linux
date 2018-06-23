// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2018 Oracle and/or its affiliates. All rights reserved. */

#include <net/xfrm.h>
#include <crypto/aead.h>
#include <linux/debugfs.h>
#include "netdevsim.h"

#define NSIM_IPSEC_AUTH_BITS	128

/**
 * nsim_ipsec_dbg_read - read for ipsec data
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 **/
static ssize_t nsim_dbg_netdev_ops_read(struct file *filp,
					char __user *buffer,
					size_t count, loff_t *ppos)
{
	struct netdevsim *ns = filp->private_data;
	struct nsim_ipsec *ipsec = &ns->ipsec;
	size_t bufsize;
	char *buf, *p;
	int len;
	int i;

	/* don't allow partial reads */
	if (*ppos != 0)
		return 0;

	/* the buffer needed is
	 * (num SAs * 3 lines each * ~60 bytes per line) + one more line
	 */
	bufsize = (ipsec->count * 4 * 60) + 60;
	buf = kzalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	p = buf;
	p += snprintf(p, bufsize - (p - buf),
		      "SA count=%u tx=%u\n",
		      ipsec->count, ipsec->tx);

	for (i = 0; i < NSIM_IPSEC_MAX_SA_COUNT; i++) {
		struct nsim_sa *sap = &ipsec->sa[i];

		if (!sap->used)
			continue;

		p += snprintf(p, bufsize - (p - buf),
			      "sa[%i] %cx ipaddr=0x%08x %08x %08x %08x\n",
			      i, (sap->rx ? 'r' : 't'), sap->ipaddr[0],
			      sap->ipaddr[1], sap->ipaddr[2], sap->ipaddr[3]);
		p += snprintf(p, bufsize - (p - buf),
			      "sa[%i]    spi=0x%08x proto=0x%x salt=0x%08x crypt=%d\n",
			      i, be32_to_cpu(sap->xs->id.spi),
			      sap->xs->id.proto, sap->salt, sap->crypt);
		p += snprintf(p, bufsize - (p - buf),
			      "sa[%i]    key=0x%08x %08x %08x %08x\n",
			      i, sap->key[0], sap->key[1],
			      sap->key[2], sap->key[3]);
	}

	len = simple_read_from_buffer(buffer, count, ppos, buf, p - buf);

	kfree(buf);
	return len;
}

static const struct file_operations ipsec_dbg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = nsim_dbg_netdev_ops_read,
};

/**
 * nsim_ipsec_find_empty_idx - find the first unused security parameter index
 * @ipsec: pointer to ipsec struct
 **/
static int nsim_ipsec_find_empty_idx(struct nsim_ipsec *ipsec)
{
	u32 i;

	if (ipsec->count == NSIM_IPSEC_MAX_SA_COUNT)
		return -ENOSPC;

	/* search sa table */
	for (i = 0; i < NSIM_IPSEC_MAX_SA_COUNT; i++) {
		if (!ipsec->sa[i].used)
			return i;
	}

	return -ENOSPC;
}

/**
 * nsim_ipsec_parse_proto_keys - find the key and salt based on the protocol
 * @xs: pointer to xfrm_state struct
 * @mykey: pointer to key array to populate
 * @mysalt: pointer to salt value to populate
 *
 * This copies the protocol keys and salt to our own data tables.  The
 * 82599 family only supports the one algorithm.
 **/
static int nsim_ipsec_parse_proto_keys(struct xfrm_state *xs,
				       u32 *mykey, u32 *mysalt)
{
	struct net_device *dev = xs->xso.dev;
	unsigned char *key_data;
	char *alg_name = NULL;
	const char aes_gcm_name[] = "rfc4106(gcm(aes))";
	int key_len;

	if (!xs->aead) {
		netdev_err(dev, "Unsupported IPsec algorithm\n");
		return -EINVAL;
	}

	if (xs->aead->alg_icv_len != NSIM_IPSEC_AUTH_BITS) {
		netdev_err(dev, "IPsec offload requires %d bit authentication\n",
			   NSIM_IPSEC_AUTH_BITS);
		return -EINVAL;
	}

	key_data = &xs->aead->alg_key[0];
	key_len = xs->aead->alg_key_len;
	alg_name = xs->aead->alg_name;

	if (strcmp(alg_name, aes_gcm_name)) {
		netdev_err(dev, "Unsupported IPsec algorithm - please use %s\n",
			   aes_gcm_name);
		return -EINVAL;
	}

	/* The key bytes come down in a bigendian array of bytes, so
	 * we don't need to do any byteswapping.
	 * 160 accounts for 16 byte key and 4 byte salt
	 */
	if (key_len > 128) {
		*mysalt = ((u32 *)key_data)[4];
	} else if (key_len == 128) {
		*mysalt = 0;
	} else {
		netdev_err(dev, "IPsec hw offload only supports 128 bit keys with optional 32 bit salt\n");
		return -EINVAL;
	}
	memcpy(mykey, key_data, 16);

	return 0;
}

/**
 * nsim_ipsec_add_sa - program device with a security association
 * @xs: pointer to transformer state struct
 **/
static int nsim_ipsec_add_sa(struct xfrm_state *xs)
{
	struct net_device *dev = xs->xso.dev;
	struct netdevsim *ns = netdev_priv(dev);
	struct nsim_ipsec *ipsec = &ns->ipsec;
	struct nsim_sa sa;
	u16 sa_idx;
	int ret;

	if (xs->id.proto != IPPROTO_ESP && xs->id.proto != IPPROTO_AH) {
		netdev_err(dev, "Unsupported protocol 0x%04x for ipsec offload\n",
			   xs->id.proto);
		return -EINVAL;
	}

	if (xs->calg) {
		netdev_err(dev, "Compression offload not supported\n");
		return -EINVAL;
	}

	/* find the first unused index */
	ret = nsim_ipsec_find_empty_idx(ipsec);
	if (ret < 0) {
		netdev_err(dev, "No space for SA in Rx table!\n");
		return ret;
	}
	sa_idx = (u16)ret;

	memset(&sa, 0, sizeof(sa));
	sa.used = true;
	sa.xs = xs;

	if (sa.xs->id.proto & IPPROTO_ESP)
		sa.crypt = xs->ealg || xs->aead;

	/* get the key and salt */
	ret = nsim_ipsec_parse_proto_keys(xs, sa.key, &sa.salt);
	if (ret) {
		netdev_err(dev, "Failed to get key data for SA table\n");
		return ret;
	}

	if (xs->xso.flags & XFRM_OFFLOAD_INBOUND) {
		sa.rx = true;

		if (xs->props.family == AF_INET6)
			memcpy(sa.ipaddr, &xs->id.daddr.a6, 16);
		else
			memcpy(&sa.ipaddr[3], &xs->id.daddr.a4, 4);
	}

	/* the preparations worked, so save the info */
	memcpy(&ipsec->sa[sa_idx], &sa, sizeof(sa));

	/* the XFRM stack doesn't like offload_handle == 0,
	 * so add a bitflag in case our array index is 0
	 */
	xs->xso.offload_handle = sa_idx | NSIM_IPSEC_VALID;
	ipsec->count++;

	return 0;
}

/**
 * nsim_ipsec_del_sa - clear out this specific SA
 * @xs: pointer to transformer state struct
 **/
static void nsim_ipsec_del_sa(struct xfrm_state *xs)
{
	struct net_device *dev = xs->xso.dev;
	struct netdevsim *ns = netdev_priv(dev);
	struct nsim_ipsec *ipsec = &ns->ipsec;
	u16 sa_idx;

	sa_idx = xs->xso.offload_handle & ~NSIM_IPSEC_VALID;
	if (!ipsec->sa[sa_idx].used) {
		netdev_err(dev, "Invalid SA for delete sa_idx=%d\n", sa_idx);
		return;
	}

	memset(&ipsec->sa[sa_idx], 0, sizeof(struct nsim_sa));
	ipsec->count--;
}

/**
 * nsim_ipsec_offload_ok - can this packet use the xfrm hw offload
 * @skb: current data packet
 * @xs: pointer to transformer state struct
 **/
static bool nsim_ipsec_offload_ok(struct sk_buff *skb, struct xfrm_state *xs)
{
	struct net_device *dev = xs->xso.dev;
	struct netdevsim *ns = netdev_priv(dev);
	struct nsim_ipsec *ipsec = &ns->ipsec;

	ipsec->ok++;

	return true;
}

static const struct xfrmdev_ops nsim_xfrmdev_ops = {
	.xdo_dev_state_add = nsim_ipsec_add_sa,
	.xdo_dev_state_delete = nsim_ipsec_del_sa,
	.xdo_dev_offload_ok = nsim_ipsec_offload_ok,
};

/**
 * nsim_ipsec_tx - check Tx packet for ipsec offload
 * @ns: pointer to ns structure
 * @skb: current data packet
 **/
int nsim_ipsec_tx(struct netdevsim *ns, struct sk_buff *skb)
{
	struct nsim_ipsec *ipsec = &ns->ipsec;
	struct xfrm_state *xs;
	struct nsim_sa *tsa;
	u32 sa_idx;

	/* do we even need to check this packet? */
	if (!skb->sp)
		return 1;

	if (unlikely(!skb->sp->len)) {
		netdev_err(ns->netdev, "%s: no xfrm state len = %d\n",
			   __func__, skb->sp->len);
		return 0;
	}

	xs = xfrm_input_state(skb);
	if (unlikely(!xs)) {
		netdev_err(ns->netdev, "%s: no xfrm_input_state() xs = %p\n",
			   __func__, xs);
		return 0;
	}

	sa_idx = xs->xso.offload_handle & ~NSIM_IPSEC_VALID;
	if (unlikely(sa_idx > NSIM_IPSEC_MAX_SA_COUNT)) {
		netdev_err(ns->netdev, "%s: bad sa_idx=%d max=%d\n",
			   __func__, sa_idx, NSIM_IPSEC_MAX_SA_COUNT);
		return 0;
	}

	tsa = &ipsec->sa[sa_idx];
	if (unlikely(!tsa->used)) {
		netdev_err(ns->netdev, "%s: unused sa_idx=%d\n",
			   __func__, sa_idx);
		return 0;
	}

	if (xs->id.proto != IPPROTO_ESP && xs->id.proto != IPPROTO_AH) {
		netdev_err(ns->netdev, "%s: unexpected proto=%d\n",
			   __func__, xs->id.proto);
		return 0;
	}

	ipsec->tx++;

	return 1;
}

/**
 * nsim_ipsec_init - initialize security registers for IPSec operation
 * @ns: board private structure
 **/
void nsim_ipsec_init(struct netdevsim *ns)
{
	ns->netdev->xfrmdev_ops = &nsim_xfrmdev_ops;

#define NSIM_ESP_FEATURES	(NETIF_F_HW_ESP | \
				 NETIF_F_HW_ESP_TX_CSUM | \
				 NETIF_F_GSO_ESP)

	ns->netdev->features |= NSIM_ESP_FEATURES;
	ns->netdev->hw_enc_features |= NSIM_ESP_FEATURES;

	ns->ipsec.pfile = debugfs_create_file("ipsec", 0400, ns->ddir, ns,
					      &ipsec_dbg_fops);
}

void nsim_ipsec_teardown(struct netdevsim *ns)
{
	struct nsim_ipsec *ipsec = &ns->ipsec;

	if (ipsec->count)
		netdev_err(ns->netdev, "%s: tearing down IPsec offload with %d SAs left\n",
			   __func__, ipsec->count);
	debugfs_remove_recursive(ipsec->pfile);
}
