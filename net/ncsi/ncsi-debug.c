/*
 * Copyright Gavin Shan, IBM Corporation 2017.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/debugfs.h>
#include <linux/skbuff.h>

#include <net/ncsi.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "internal.h"
#include "ncsi-pkt.h"

static struct dentry *ncsi_dentry;
static const char *ncsi_pkt_type_name(unsigned int type);

void ncsi_dev_update_stats(struct ncsi_dev_priv *ndp,
			   int type, int subtype, int errno)
{
	unsigned long flags;

	if (errno >= ETHTOOL_NCSI_SW_STAT_MAX)
		return;

	spin_lock_irqsave(&ndp->lock, flags);

	if (type == NCSI_PKT_AEN) {
		if (subtype < 256)
			ndp->stats.aen[subtype][errno]++;
	} else if (type < 128) {
		ndp->stats.command[type][errno]++;
	} else if (type < 256) {
		ndp->stats.response[type - 128][errno]++;
	}

	spin_unlock_irqrestore(&ndp->lock, flags);
}

void ncsi_dev_reset_debug_pkt(struct ncsi_dev_priv *ndp,
			      struct sk_buff *skb, int errno)
{
	unsigned long flags;
	struct sk_buff *old;

	spin_lock_irqsave(&ndp->lock, flags);
	ndp->pkt.req = NCSI_PKT_REQ_FREE;
	ndp->pkt.errno = errno;

	old = ndp->pkt.rsp;
	ndp->pkt.rsp = skb;
	spin_unlock_irqrestore(&ndp->lock, flags);

	consume_skb(old);
}

static int ncsi_pkt_input_default(struct ncsi_dev_priv *ndp,
				  struct ncsi_cmd_arg *nca, char *buf)
{
	return 0;
}

static int ncsi_pkt_input_params(char *buf, int *outval, int count)
{
	int num, i;

	for (i = 0; i < count; i++, outval++) {
		if (sscanf(buf, "%x%n", outval, &num) != 1)
			return -EINVAL;

		if (buf[num] == ',')
			buf += (count + 1);
		else
			buf += count;
	}

	return 0;
}

static int ncsi_pkt_input_sp(struct ncsi_dev_priv *ndp,
			     struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* The hardware arbitration will be configured according
	 * to the NCSI's capability if it's not specified.
	 */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (!ret && param != 0 && param != 1)
		return -EINVAL;
	else if (ret)
		param = (ndp->flags & NCSI_DEV_HWA) ? 1 : 0;

	nca->bytes[0] = param;

	return 0;
}

static int ncsi_pkt_input_dc(struct ncsi_dev_priv *ndp,
			     struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* Allow link down will be disallowed if it's not specified */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (!ret && param != 0 && param != 1)
		return -EINVAL;
	else if (ret)
		param = 0;

	nca->bytes[0] = param;

	return 0;
}

static int ncsi_pkt_input_ae(struct ncsi_dev_priv *ndp,
			     struct ncsi_cmd_arg *nca, char *buf)
{
	int param[2], ret;

	/* MC ID and AE mode are mandatory */
	ret = ncsi_pkt_input_params(buf, param, 2);
	if (ret)
		return -EINVAL;

	nca->bytes[0] = param[0];
	nca->dwords[1] = param[1];

	return 0;
}

static int ncsi_pkt_input_sl(struct ncsi_dev_priv *ndp,
			     struct ncsi_cmd_arg *nca, char *buf)
{
	int param[2], ret;

	/* Link mode and OEM mode are mandatory */
	ret = ncsi_pkt_input_params(buf, param, 2);
	if (ret)
		return -EINVAL;

	nca->dwords[0] = param[0];
	nca->dwords[1] = param[1];

	return 0;
}

static int ncsi_pkt_input_svf(struct ncsi_dev_priv *ndp,
			      struct ncsi_cmd_arg *nca, char *buf)
{
	int param[3], ret;

