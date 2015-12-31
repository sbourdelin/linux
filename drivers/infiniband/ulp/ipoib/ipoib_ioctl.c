/*
 * Copyright (c) 2015 Oracle Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
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

#include <net/arp.h>
#include <linux/jhash.h>
#include <net/inet_sock.h>
#include <net/route.h>

#include "ipoib.h"

static int ipoib_get_sguid(struct net_device *dev, int fd, u64 *sgid,
			   u64 *subnet_prefix)
{
	struct socket *sock;
	struct inet_sock *inetsock;
	struct neighbour *neigh;
	int rc = 0;
	union ib_gid *gid;
	struct list_head *dev_list = 0;
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	u16 pkey_index = priv->pkey_index;
	struct ipoib_dev_priv *child_priv;

	sock = sockfd_lookup(fd, &rc);
	if (IS_ERR_OR_NULL(sock))
		return -EINVAL;

	inetsock = inet_sk(sock->sk);

	neigh = neigh_lookup(&arp_tbl, &inetsock->inet_daddr, dev);
	if (!IS_ERR_OR_NULL(neigh))
		goto found;

	/* If not found try in all other ipoib devices */
	dev_list = ipoib_get_dev_list(priv->ca);
	if (!dev_list)
		return -EINVAL;

	list_for_each_entry(priv, dev_list, list) {
		if (priv->pkey_index == pkey_index) {
			neigh = neigh_lookup(&arp_tbl, &inetsock->inet_daddr,
					     priv->dev);
			if (!IS_ERR_OR_NULL(neigh))
				goto found;
		}
		list_for_each_entry(child_priv, &priv->child_intfs, list) {
			if (child_priv->pkey_index == pkey_index) {
				neigh = neigh_lookup(&arp_tbl,
						     &inetsock->inet_daddr,
						     child_priv->dev);
				if (!IS_ERR_OR_NULL(neigh))
					goto found;
			}
		}
	}

	return -ENODEV;

found:
	if (!(neigh->nud_state & NUD_VALID))
		return -EINVAL;

	gid = (union ib_gid *)(neigh->ha + 4);
	*sgid = be64_to_cpu(gid->global.interface_id);
	*subnet_prefix = be64_to_cpu(gid->global.subnet_prefix);

	neigh_release(neigh);

	return 0;
}

static int ipoib_ioctl_getsguid(struct net_device *dev, struct ifreq *ifr)
{
	struct ipoib_ioctl_getsgid_data req_data;
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	int rc;

	rc = copy_from_user(&req_data, ifr->ifr_data,
			    sizeof(struct ipoib_ioctl_getsgid_data));
	if (rc != 0) {
		ipoib_warn(priv, "ioctl fail to copy request data\n");
		return -EINVAL;
	}
	rc = ipoib_get_sguid(dev, req_data.fd, &req_data.gid,
			     &req_data.subnet_prefix);
	if (rc) {
		ipoib_warn(priv, "Invalid fd %d (err=%d)\n",
			   req_data.fd, rc);
		return rc;
	}
	ipoib_dbg(priv, "ioctl_getsgid: subnet_prefix=0x%llx\n",
		  req_data.subnet_prefix);
	ipoib_dbg(priv, "ioctl_getsgid: src_gid=0x%llx\n", req_data.gid);
	rc = copy_to_user(ifr->ifr_data, &req_data,
			  sizeof(struct ipoib_ioctl_getsgid_data));
	if (rc != 0) {
		ipoib_warn(priv,
			   "ioctl fail to copy back request data\n");
		return -EINVAL;
	}

	return rc;
}

int ipoib_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct ipoib_dev_priv *priv = netdev_priv(dev);
	int rc = -EINVAL;

	switch (cmd) {
	case IPOIBGETSGUID:
		rc = ipoib_ioctl_getsguid(dev, ifr);
		if (rc != 0)
			return -EINVAL;
		break;
	default:
		ipoib_warn(priv, "invalid ioctl opcode 0x%x\n", cmd);
		rc = -EINVAL;
		break;
	}

	return rc;
}
