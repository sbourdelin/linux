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
static const struct ncsi_pkt_handler {
	unsigned char	type;
	const char	*name;
} ncsi_pkt_handlers[] = {
	{ NCSI_PKT_CMD_CIS,    "CIS"    },
	{ NCSI_PKT_CMD_SP,     "SP"     },
	{ NCSI_PKT_CMD_DP,     "DP"     },
	{ NCSI_PKT_CMD_EC,     "EC"     },
	{ NCSI_PKT_CMD_DC,     "DC"     },
	{ NCSI_PKT_CMD_RC,     "RC"     },
	{ NCSI_PKT_CMD_ECNT,   "ECNT"   },
	{ NCSI_PKT_CMD_DCNT,   "DCNT"   },
	{ NCSI_PKT_CMD_AE,     "AE"     },
	{ NCSI_PKT_CMD_SL,     "SL"     },
	{ NCSI_PKT_CMD_GLS,    "GLS"    },
	{ NCSI_PKT_CMD_SVF,    "SVF"    },
	{ NCSI_PKT_CMD_EV,     "EV"     },
	{ NCSI_PKT_CMD_DV,     "DV"     },
	{ NCSI_PKT_CMD_SMA,    "SMA"    },
	{ NCSI_PKT_CMD_EBF,    "EBF"    },
	{ NCSI_PKT_CMD_DBF,    "DBF"    },
	{ NCSI_PKT_CMD_EGMF,   "EGMF"   },
	{ NCSI_PKT_CMD_DGMF,   "DGMF"   },
	{ NCSI_PKT_CMD_SNFC,   "SNFC"   },
	{ NCSI_PKT_CMD_GVI,    "GVI"    },
	{ NCSI_PKT_CMD_GC,     "GC"     },
	{ NCSI_PKT_CMD_GP,     "GP"     },
	{ NCSI_PKT_CMD_GCPS,   "GCPS"   },
	{ NCSI_PKT_CMD_GNS,    "GNS"    },
	{ NCSI_PKT_CMD_GNPTS,  "GNPTS"  },
	{ NCSI_PKT_CMD_GPS,    "GPS"    },
	{ NCSI_PKT_CMD_OEM,    "OEM"    },
	{ NCSI_PKT_CMD_PLDM,   "PLDM"   },
	{ NCSI_PKT_CMD_GPUUID, "GPUUID" },
};

static bool ncsi_dev_stats_index(struct ncsi_dev_priv *ndp, loff_t pos,
				 unsigned long *type, unsigned long *index,
				 unsigned long *entries)
{
	const unsigned long ranges[3][2] = {
		{ 1,
		  ARRAY_SIZE(ndp->stats.cmd) - 1		},
		{ ranges[0][1] + 2,
		  ranges[1][0] + ARRAY_SIZE(ndp->stats.rsp) - 1	},
		{ ranges[1][1] + 2,
		  ranges[2][0] + ARRAY_SIZE(ndp->stats.aen) - 1 }
	};
	int i;

	for (i = 0; i < 3; i++) {
		if (pos == (ranges[i][0] - 1)) {
			*index = i;
			*entries = 0;
			return true;
		}

		if (pos >= ranges[i][0] && pos <= ranges[i][1]) {
			*type = i;
			*index = (pos - ranges[i][0]);
			*entries = NCSI_PKT_STAT_MAX;
			return true;
		}
	}

	return false;
}

static void *ncsi_dev_stats_data(struct ncsi_dev_priv *ndp, loff_t pos)
{
	unsigned long type, index, entries;
	bool valid;

	valid = ncsi_dev_stats_index(ndp, pos, &type, &index, &entries);
	if (!valid)
		return NULL;

	/* The bits in return value are assigned as below:
	 *
	 * Bit[7:0]:   Number of ulong entries
	 * Bit[23:8]:  Offset to that specific data entry
	 * Bit[30:24]: Type of packet statistics
	 * Bit[31]:    0x1 as valid flag.
	 */
	if (!entries)
		index += ((unsigned long)SEQ_START_TOKEN);
	else
		index = (1 << 31) | (type << 24) | (index << 8) | entries;

	return (void *)index;
}

static void *ncsi_dev_stats_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct ncsi_dev_priv *ndp = seq->private;
	void *data;

	data = ncsi_dev_stats_data(ndp, *pos);
	++(*pos);

	return data;
}

static void *ncsi_dev_stats_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct ncsi_dev_priv *ndp = seq->private;
	void *data;

	data = ncsi_dev_stats_data(ndp, *pos);
	++(*pos);
	return data;
}

static void ncsi_dev_stats_seq_stop(struct seq_file *seq, void *v)
{
}

static const char *ncsi_pkt_type_name(unsigned int type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ncsi_pkt_handlers); i++) {
		if (ncsi_pkt_handlers[i].type == type)
			return ncsi_pkt_handlers[i].name;
	}

	return "N/A";
}