	/* VLAN ID, table index and enable */
	ret = ncsi_pkt_input_params(buf, param, 3);
	if (ret)
		return -EINVAL;

	if (param[2] != 0 && param[2] != 1)
		return -EINVAL;

	nca->words[0] = param[0];
	nca->bytes[2] = param[1];
	nca->bytes[3] = param[2];

	return 0;
}

static int ncsi_pkt_input_ev(struct ncsi_dev_priv *ndp,
			     struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* VLAN filter mode */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (ret)
		return -EINVAL;

	nca->bytes[0] = param;

	return 0;
}

static int ncsi_pkt_input_sma(struct ncsi_dev_priv *ndp,
			      struct ncsi_cmd_arg *nca, char *buf)
{
	int param[8], ret;

	/* MAC address, MAC table index, Address type and operation */
	ret = ncsi_pkt_input_params(buf, param, 8);
	if (ret)
		return -EINVAL;

	if (param[7] & ~0x9)
		return -EINVAL;

	nca->bytes[0] = param[0];
	nca->bytes[1] = param[1];
	nca->bytes[2] = param[2];
	nca->bytes[3] = param[3];
	nca->bytes[4] = param[4];
	nca->bytes[5] = param[5];

	nca->bytes[6] = param[6];
	nca->bytes[7] = param[7];

	return 0;
}

static int ncsi_pkt_input_ebf(struct ncsi_dev_priv *ndp,
			      struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* Broadcast filter mode */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (ret)
		return -EINVAL;

	nca->dwords[0] = param;

	return 0;
}

static int ncsi_pkt_input_egmf(struct ncsi_dev_priv *ndp,
			       struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* Global multicast filter mode */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (ret)
		return -EINVAL;

	nca->dwords[0] = param;

	return 0;
}

static int ncsi_pkt_input_snfc(struct ncsi_dev_priv *ndp,
			       struct ncsi_cmd_arg *nca, char *buf)
{
	int param, ret;

	/* NCSI flow control mode */
	ret = ncsi_pkt_input_params(buf, &param, 1);
	if (ret)
		return -EINVAL;

	nca->bytes[0] = param;

	return 0;
}

static void ncsi_pkt_output_header(struct ncsi_dev_priv *ndp,
				   struct seq_file *seq,
				   struct ncsi_rsp_pkt_hdr *h)
{
	seq_printf(seq, "NCSI response [%s] packet received\n\n",
		   ncsi_pkt_type_name(h->common.type - 0x80));
	seq_printf(seq, "%02x %02x %02x %02x %02x %04x %04x %04x\n",
		   h->common.mc_id, h->common.revision, h->common.id,
		   h->common.type, h->common.channel, ntohs(h->common.length),
		   ntohs(h->code), ntohs(h->reason));
}

static int ncsi_pkt_output_default(struct ncsi_dev_priv *ndp,
				   struct seq_file *seq,
				   struct sk_buff *skb)
{
	struct ncsi_rsp_pkt_hdr *hdr;

	hdr = (struct ncsi_rsp_pkt_hdr *)skb_network_header(skb);
	ncsi_pkt_output_header(ndp, seq, hdr);

	return 0;
}

static int ncsi_pkt_output_gls(struct ncsi_dev_priv *ndp,
			       struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gls_pkt *gls;

	ncsi_pkt_output_default(ndp, seq, skb);

	gls = (struct ncsi_rsp_gls_pkt *)skb_network_header(skb);
	seq_printf(seq, "Status: %08x Other: %08x OEM: %08x\n",
		   ntohl(gls->status), ntohl(gls->other),
		   ntohl(gls->oem_status));

	return 0;
}

