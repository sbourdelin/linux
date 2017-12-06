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

#ifdef CONFIG_INTEL_IOMMU_SVM
static void ext_ctx_tbl_entry_show(struct seq_file *m, void *unused,
				   struct intel_iommu *iommu, int bus, int ctx,
				   struct context_entry *context, bool new_ext)
{
	u64 ctx_lo;

	if (new_ext) {
		seq_printf(m, "Higher Context tbl entries for Bus: %d\n", bus);
		ctx_lo = context[0].lo;

		if (!(ctx_lo & CONTEXT_PASIDE)) {
			context[1].hi = (u64)virt_to_phys(
					iommu->pasid_state_table);
			context[1].lo = (u64)virt_to_phys(iommu->pasid_table) |
					intel_iommu_get_pts(iommu);
		}

		seq_printf(m, "[%d]\t%04x:%02x:%02x.%02x\t%llx\t%llx\n", ctx,
			   iommu->segment, bus, PCI_SLOT(ctx), PCI_FUNC(ctx),
			   context[1].lo, context[1].hi);
	}
}
#else /* CONFIG_INTEL_IOMMU_SVM */
static void ext_ctx_tbl_entry_show(struct seq_file *m, void *unused,
				   struct intel_iommu *iommu, int bus, int ctx,
				   struct context_entry *context, bool new_ext)
{
	return;
}
#endif /* CONFIG_INTEL_IOMMU_SVM */

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

			ext_ctx_tbl_entry_show(m, unused, iommu, bus, ctx,
					       context, new_ext);
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

static int iommu_regset_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	unsigned long long base;
	int i;
	struct regset {
		int offset;
		char *regs;
	};

	static const struct regset regstr[] = {{DMAR_VER_REG, "VER"},
					       {DMAR_CAP_REG, "CAP"},
					       {DMAR_ECAP_REG, "ECAP"},
					       {DMAR_GCMD_REG, "GCMD"},
					       {DMAR_GSTS_REG, "GSTS"},
					       {DMAR_RTADDR_REG, "RTADDR"},
					       {DMAR_CCMD_REG, "CCMD"},
					       {DMAR_FSTS_REG, "FSTS"},
					       {DMAR_FECTL_REG, "FECTL"},
					       {DMAR_FEDATA_REG, "FEDATA"},
					       {DMAR_FEADDR_REG, "FEADDR"},
					       {DMAR_FEUADDR_REG, "FEUADDR"},
					       {DMAR_AFLOG_REG, "AFLOG"},
					       {DMAR_PMEN_REG, "PMEN"},
					       {DMAR_PLMBASE_REG, "PLMBASE"},
					       {DMAR_PLMLIMIT_REG, "PLMLIMIT"},
					       {DMAR_PHMBASE_REG, "PHMBASE"},
					       {DMAR_PHMLIMIT_REG,  "PHMLIMIT"},
					       {DMAR_IQH_REG, "IQH"},
					       {DMAR_IQT_REG, "IQT"},
					       {DMAR_IQ_SHIFT, "IQ"},
					       {DMAR_IQA_REG, "IQA"},
					       {DMAR_ICS_REG, "ICS"},
					       {DMAR_IRTA_REG, "IRTA"},
					       {DMAR_PQH_REG, "PQH"},
					       {DMAR_PQT_REG, "PQT"},
					       {DMAR_PQA_REG, "PQA"},
					       {DMAR_PRS_REG, "PRS"},
					       {DMAR_PECTL_REG, "PECTL"},
					       {DMAR_PEDATA_REG, "PEDATA"},
					       {DMAR_PEADDR_REG, "PEADDR"},
					       {DMAR_PEUADDR_REG, "PEUADDR"},
					       {DMAR_MTRRCAP_REG, "MTRRCAP"},
					       {DMAR_MTRRDEF_REG, "MTRRDEF"} };

	rcu_read_lock();
	for_each_active_iommu(iommu, drhd) {
		if (iommu) {
			if (!drhd->reg_base_addr) {
				seq_printf(m, "IOMMU: Invalid base address\n");
				rcu_read_unlock();
				return -EINVAL;
			}

			base = drhd->reg_base_addr;
			seq_printf(m, "\nDMAR: %s: reg_base_addr %llx\n",
				   iommu->name, base);
			seq_printf(m, "Name\t\t\tOffset\t\tContents\n");
			/*
			 * Publish the contents of the 64-bit hardware registers
			 * by adding the offset to the pointer(virtual addr)
			 */
			for (i = 0 ; i < ARRAY_SIZE(regstr); i++) {
				seq_printf(m, "%-8s\t\t0x%02x\t\t0x%016lx\n",
					   regstr[i].regs, regstr[i].offset,
					   readq(iommu->reg + regstr[i].offset)
					  );
			}
		}
	}

	rcu_read_unlock();
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(iommu_regset);

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

	if (!debugfs_create_file("iommu_regset", S_IRUGO,
				 iommu_debug_root, NULL, &iommu_regset_fops)) {
		pr_err("Can't create iommu_regset file\n");
		goto err;
	}

	return;

err:
	debugfs_remove_recursive(iommu_debug_root);

}
