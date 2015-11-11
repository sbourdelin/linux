/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors:
 * (C) 2015 Pengutronix, Alexander Aring <aar@pengutronix.de>
 * Copyright (c)  2015 Nordic Semiconductor. All Rights Reserved.
 */

#include <linux/debugfs.h>

#include <net/6lowpan.h>

#include "6lowpan_i.h"

static struct dentry *lowpan_debugfs;

static int lowpan_context_show(struct seq_file *file, void *offset)
{
	struct lowpan_iphc_ctx_table *t = file->private;
	int i;

	seq_printf(file, "%-2s %-43s %s\n", "ID", "ipv6-address/prefix-length",
		   "state");

	spin_lock_bh(&t->lock);
	for (i = 0; i < LOWPAN_IPHC_CI_TABLE_SIZE; i++) {
		seq_printf(file,
			   "%-2d %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x/%03d %d\n",
			   t->table[i].id,
			   be16_to_cpu(t->table[i].addr.s6_addr16[0]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[1]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[2]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[3]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[4]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[5]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[6]),
			   be16_to_cpu(t->table[i].addr.s6_addr16[7]),
			   t->table[i].prefix_len, t->table[i].state);
	}
	spin_unlock_bh(&t->lock);

	return 0;
}

static int lowpan_context_dbgfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, lowpan_context_show, inode->i_private);
}

static ssize_t lowpan_context_dbgfs_write(struct file *fp,
					  const char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	char buf[128] = { };
	struct seq_file *file = fp->private_data;
	struct lowpan_iphc_ctx_table *t = file->private;
	struct lowpan_iphc_ctx ctx;
	int status = count, n, id, state, i, prefix_len, ret;
	unsigned int addr[8];

	if (copy_from_user(&buf, user_buf, min_t(size_t, sizeof(buf) - 1,
						 count))) {
		status = -EFAULT;
		goto out;
	}

	n = sscanf(buf, "%d %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x/%d %d",
		   &id, &addr[0], &addr[1], &addr[2], &addr[3], &addr[4],
		   &addr[5], &addr[6], &addr[7], &prefix_len, &state);
	if (n != 11) {
		status = -EIO;
		goto out;
	}

	if (id > LOWPAN_IPHC_CI_TABLE_SIZE - 1 ||
	    state > LOWPAN_IPHC_CTX_STATE_MAX || prefix_len > 128) {
		status = -EINVAL;
		goto out;
	}

	ctx.id = id;
	ctx.state = state;
	ctx.prefix_len = prefix_len;

	for (i = 0; i < 8; i++)
		ctx.addr.s6_addr16[i] = cpu_to_be16(addr[i] & 0xffff);

	spin_lock_bh(&t->lock);
	ret = lowpan_ctx_update(t, &ctx);
	if (ret < 0)
		status = ret;
	spin_unlock_bh(&t->lock);

out:
	return status;
}

const struct file_operations lowpan_context_fops = {
	.open		= lowpan_context_dbgfs_open,
	.read		= seq_read,
	.write		= lowpan_context_dbgfs_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

int lowpan_dev_debugfs_init(struct net_device *dev)
{
	static struct dentry *iface, *dentry;

	/* creating the root */
	iface = debugfs_create_dir(dev->name, lowpan_debugfs);
	if (!iface)
		goto fail;

	dentry = debugfs_create_file("dci_table", 0664, iface,
				     &lowpan_priv(dev)->iphc_dci,
				     &lowpan_context_fops);
	if (!dentry)
		goto fail;

	dentry = debugfs_create_file("sci_table", 0664, iface,
				     &lowpan_priv(dev)->iphc_sci,
				     &lowpan_context_fops);
	if (!dentry)
		goto fail;

	dentry = debugfs_create_file("mcast_dci_table", 0664, iface,
				     &lowpan_priv(dev)->iphc_mcast_dci,
				     &lowpan_context_fops);
	if (!dentry)
		goto fail;

	return 0;

fail:
	lowpan_debugfs_exit();
	return -EINVAL;
}

int __init lowpan_debugfs_init(void)
{
	lowpan_debugfs = debugfs_create_dir("6lowpan", NULL);
	if (!lowpan_debugfs)
		return -EINVAL;

	return 0;
}

void lowpan_debugfs_exit(void)
{
	debugfs_remove_recursive(lowpan_debugfs);
}