static int ncsi_pkt_output_gvi(struct ncsi_dev_priv *ndp,
			       struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gvi_pkt *gvi;

	ncsi_pkt_output_default(ndp, seq, skb);

	gvi = (struct ncsi_rsp_gvi_pkt *)skb_network_header(skb);
	seq_printf(seq, "NCSI Version: %08x Alpha2: %02x\n",
		   ntohl(gvi->ncsi_version), gvi->alpha2);
	seq_printf(seq, "Firmware: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x Version: %08x\n",
		   gvi->fw_name[0], gvi->fw_name[1], gvi->fw_name[2],
		   gvi->fw_name[3], gvi->fw_name[4], gvi->fw_name[5],
		   gvi->fw_name[6], gvi->fw_name[7], gvi->fw_name[8],
		   gvi->fw_name[9], gvi->fw_name[10], gvi->fw_name[11],
		   ntohl(gvi->fw_version));
	seq_printf(seq, "PCI: %04x %04x %04x %04x Manufacture ID: %08x\n",
		   ntohs(gvi->pci_ids[0]), ntohs(gvi->pci_ids[1]),
		   ntohs(gvi->pci_ids[2]), ntohs(gvi->pci_ids[3]),
		   ntohl(gvi->mf_id));

	return 0;
}

static int ncsi_pkt_output_gc(struct ncsi_dev_priv *ndp,
			      struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gc_pkt *gc;

	ncsi_pkt_output_default(ndp, seq, skb);

	gc = (struct ncsi_rsp_gc_pkt *)skb_network_header(skb);
	seq_printf(seq, "Cap: %08x BC: %08x MC: %08x Buf: %08x AEN: %08x\n",
		   ntohl(gc->cap), ntohl(gc->bc_cap), ntohl(gc->mc_cap),
		   ntohl(gc->buf_cap), ntohl(gc->aen_cap));
	seq_printf(seq, "VLAN: %02x Mixed: %02x MC: %02x UC: %02x\n",
		   gc->vlan_cnt, gc->mixed_cnt, gc->mc_cnt, gc->uc_cnt);
	seq_printf(seq, "VLAN Mode: %02x Channels: %02x\n",
		   gc->vlan_mode, gc->channel_cnt);

	return 0;
}

static int ncsi_pkt_output_gp(struct ncsi_dev_priv *ndp,
			      struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gp_pkt *gp;

	ncsi_pkt_output_default(ndp, seq, skb);

	gp = (struct ncsi_rsp_gp_pkt *)skb_network_header(skb);
	seq_printf(seq, "MAC: %02x %02x VLAN: %02x %04x\n",
		   gp->mac_cnt, gp->mac_enable, gp->vlan_cnt,
		   ntohs(gp->vlan_enable));
	seq_printf(seq, "Link: %08x BC: %08x Valid: %08x\n",
		   ntohl(gp->link_mode), ntohl(gp->bc_mode),
		   ntohl(gp->valid_modes));
	seq_printf(seq, "VLAN: %02x FC: %02x AEN: %08x\n",
		   gp->vlan_mode, gp->fc_mode, ntohl(gp->aen_mode));
	seq_printf(seq, "MAC: %02x %02x %02x %02x %02x %02x VLAN: %04x\n",
		   gp->mac[5], gp->mac[4], gp->mac[3], gp->mac[2],
		   gp->mac[1], gp->mac[0], ntohs(gp->vlan));

	return 0;
}

