/*
 * Copyright © 2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Authors: Gayatri Kammela <gayatri.kammela@intel.com>
 *          Jacob Pan <jacob.jun.pan@linux.intel.com>
 *
 */

#define pr_fmt(fmt)     "INTEL_IOMMU: " fmt
#include <linux/err.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/debugfs.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/intel-iommu.h>
#include <linux/intel-svm.h>
#include <linux/dmar.h>
#include <linux/spinlock.h>

#include "irq_remapping.h"

#define TOTAL_BUS_NR (256) /* full bus range 256 */
#define DEFINE_SHOW_ATTRIBUTE(__name)					\
static int __name ## _open(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, __name ## _show, inode->i_private);	\
}									\
static const struct file_operations __name ## _fops =			\
{									\
	.open		= __name ## _open,				\
	.read		= seq_read,					\
	.llseek		= seq_lseek,					\
	.release	= single_release,				\
	.owner		= THIS_MODULE,					\
}

static void ctx_tbl_entry_show(struct seq_file *m, void *unused,
			       struct intel_iommu *iommu, int bus, bool ext,
			       bool new_ext)
{
	struct context_entry *context;
	int ctx;
	unsigned long flags;

	seq_printf(m, "%s Context table entries for Bus: %d\n",
		   ext ? "Lower" : "", bus);
	seq_printf(m, "[entry]\tDID :B :D .F\tLow\t\tHigh\n");

	spin_lock_irqsave(&iommu->lock, flags);

	/* Publish either context entries or extended contenxt entries */
	for (ctx = 0; ctx < (ext ? 128 : 256); ctx++) {
		context = iommu_context_addr(iommu, bus, ctx, 0);
		if (!context)
			goto out;
		if (context_present(context)) {
			seq_printf(m, "[%d]\t%04x:%02x:%02x.%02x\t%llx\t%llx\n",
				   ctx, iommu->segment, bus, PCI_SLOT(ctx),
				   PCI_FUNC(ctx), context[0].lo, context[0].hi);
		}
	}
out:
	spin_unlock_irqrestore(&iommu->lock, flags);
}

static void root_tbl_entry_show(struct seq_file *m, void *unused,
				struct intel_iommu *iommu, u64 rtaddr_reg,
				bool ext, bool new_ext)
{
	int bus;

	seq_printf(m, "\nIOMMU %s: %2s Root Table Addr:%llx\n", iommu->name,
		   ext ? "Extended" : "", rtaddr_reg);
	/* Publish extended root table entries or root table entries here */
	for (bus = 0; bus < TOTAL_BUS_NR; bus++) {
		if (!iommu->root_entry[bus].lo)
			continue;

		seq_printf(m, "%s Root tbl entries:\n", ext ? "Extended" : "");
		seq_printf(m, "Bus %d L: %llx H: %llx\n", bus,
			   iommu->root_entry[bus].lo, iommu->root_entry[bus].hi
			  );

		ctx_tbl_entry_show(m, unused, iommu, bus, ext, new_ext);
	}
}

static int dmar_translation_struct_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	u64 rtaddr_reg;
	bool new_ext, ext;

	rcu_read_lock();
	for_each_active_iommu(iommu, drhd) {
		if (iommu) {
			/* Check if root table type is set */
			rtaddr_reg = dmar_readq(iommu->reg + DMAR_RTADDR_REG);
			ext        = !!(rtaddr_reg & DMA_RTADDR_RTT);
			new_ext    = !!ecap_ecs(iommu->ecap);
			if (new_ext != ext) {
				seq_printf(m, "IOMMU %s: invalid ecs\n",
					   iommu->name);
				rcu_read_unlock();
				return -EINVAL;
			}
			root_tbl_entry_show(m, unused, iommu, rtaddr_reg, ext,
					    new_ext);
		}
	}
	rcu_read_unlock();

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(dmar_translation_struct);

void __init intel_iommu_debugfs_init(void)
{
	struct dentry *iommu_debug_root;

	iommu_debug_root = debugfs_create_dir("intel_iommu", NULL);

	if (!iommu_debug_root) {
		pr_err("can't create debugfs dir\n");
		goto err;
	}

	if (!debugfs_create_file("dmar_translation_struct", S_IRUGO,
				 iommu_debug_root, NULL,
				 &dmar_translation_struct_fops)) {
		pr_err("Can't create dmar_translation_struct file\n");
		goto err;
	}

err:
	debugfs_remove_recursive(iommu_debug_root);

}