static const char *ncsi_dev_stats_pkt_name(unsigned long type,
					   unsigned long index)
{
	switch (type) {
	case 0: /* Command */
	case 1: /* Response */
		return ncsi_pkt_type_name(index);
	case 2: /* AEN */
		switch (index) {
		case NCSI_PKT_AEN_LSC:
			return "LSC";
		case NCSI_PKT_AEN_CR:
			return "CR";
		case NCSI_PKT_AEN_HNCDSC:
			return "HNCDSC";
		default:
			return "N/A";
		}
	}

	return "N/A";
}

static int ncsi_dev_stats_seq_show(struct seq_file *seq, void *v)
{
	struct ncsi_dev_priv *ndp = seq->private;
	unsigned long type, index, entries, *data;
	const char *name;

	if (v >= SEQ_START_TOKEN && v <= (SEQ_START_TOKEN + 2)) {
		static const char * const header[] = { "CMD", "RSP", "AEN" };

		seq_puts(seq, "\n");
		seq_printf(seq, "%-12s %-8s %-8s %-8s\n",
			   header[v - SEQ_START_TOKEN],
			   "OK", "TIMEOUT", "ERROR");
		seq_puts(seq, "=======================================\n");
		return 0;
	}

	index = (unsigned long)v;
	type = (index >> 24) & 0x7F;
	entries = (index & 0xFF);
	index = ((index >> 8) & 0xFFFF);
	name = ncsi_dev_stats_pkt_name(type, index);
	if (WARN_ON_ONCE(entries != NCSI_PKT_STAT_MAX))
		return 0;

	switch (type) {
	case 0: /* Command */
		data = &ndp->stats.cmd[0][0];
		break;
	case 1: /* Response */
		data = &ndp->stats.rsp[0][0];
		break;
	case 2: /* AEN */
		data = &ndp->stats.aen[0][0];
		break;
	default:
		pr_warn("%s: Unsupported type %ld\n", __func__, type);
		return 0;
	}

	data += (index * NCSI_PKT_STAT_MAX);
	if (*data || *(data + 1) || *(data + 2)) {
		seq_printf(seq, "%-12s %-8ld %-8ld %-8ld\n",
			   name, *data, *(data + 1), *(data + 2));
	}

	return 0;
}

static const struct seq_operations ncsi_dev_stats_seq_ops = {
	.start = ncsi_dev_stats_seq_start,
	.next  = ncsi_dev_stats_seq_next,
	.stop  = ncsi_dev_stats_seq_stop,
	.show  = ncsi_dev_stats_seq_show,
};

static int ncsi_dev_stats_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *sf;
	int ret;

	ret = seq_open(file, &ncsi_dev_stats_seq_ops);
	if (!ret) {
		sf = file->private_data;
		sf->private = inode->i_private;
	}

	return ret;
}

static const struct file_operations ncsi_dev_stats_fops = {
	.owner   = THIS_MODULE,
	.open    = ncsi_dev_stats_seq_open,
	.read    = seq_read,
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

	ndp->stats.dentry = debugfs_create_file("stats", 0400, ndp->dentry,
						ndp, &ncsi_dev_stats_fops);
	if (!ndp->stats.dentry) {
		pr_debug("Failed to create debugfs file 'ncsi/%s/stats'\n",
			 netdev_name(ndp->ndev.dev));
		return -ENOMEM;
	}

	return 0;
}

void ncsi_dev_update_stats(struct ncsi_dev_priv *ndp,
			   int type, int subtype, int errno)
{
	if (errno >= NCSI_PKT_STAT_MAX)
		return;

	if (type == NCSI_PKT_AEN)
		ndp->stats.aen[subtype][errno]++;
	else if (type >= 0x80)
		ndp->stats.rsp[type - 0x80][errno]++;
	else
		ndp->stats.cmd[type][errno]++;
}

void ncsi_dev_release_debug(struct ncsi_dev_priv *ndp)
{
	debugfs_remove(ndp->stats.dentry);
	debugfs_remove(ndp->dentry);
}

int ncsi_package_init_debug(struct ncsi_package *np)
{
	struct ncsi_dev_priv *ndp = np->ndp;
	char name[4];

	if (!ndp->dentry)
		return -ENOENT;

	sprintf(name, "p%d", np->id);
	np->dentry = debugfs_create_dir(name, ndp->dentry);
	if (!np->dentry) {
		pr_debug("Failed to create debugfs directory ncsi/%s/%s\n",
			 netdev_name(ndp->ndev.dev), name);
		return -ENOMEM;
	}

	return 0;
}

void ncsi_package_release_debug(struct ncsi_package *np)
{
	debugfs_remove(np->dentry);
}

int ncsi_channel_init_debug(struct ncsi_channel *nc)
{
	struct ncsi_package *np = nc->package;
	struct ncsi_dev_priv *ndp = np->ndp;
	char name[3];

	if (!np->dentry)
		return -ENOENT;

	sprintf(name, "c%d", nc->id);
	nc->dentry = debugfs_create_dir(name, np->dentry);
	if (!nc->dentry) {
		pr_debug("Failed to create debugfs directory ncsi/%s/p%d/c%d\n",
			 netdev_name(ndp->ndev.dev), np->id, nc->id);
		return -ENOMEM;
	}

	return 0;
}

void ncsi_channel_release_debug(struct ncsi_channel *nc)
{
	debugfs_remove(nc->dentry);
}