static int ncsi_pkt_output_gcps(struct ncsi_dev_priv *ndp,
				struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gcps_pkt *gcps;

	ncsi_pkt_output_default(ndp, seq, skb);

	gcps = (struct ncsi_rsp_gcps_pkt *)skb_network_header(skb);
	seq_printf(seq, "cnt_hi: %08x cnt_lo: %08x rx_bytes: %08x\n",
		   ntohl(gcps->cnt_hi), ntohl(gcps->cnt_lo),
		   ntohl(gcps->rx_bytes));
	seq_printf(seq, "tx_bytes: %08x rx_uc_pkts: %08x rx_mc_pkts: %08x\n",
		   ntohl(gcps->tx_bytes), ntohl(gcps->rx_uc_pkts),
		   ntohl(gcps->rx_mc_pkts));
	seq_printf(seq, "rx_bc_pkts: %08x tx_uc_pkts: %08x tx_mc_pkts: %08x\n",
		   ntohl(gcps->rx_bc_pkts), ntohl(gcps->tx_uc_pkts),
		   ntohl(gcps->tx_mc_pkts));
	seq_printf(seq, "tx_bc_pkts: %08x fcs_err: %08x align_err: %08x\n",
		   ntohl(gcps->tx_bc_pkts), ntohl(gcps->fcs_err),
		   ntohl(gcps->align_err));
	seq_printf(seq, "false_carrier: %08x runt_pkts: %08x jabber_pkts: %08x\n",
		   ntohl(gcps->false_carrier), ntohl(gcps->runt_pkts),
		   ntohl(gcps->jabber_pkts));
	seq_printf(seq, "rx_pause_xon: %08x rx_pause_xoff: %08x tx_pause_xon: %08x",
		   ntohl(gcps->rx_pause_xon), ntohl(gcps->rx_pause_xoff),
		   ntohl(gcps->tx_pause_xon));
	seq_printf(seq, "tx_pause_xoff: %08x tx_s_collision: %08x tx_m_collision: %08x\n",
		   ntohl(gcps->tx_pause_xoff), ntohl(gcps->tx_s_collision),
		   ntohl(gcps->tx_m_collision));
	seq_printf(seq, "l_collision: %08x e_collision: %08x rx_ctl_frames: %08x\n",
		   ntohl(gcps->l_collision), ntohl(gcps->e_collision),
		   ntohl(gcps->rx_ctl_frames));
	seq_printf(seq, "rx_64_frames: %08x rx_127_frames: %08x rx_255_frames: %08x\n",
		   ntohl(gcps->rx_64_frames), ntohl(gcps->rx_127_frames),
		   ntohl(gcps->rx_255_frames));
	seq_printf(seq, "rx_511_frames: %08x rx_1023_frames: %08x rx_1522_frames: %08x\n",
		   ntohl(gcps->rx_511_frames), ntohl(gcps->rx_1023_frames),
		   ntohl(gcps->rx_1522_frames));
	seq_printf(seq, "rx_9022_frames: %08x tx_64_frames: %08x tx_127_frames: %08x\n",
		   ntohl(gcps->rx_9022_frames), ntohl(gcps->tx_64_frames),
		   ntohl(gcps->tx_127_frames));
	seq_printf(seq, "tx_255_frames: %08x tx_511_frames: %08x tx_1023_frames: %08x\n",
		   ntohl(gcps->tx_255_frames), ntohl(gcps->tx_511_frames),
		   ntohl(gcps->tx_1023_frames));
	seq_printf(seq, "tx_1522_frames: %08x tx_9022_frames: %08x rx_valid_bytes: %08x\n",
		   ntohl(gcps->tx_1522_frames), ntohl(gcps->tx_9022_frames),
		   ntohl(gcps->rx_valid_bytes));
	seq_printf(seq, "rx_runt_pkts: %08x rx_jabber_pkts: %08x\n",
		   ntohl(gcps->rx_runt_pkts), ntohl(gcps->rx_jabber_pkts));

	return 0;
}

static int ncsi_pkt_output_gns(struct ncsi_dev_priv *ndp,
			       struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gns_pkt *gns;

	ncsi_pkt_output_default(ndp, seq, skb);

	gns = (struct ncsi_rsp_gns_pkt *)skb_network_header(skb);
	seq_printf(seq, "rx_cmds: %08x dropped_cmds: %08x cmd_type_errs: %08x\n",
		   ntohl(gns->rx_cmds), ntohl(gns->dropped_cmds),
		   ntohl(gns->cmd_type_errs));
	seq_printf(seq, "cmd_csum_errs: %08x rx_pkts: %08x tx_pkts: %08x\n",
		   ntohl(gns->cmd_csum_errs), ntohl(gns->rx_pkts),
		   ntohl(gns->tx_pkts));
	seq_printf(seq, "tx_aen_pkts: %08x\n",
		   ntohl(gns->tx_aen_pkts));

	return 0;
}

