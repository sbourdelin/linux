/*
 * Copyright (C) 2017 Netronome Systems, Inc.
 *
 * This software is dual licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree or the BSD 2-Clause License provided below.  You have the
 * option to license this software under the complete terms of either license.
 *
 * The BSD 2-Clause License:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      1. Redistributions of source code must retain the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer.
 *
 *      2. Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials
 *         provided with the distribution.
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

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>

#define DRV_NAME	"netdevsim"

#define NSIM_XDP_MAX_MTU	4000

#define NSIM_EA(extack, msg)	NL_SET_ERR_MSG_MOD((extack), msg)

struct bpf_prog;
struct dentry;

struct netdevsim {
	struct net_device *netdev;

	u64 tx_packets;
	u64 tx_bytes;
	struct u64_stats_sync syncp;

	struct dentry *ddir;

	struct bpf_prog	*bpf_offloaded;
	u32 bpf_offloaded_id;

	u32 xdp_flags;
	int xdp_prog_mode;
	struct bpf_prog	*xdp_prog;

	u32 prog_id_gen;

	bool bpf_bind_accept;
	u32 bpf_bind_verifier_delay;
	struct dentry *ddir_bpf_bound_progs;
	struct list_head bpf_bound_progs;

	bool bpf_tc_accept;
	bool bpf_tc_no_skip_accept;
	bool bpf_xdpdrv_accept;
	bool bpf_xdpoffload_accept;
};

extern struct dentry *nsim_ddir;

int nsim_bpf_init(struct netdevsim *ns);
void nsim_bpf_uninit(struct netdevsim *ns);
int nsim_bpf(struct net_device *dev, struct netdev_bpf *bpf);
int nsim_bpf_disable_tc(struct netdevsim *ns);
int nsim_bpf_setup_tc_block_cb(enum tc_setup_type type,
			       void *type_data, void *cb_priv);
