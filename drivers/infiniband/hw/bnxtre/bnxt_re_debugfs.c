/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2016, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: DebugFS specifics
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/netdevice.h>

#include <rdma/ib_verbs.h>
#include "bnxt_re_hsi.h"
#include "bnxt_ulp.h"
#include "bnxt_qplib_res.h"
#include "bnxt_qplib_sp.h"
#include "bnxt_qplib_fp.h"
#include "bnxt_qplib_rcfw.h"

#include "bnxt_re.h"
#include "bnxt_re_debugfs.h"

static struct dentry *bnxt_re_debugfs_root;
static struct dentry *bnxt_re_debugfs_info;

static ssize_t bnxt_re_debugfs_clear(struct file *fil, const char __user *u,
				     size_t size, loff_t *off)
{
	return size;
}

static int bnxt_re_debugfs_show(struct seq_file *s, void *unused)
{
	struct bnxt_re_dev *rdev;

	seq_puts(s, "bnxt_re debug info:\n");

	mutex_lock(&bnxt_re_dev_lock);
	list_for_each_entry(rdev, &bnxt_re_dev_list, list) {
		struct ctx_hw_stats *stats = rdev->qplib_ctx.stats.dma;

		seq_printf(s, "=====[ IBDEV %s ]=============================\n",
			   rdev->ibdev.name);
		if (rdev->netdev)
			seq_printf(s, "\tlink state: %s\n",
				   test_bit(__LINK_STATE_START,
					    &rdev->netdev->state) ?
				   (test_bit(__LINK_STATE_NOCARRIER,
					     &rdev->netdev->state) ?
				    "DOWN" : "UP") : "DOWN");
		seq_printf(s, "\tMax QP: 0x%x\n", rdev->dev_attr.max_qp);
		seq_printf(s, "\tMax SRQ: 0x%x\n", rdev->dev_attr.max_srq);
		seq_printf(s, "\tMax CQ: 0x%x\n", rdev->dev_attr.max_cq);
		seq_printf(s, "\tMax MR: 0x%x\n", rdev->dev_attr.max_mr);
		seq_printf(s, "\tMax MW: 0x%x\n", rdev->dev_attr.max_mw);

		seq_printf(s, "\tActive QP: %d\n",
			   atomic_read(&rdev->qp_count));
		seq_printf(s, "\tActive SRQ: %d\n",
			   atomic_read(&rdev->srq_count));
		seq_printf(s, "\tActive CQ: %d\n",
			   atomic_read(&rdev->cq_count));
		seq_printf(s, "\tActive MR: %d\n",
			   atomic_read(&rdev->mr_count));
		seq_printf(s, "\tActive MW: %d\n",
			   atomic_read(&rdev->mw_count));
		seq_printf(s, "\tRx Pkts: %lld\n",
			   stats ? stats->rx_ucast_pkts : 0);
		seq_printf(s, "\tRx Bytes: %lld\n",
			   stats ? stats->rx_ucast_bytes : 0);
		seq_printf(s, "\tTx Pkts: %lld\n",
			   stats ? stats->tx_ucast_pkts : 0);
		seq_printf(s, "\tTx Bytes: %lld\n",
			   stats ? stats->tx_ucast_bytes : 0);
		seq_printf(s, "\tRecoverable Errors: %lld\n",
			   stats ? stats->tx_bcast_pkts : 0);
		seq_puts(s, "\n");
	}
	mutex_unlock(&bnxt_re_dev_lock);
	return 0;
}

static int bnxt_re_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, bnxt_re_debugfs_show, NULL);
}

static int bnxt_re_debugfs_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static const struct file_operations bnxt_re_dbg_ops = {
	.owner		= THIS_MODULE,
	.open		= bnxt_re_debugfs_open,
	.read		= seq_read,
	.write		= bnxt_re_debugfs_clear,
	.llseek		= seq_lseek,
	.release	= bnxt_re_debugfs_release,
};

void bnxt_re_debugfs_remove(void)
{
	debugfs_remove_recursive(bnxt_re_debugfs_root);
	bnxt_re_debugfs_root = NULL;
}

void bnxt_re_debugfs_init(void)
{
	bnxt_re_debugfs_root = debugfs_create_dir(ROCE_DRV_MODULE_NAME, NULL);
	if (IS_ERR_OR_NULL(bnxt_re_debugfs_root)) {
		dev_dbg(NULL, "%s: Unable to create debugfs root directory ",
			ROCE_DRV_MODULE_NAME);
		dev_dbg(NULL, "with err 0x%lx", PTR_ERR(bnxt_re_debugfs_root));
		return;
	}
	bnxt_re_debugfs_info = debugfs_create_file("info", 0400,
						   bnxt_re_debugfs_root, NULL,
						   &bnxt_re_dbg_ops);
	if (IS_ERR_OR_NULL(bnxt_re_debugfs_info)) {
		dev_dbg(NULL, "%s: Unable to create debugfs info node ",
			ROCE_DRV_MODULE_NAME);
		dev_dbg(NULL, "with err 0x%lx", PTR_ERR(bnxt_re_debugfs_info));
		bnxt_re_debugfs_remove();
	}
}