static int ncsi_pkt_output_gnpts(struct ncsi_dev_priv *ndp,
				 struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gnpts_pkt *gnpts;

	ncsi_pkt_output_default(ndp, seq, skb);

	gnpts = (struct ncsi_rsp_gnpts_pkt *)skb_network_header(skb);
	seq_printf(seq, "tx_pkts: %08x tx_dropped: %08x tx_channel_err: %08x\n",
		   ntohl(gnpts->tx_pkts), ntohl(gnpts->tx_dropped),
		   ntohl(gnpts->tx_channel_err));
	seq_printf(seq, "tx_us_err: %08x rx_pkts: %08x rx_dropped: %08x\n",
		   ntohl(gnpts->tx_us_err), ntohl(gnpts->rx_pkts),
		   ntohl(gnpts->rx_dropped));
	seq_printf(seq, "rx_channel_err: %08x rx_us_err: %08x rx_os_err: %08x\n",
		   ntohl(gnpts->rx_channel_err), ntohl(gnpts->rx_us_err),
		   ntohl(gnpts->rx_os_err));

	return 0;
}

static int ncsi_pkt_output_gps(struct ncsi_dev_priv *ndp,
			       struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gps_pkt *gps;

	ncsi_pkt_output_default(ndp, seq, skb);

	gps = (struct ncsi_rsp_gps_pkt *)skb_network_header(skb);
	seq_printf(seq, "Status: %08x\n", ntohl(gps->status));

	return 0;
}

static int ncsi_pkt_output_gpuuid(struct ncsi_dev_priv *ndp,
				  struct seq_file *seq, struct sk_buff *skb)
{
	struct ncsi_rsp_gpuuid_pkt *gpuuid;

	ncsi_pkt_output_default(ndp, seq, skb);

	gpuuid = (struct ncsi_rsp_gpuuid_pkt *)skb_network_header(skb);
	seq_printf(seq, "UUID: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		   gpuuid->uuid[15], gpuuid->uuid[14], gpuuid->uuid[13],
		   gpuuid->uuid[12], gpuuid->uuid[11], gpuuid->uuid[10],
		   gpuuid->uuid[9],  gpuuid->uuid[8]);
	seq_printf(seq, "      %02x %02x %02x %02x %02x %02x %02x %02x\n",
		   gpuuid->uuid[7], gpuuid->uuid[6], gpuuid->uuid[5],
		   gpuuid->uuid[4], gpuuid->uuid[3], gpuuid->uuid[2],
		   gpuuid->uuid[1], gpuuid->uuid[0]);

	return 0;
}

