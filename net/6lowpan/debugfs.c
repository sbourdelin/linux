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

#include <net/6lowpan.h>

#include "6lowpan_i.h"

#define LOWPAN_DEBUGFS_CTX_NUM_ARGS		11

static struct dentry *lowpan_debugfs;

static int lowpan_context_show(struct seq_file *file, void *offset)
{
	struct lowpan_iphc_ctx_table *t = file->private;
	int i;

	seq_printf(file, "%-2s %-43s %s\n", "ID", "ipv6-address/prefix-length",
		   "flags");

	spin_lock_bh(&t->lock);
	for (i = 0; i < LOWPAN_IPHC_CI_TABLE_SIZE; i++)
		seq_printf(file,
			   "%-2d %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x/%-3d %x\n",
			   t->table[i].id,
			   be16_to_cpu(t->table[i].pfx.s6_addr16[0]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[1]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[2]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[3]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[4]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[5]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[6]),
			   be16_to_cpu(t->table[i].pfx.s6_addr16[7]),
			   t->table[i].plen, t->table[i].flags);
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
	char buf[128] = {};
	struct seq_file *file = fp->private_data;
	struct lowpan_iphc_ctx_table *t = file->private;
	struct lowpan_iphc_ctx ctx;
	int status = count, n, id, i, plen;
	unsigned int addr[8], flags;

	if (copy_from_user(&buf, user_buf, min_t(size_t, sizeof(buf) - 1,
						 count))) {
		status = -EFAULT;
		goto out;
	}

	n = sscanf(buf, "%d %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x/%d %x",
		   &id, &addr[0], &addr[1], &addr[2], &addr[3], &addr[4],
		   &addr[5], &addr[6], &addr[7], &plen, &flags);
	if (n != LOWPAN_DEBUGFS_CTX_NUM_ARGS) {
		status = -EIO;
		goto out;
	}

	if (id > LOWPAN_IPHC_CI_TABLE_SIZE - 1 || plen > 128) {
		status = -EINVAL;
		goto out;
	}

	ctx.id = id;
	ctx.plen = plen;
	ctx.flags = flags & (LOWPAN_IPHC_CTX_FLAG_ACTIVE |
			     LOWPAN_IPHC_CTX_FLAG_C);

	for (i = 0; i < 8; i++)
		ctx.pfx.s6_addr16[i] = cpu_to_be16(addr[i] & 0xffff);

	spin_lock_bh(&t->lock);
	memcpy(&t->table[ctx.id], &ctx, sizeof(ctx));
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
	struct lowpan_priv *lpriv = lowpan_priv(dev);
	static struct dentry *dentry;

	/* creating the root */
	lpriv->iface_debugfs = debugfs_create_dir(dev->name, lowpan_debugfs);
	if (!lpriv->iface_debugfs)
		goto fail;

	dentry = debugfs_create_file("ctx_table", 0644, lpriv->iface_debugfs,
				     &lowpan_priv(dev)->ctx,
				     &lowpan_context_fops);
	if (!dentry)
		goto remove_root;

	return 0;

remove_root:
	lowpan_dev_debugfs_exit(dev);
fail:
	return -EINVAL;
}

void lowpan_dev_debugfs_exit(struct net_device *dev)
{
	debugfs_remove_recursive(lowpan_priv(dev)->iface_debugfs);
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