static const struct ncsi_pkt_handler {
	unsigned char	type;
	const char	*name;
	int		(*input)(struct ncsi_dev_priv *ndp,
				 struct ncsi_cmd_arg *nca, char *buf);
	int		(*output)(struct ncsi_dev_priv *ndp,
				  struct seq_file *seq, struct sk_buff *skb);
} ncsi_pkt_handlers[] = {
	{ NCSI_PKT_CMD_CIS,    "CIS",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_SP,     "SP",
	  ncsi_pkt_input_sp,      ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DP,     "DP",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_EC,     "EC",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DC,     "DC",
	  ncsi_pkt_input_dc,      ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_RC,     "RC",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_ECNT,   "ECNT",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DCNT,   "DCNT",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_AE,     "AE",
	  ncsi_pkt_input_ae,      ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_SL,     "SL",
	  ncsi_pkt_input_sl,      ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_GLS,    "GLS",
	  ncsi_pkt_input_default, ncsi_pkt_output_gls     },
	{ NCSI_PKT_CMD_SVF,    "SVF",
	  ncsi_pkt_input_svf,     ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_EV,     "EV",
	  ncsi_pkt_input_ev,      ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DV,     "DV",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_SMA,    "SMA",
	  ncsi_pkt_input_sma,     ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_EBF,    "EBF",
	  ncsi_pkt_input_ebf,     ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DBF,    "DBF",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_EGMF,   "EGMF",
	  ncsi_pkt_input_egmf,    ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_DGMF,   "DGMF",
	  ncsi_pkt_input_default, ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_SNFC,   "SNFC",
	  ncsi_pkt_input_snfc,    ncsi_pkt_output_default },
	{ NCSI_PKT_CMD_GVI,    "GVI",
	  ncsi_pkt_input_default, ncsi_pkt_output_gvi     },
	{ NCSI_PKT_CMD_GC,     "GC",
	  ncsi_pkt_input_default, ncsi_pkt_output_gc      },
	{ NCSI_PKT_CMD_GP,     "GP",
	  ncsi_pkt_input_default, ncsi_pkt_output_gp      },
	{ NCSI_PKT_CMD_GCPS,   "GCPS",
	  ncsi_pkt_input_default, ncsi_pkt_output_gcps    },
	{ NCSI_PKT_CMD_GNS,    "GNS",
	  ncsi_pkt_input_default, ncsi_pkt_output_gns     },
	{ NCSI_PKT_CMD_GNPTS,  "GNPTS",
	  ncsi_pkt_input_default, ncsi_pkt_output_gnpts   },
	{ NCSI_PKT_CMD_GPS,    "GPS",
	  ncsi_pkt_input_default, ncsi_pkt_output_gps     },
	{ NCSI_PKT_CMD_OEM,    "OEM",
	  NULL,                   NULL                    },
	{ NCSI_PKT_CMD_PLDM,   "PLDM",
	  NULL,                   NULL                    },
	{ NCSI_PKT_CMD_GPUUID, "GPUUID",
	  ncsi_pkt_input_default, ncsi_pkt_output_gpuuid  },
};

static const char *ncsi_pkt_type_name(unsigned int type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ncsi_pkt_handlers); i++) {
		if (ncsi_pkt_handlers[i].type == type)
			return ncsi_pkt_handlers[i].name;
	}

	return "N/A";
}

static int ncsi_dev_pkt_seq_show(struct seq_file *seq, void *v)
{
	struct ncsi_dev_priv *ndp = seq->private;
	const struct ncsi_pkt_handler *h = NULL;
	struct ncsi_pkt_hdr *hdr;
	struct sk_buff *skb;
	int i, errno;
	unsigned long flags;

	spin_lock_irqsave(&ndp->lock, flags);
	errno = ndp->pkt.errno;
	skb = ndp->pkt.rsp;
	ndp->pkt.rsp = NULL;
	spin_unlock_irqrestore(&ndp->lock, flags);
	ncsi_dev_reset_debug_pkt(ndp, NULL, 0);

	if (errno) {
		WARN_ON(skb);
		seq_printf(seq, "Error %d receiving response packet\n", errno);
		return 0;
	} else if (!skb) {
		seq_puts(seq, "No available response packet\n");
		return 0;
	}

	hdr = (struct ncsi_pkt_hdr *)skb_network_header(skb);
	for (i = 0; i < ARRAY_SIZE(ncsi_pkt_handlers); i++) {
		if (ncsi_pkt_handlers[i].type == (hdr->type - 0x80)) {
			h = &ncsi_pkt_handlers[i];
			break;
		}
	}

	if (!h || !h->output) {
		consume_skb(skb);
		return 0;
	}

	h->output(ndp, seq, skb);
	return 0;
}

static int ncsi_dev_pkt_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, ncsi_dev_pkt_seq_show, inode->i_private);
}

static ssize_t ncsi_dev_pkt_seq_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *pos)
{
	struct seq_file *seq = file->private_data;
	struct ncsi_dev_priv *ndp = seq->private;
	struct ncsi_cmd_arg nca;
	char buf[64], name[64], *pbuf;
	const struct ncsi_pkt_handler *h;
	int num, package, channel, i, ret;
	unsigned long flags;

	if (count >= sizeof(buf))
		return -EINVAL;

	/* Copy the buffer from user space. Currently we have 64 bytes as
	 * the length limitation. It should be enough as there are no bunch
	 * of parameters to be specified when sending NCSI command packet.
	 */
	memset(buf, 0, sizeof(buf));
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	/* Extract the specified command */
	memset(name, 0, sizeof(name));
	pbuf = strchr(buf, ',');
	if (!pbuf)
		return -EINVAL;
	memcpy(name, buf, pbuf - buf);
	pbuf++;

	/* Extract mandatory parameters: package and channel ID */
	memset(&nca, 0, sizeof(struct ncsi_cmd_arg));
	if (sscanf(pbuf, "%x,%x%n", &package, &channel, &num) != 2)
		return -EINVAL;
	if (package < 0 || package >= 8 ||
	    channel < 0 || channel > NCSI_RESERVED_CHANNEL)
		return -EINVAL;

	nca.package = package;
	nca.channel = channel;
	if (pbuf[num] == ',')
		pbuf += (count + 1);
	else
		pbuf += count;

	/* Search for handler */
	h = NULL;
	for (i = 0; i < ARRAY_SIZE(ncsi_pkt_handlers); i++) {
		if (!strcmp(ncsi_pkt_handlers[i].name, name)) {
			h = &ncsi_pkt_handlers[i];
			nca.type = h->type;
			break;
		}
	}

	if (!h || !h->input)
		return -ERANGE;

	/* Sort out additional parameters */
	nca.ndp = ndp;
	nca.req_flags = NCSI_REQ_FLAG_DEBUG;
	ret = h->input(ndp, &nca, pbuf);
	if (ret)
		return ret;

	/* This interface works in serialized fashion, meaning new command
	 * cannot be sent until previous one has been finalized.
	 */
	spin_lock_irqsave(&ndp->lock, flags);
	if (ndp->pkt.req != NCSI_PKT_REQ_FREE) {
		spin_unlock_irqrestore(&ndp->lock, flags);
		return -EBUSY;
	}

	ndp->pkt.req = NCSI_PKT_REQ_BUSY;
	spin_unlock_irqrestore(&ndp->lock, flags);

	ret = ncsi_xmit_cmd(&nca);
	if (ret) {
		spin_lock_irqsave(&ndp->lock, flags);
		ndp->pkt.req = NCSI_PKT_REQ_FREE;
		spin_unlock_irqrestore(&ndp->lock, flags);
		return ret;
	}

	return count;
}

static const struct file_operations ncsi_dev_pkt_fops = {
	.owner   = THIS_MODULE,
	.open    = ncsi_dev_pkt_seq_open,
	.read    = seq_read,
	.write   = ncsi_dev_pkt_seq_write,
	.llseek  = seq_lseek,
	.release = seq_release,
};

int ncsi_dev_init_debug(struct ncsi_dev_priv *ndp)
{
	if (WARN_ON_ONCE(ndp->dentry))
		return 0;

	if (!ncsi_dentry) {
		ncsi_dentry = debugfs_create_dir("ncsi", NULL);
		if (!ncsi_dentry) {
			pr_debug("Failed to create debugfs directory 'ncsi'\n");
			return -ENOMEM;
		}
	}

	ndp->dentry = debugfs_create_dir(netdev_name(ndp->ndev.dev),
					 ncsi_dentry);
	if (!ndp->dentry) {
		pr_debug("Failed to create debugfs directory 'ncsi/%s'\n",
			 netdev_name(ndp->ndev.dev));
		return -ENOMEM;
	}

	ndp->pkt.dentry = debugfs_create_file("pkt", 0600, ndp->dentry,
					      ndp, &ncsi_dev_pkt_fops);
	if (!ndp->pkt.dentry) {
		pr_debug("Failed to create debugfs file 'ncsi/%s/pkt'\n",
			 netdev_name(ndp->ndev.dev));
		return -ENOMEM;
	}

	return 0;
}

void ncsi_dev_release_debug(struct ncsi_dev_priv *ndp)
{
	debugfs_remove(ndp->pkt.dentry);
	debugfs_remove(ndp->dentry);
}
