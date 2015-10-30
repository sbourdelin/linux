/*
 * Qualcomm Technologies HIDMA DMA engine Management interface
 *
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dmaengine.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include "qcom_hidma.h"

#define MHICFG_OFFSET			0x10
#define QOS_N_OFFSET			0x300
#define CFG_OFFSET			0x400
#define HW_PARAM_OFFSET		0x408
#define MAX_BUS_REQ_LEN_OFFSET		0x41C
#define MAX_XACTIONS_OFFSET		0x420
#define SW_VERSION_OFFSET		0x424
#define CHRESET_TIMEOUUT_OFFSET	0x500
#define MEMSET_LIMIT_OFFSET		0x600
#define MHID_BUS_ERR0_OFFSET		0x1020
#define MHID_BUS_ERR1_OFFSET		0x1024
#define MHID_BUS_ERR_CLR_OFFSET	0x102C
#define EVT_BUS_ERR0_OFFSET		0x1030
#define EVT_BUS_ERR1_OFFSET		0x1034
#define EVT_BUS_ERR_CLR_OFFSET		0x103C
#define IDE_BUS_ERR0_OFFSET		0x1040
#define IDE_BUS_ERR1_OFFSET		0x1044
#define IDE_BUS_ERR2_OFFSET		0x1048
#define IDE_BUS_ERR_CLR_OFFSET		0x104C
#define ODE_BUS_ERR0_OFFSET		0x1050
#define ODE_BUS_ERR1_OFFSET		0x1054
#define ODE_BUS_ERR2_OFFSET		0x1058
#define ODE_BUS_ERR_CLR_OFFSET		0x105C
#define MSI_BUS_ERR0_OFFSET		0x1060
#define MSI_BUS_ERR_CLR_OFFSET		0x106C
#define TRE_ERR0_OFFSET		0x1070
#define TRE_ERR_CLR_OFFSET		0x107C
#define HW_EVENTS_CFG_OFFSET		0x1080

#define HW_EVENTS_CFG_MASK		0xFF
#define TRE_ERR_TRCHID_MASK		0xF
#define TRE_ERR_EVRIDX_MASK		0xFF
#define TRE_ERR_TYPE_MASK		0xFF
#define MSI_ERR_RESP_MASK		0xFF
#define MSI_ERR_TRCHID_MASK		0xFF
#define ODE_ERR_REQLEN_MASK		0xFFFF
#define ODE_ERR_RESP_MASK		0xFF
#define ODE_ERR_TRCHID_MASK		0xFF
#define IDE_ERR_REQLEN_MASK		0xFFFF
#define IDE_ERR_RESP_MASK		0xFF
#define IDE_ERR_TRCHID_MASK		0xFF
#define EVT_ERR_RESP_MASK		0xFF
#define EVT_ERR_TRCHID_MASK		0xFF
#define MHID_ERR_RESP_MASK		0xFF
#define MHID_ERR_TRCHID_MASK		0xFF
#define MEMSET_LIMIT_MASK		0x1F
#define MAX_WR_XACTIONS_MASK		0x1F
#define MAX_RD_XACTIONS_MASK		0x1F
#define MAX_JOBSIZE_MASK		0xFF
#define MAX_COIDX_MASK			0xFF
#define TREQ_CAPACITY_MASK		0xFF
#define WEIGHT_MASK			0x7F
#define TREQ_LIMIT_MASK		0x1FF
#define NR_CHANNEL_MASK		0xFFFF
#define MAX_BUS_REQ_LEN_MASK		0xFFFF
#define CHRESET_TIMEOUUT_MASK		0xFFFFF

#define TRE_ERR_TRCHID_BIT_POS		28
#define TRE_ERR_IEOB_BIT_POS		16
#define TRE_ERR_EVRIDX_BIT_POS		8
#define MSI_ERR_RESP_BIT_POS		8
#define ODE_ERR_REQLEN_BIT_POS		16
#define ODE_ERR_RESP_BIT_POS		8
#define IDE_ERR_REQLEN_BIT_POS		16
#define IDE_ERR_RESP_BIT_POS		8
#define EVT_ERR_RESP_BIT_POS		8
#define MHID_ERR_RESP_BIT_POS		8
#define MAX_WR_XACTIONS_BIT_POS	16
#define TREQ_CAPACITY_BIT_POS		8
#define MAX_JOB_SIZE_BIT_POS		16
#define NR_EV_CHANNEL_BIT_POS		16
#define MAX_BUS_WR_REQ_BIT_POS		16
#define WRR_BIT_POS			8
#define PRIORITY_BIT_POS		15
#define TREQ_LIMIT_BIT_POS		16
#define TREQ_LIMIT_EN_BIT_POS		23
#define STOP_BIT_POS			24

#define MODULE_NAME			"hidma-mgmt"
#define PREFIX				MODULE_NAME ": "
#define AUTOSUSPEND_TIMEOUT		2000

#define HIDMA_RUNTIME_GET(dmadev)				\
do {								\
	atomic_inc(&(dmadev)->pm_counter);			\
	TRC_PM(&(dmadev)->pdev->dev,				\
		"%s:%d pm_runtime_get %d\n", __func__, __LINE__,\
		atomic_read(&(dmadev)->pm_counter));		\
	pm_runtime_get_sync(&(dmadev)->pdev->dev);		\
} while (0)

#define HIDMA_RUNTIME_SET(dmadev)				\
do {								\
	atomic_dec(&(dmadev)->pm_counter);			\
	TRC_PM(&(dmadev)->pdev->dev,				\
		"%s:%d pm_runtime_put_autosuspend:%d\n",	\
		__func__, __LINE__,				\
		atomic_read(&(dmadev)->pm_counter));		\
	pm_runtime_mark_last_busy(&(dmadev)->pdev->dev);	\
	pm_runtime_put_autosuspend(&(dmadev)->pdev->dev);	\
} while (0)

struct qcom_hidma_mgmt_dev {
	u8 max_wr_xactions;
	u8 max_rd_xactions;
	u8 max_memset_limit;
	u16 max_write_request;
	u16 max_read_request;
	u16 nr_channels;
	u32 chreset_timeout;
	u32 sw_version;
	u8 *priority;
	u8 *weight;

	atomic_t	pm_counter;
	/* Hardware device constants */
	dma_addr_t dev_physaddr;
	void __iomem *dev_virtaddr;
	resource_size_t dev_addrsize;

	struct dentry	*debugfs;
	struct dentry	*info;
	struct dentry	*err;
	struct dentry	*mhid_errclr;
	struct dentry	*evt_errclr;
	struct dentry	*ide_errclr;
	struct dentry	*ode_errclr;
	struct dentry	*msi_errclr;
	struct dentry	*tre_errclr;
	struct dentry	*evt_ena;
	struct platform_device *pdev;
};

static unsigned int debug_pm;
module_param(debug_pm, uint, 0644);
MODULE_PARM_DESC(debug_pm,
		 "debug runtime power management transitions (default: 0)");

#define TRC_PM(...) do {			\
		if (debug_pm)			\
			dev_info(__VA_ARGS__);	\
	} while (0)


#if IS_ENABLED(CONFIG_DEBUG_FS)

#define HIDMA_SHOW(dma, name) \
		seq_printf(s, #name "=0x%x\n", dma->name)

#define HIDMA_READ_SHOW(dma, name, offset) \
	do { \
		u32 val; \
		val = readl(dma->dev_virtaddr + offset); \
		seq_printf(s, name "=0x%x\n", val); \
	} while (0)

/**
 * qcom_hidma_mgmt_info: display HIDMA device info
 *
 * Display the info for the current HIDMA device.
 */
static int qcom_hidma_mgmt_info(struct seq_file *s, void *unused)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = s->private;
	u32 val;
	int i;

	HIDMA_RUNTIME_GET(mgmtdev);
	HIDMA_SHOW(mgmtdev, sw_version);

	val = readl(mgmtdev->dev_virtaddr + CFG_OFFSET);
	seq_printf(s, "ENABLE=%d\n", val & 0x1);

	val = readl(mgmtdev->dev_virtaddr + CHRESET_TIMEOUUT_OFFSET);
	seq_printf(s, "reset_timeout=%d\n", val & CHRESET_TIMEOUUT_MASK);

	val = readl(mgmtdev->dev_virtaddr + MHICFG_OFFSET);
	seq_printf(s, "nr_event_channel=%d\n",
		(val >> NR_EV_CHANNEL_BIT_POS) & NR_CHANNEL_MASK);
	seq_printf(s, "nr_tr_channel=%d\n", (val & NR_CHANNEL_MASK));
	seq_printf(s, "nr_virt_tr_channel=%d\n", mgmtdev->nr_channels);
	seq_printf(s, "dev_virtaddr=%p\n", &mgmtdev->dev_virtaddr);
	seq_printf(s, "dev_physaddr=%pap\n", &mgmtdev->dev_physaddr);
	seq_printf(s, "dev_addrsize=%pap\n", &mgmtdev->dev_addrsize);

	val = readl(mgmtdev->dev_virtaddr + MEMSET_LIMIT_OFFSET);
	seq_printf(s, "MEMSET_LIMIT_OFFSET=%d\n", val & MEMSET_LIMIT_MASK);

	val = readl(mgmtdev->dev_virtaddr + HW_PARAM_OFFSET);
	seq_printf(s, "MAX_JOB_SIZE=%d\n",
		(val >> MAX_JOB_SIZE_BIT_POS) & MAX_JOBSIZE_MASK);
	seq_printf(s, "TREQ_CAPACITY=%d\n",
		(val >> TREQ_CAPACITY_BIT_POS) & TREQ_CAPACITY_MASK);
	seq_printf(s, "MAX_COIDX_DEPTH=%d\n", val & MAX_COIDX_MASK);

	val = readl(mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);
	seq_printf(s, "MAX_BUS_WR_REQ_LEN=%d\n",
		(val >> MAX_BUS_WR_REQ_BIT_POS) & MAX_BUS_REQ_LEN_MASK);
	seq_printf(s, "MAX_BUS_RD_REQ_LEN=%d\n", val & MAX_BUS_REQ_LEN_MASK);

	val = readl(mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);
	seq_printf(s, "MAX_WR_XACTIONS=%d\n",
		(val >> MAX_WR_XACTIONS_BIT_POS) & MAX_WR_XACTIONS_MASK);
	seq_printf(s, "MAX_RD_XACTIONS=%d\n", val & MAX_RD_XACTIONS_MASK);

	for (i = 0; i < mgmtdev->nr_channels; i++) {
		void __iomem *offset;

		offset = mgmtdev->dev_virtaddr + QOS_N_OFFSET + (4 * i);
		val = readl(offset);

		seq_printf(s, "CH#%d STOP=%d\n",
			i, (val & (1 << STOP_BIT_POS)) ? 1 : 0);
		seq_printf(s, "CH#%d TREQ LIMIT EN=%d\n", i,
			(val & (1 << TREQ_LIMIT_EN_BIT_POS)) ? 1 : 0);
		seq_printf(s, "CH#%d TREQ LIMIT=%d\n",
			i, (val >> TREQ_LIMIT_BIT_POS) & TREQ_LIMIT_MASK);
		seq_printf(s, "CH#%d priority=%d\n", i,
			(val & (1 << PRIORITY_BIT_POS)) ? 1 : 0);
		seq_printf(s, "CH#%d WRR=%d\n", i,
			(val >> WRR_BIT_POS) & WEIGHT_MASK);
		seq_printf(s, "CH#%d USE_DLA=%d\n", i, (val & 1) ? 1 : 0);
	}
	HIDMA_RUNTIME_SET(mgmtdev);

	return 0;
}

static int qcom_hidma_mgmt_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, qcom_hidma_mgmt_info, inode->i_private);
}

static const struct file_operations qcom_hidma_mgmt_fops = {
	.open = qcom_hidma_mgmt_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * qcom_hidma_mgmt_err: display HIDMA error info
 *
 * Display the error info for the current HIDMA device.
 */
static int qcom_hidma_mgmt_err(struct seq_file *s, void *unused)
{
	u32 val;
	struct qcom_hidma_mgmt_dev *mgmtdev = s->private;

	HIDMA_RUNTIME_GET(mgmtdev);
	val = readl(mgmtdev->dev_virtaddr + MHID_BUS_ERR0_OFFSET);
	seq_printf(s, "MHID TR_CHID=%d\n", val & MHID_ERR_TRCHID_MASK);
	seq_printf(s, "MHID RESP_ERROR=%d\n",
		(val >> MHID_ERR_RESP_BIT_POS) & MHID_ERR_RESP_MASK);
	HIDMA_READ_SHOW(mgmtdev, "MHID READ_PTR", MHID_BUS_ERR1_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + EVT_BUS_ERR0_OFFSET);
	seq_printf(s, "EVT TR_CHID=%d\n", val & EVT_ERR_TRCHID_MASK);
	seq_printf(s, "EVT RESP_ERROR=%d\n",
		(val >> EVT_ERR_RESP_BIT_POS) & EVT_ERR_RESP_MASK);
	HIDMA_READ_SHOW(mgmtdev, "EVT WRITE_PTR", EVT_BUS_ERR1_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + IDE_BUS_ERR0_OFFSET);
	seq_printf(s, "IDE TR_CHID=%d\n", val & IDE_ERR_TRCHID_MASK);
	seq_printf(s, "IDE RESP_ERROR=%d\n",
		(val >> IDE_ERR_RESP_BIT_POS) & IDE_ERR_RESP_MASK);
	seq_printf(s, "IDE REQ_LENGTH=%d\n",
		(val >> IDE_ERR_REQLEN_BIT_POS) & IDE_ERR_REQLEN_MASK);
	HIDMA_READ_SHOW(mgmtdev, "IDE ADDR_LSB", IDE_BUS_ERR1_OFFSET);
	HIDMA_READ_SHOW(mgmtdev, "IDE ADDR_MSB", IDE_BUS_ERR2_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + ODE_BUS_ERR0_OFFSET);
	seq_printf(s, "ODE TR_CHID=%d\n", val & ODE_ERR_TRCHID_MASK);
	seq_printf(s, "ODE RESP_ERROR=%d\n",
		(val >> ODE_ERR_RESP_BIT_POS) & ODE_ERR_RESP_MASK);
	seq_printf(s, "ODE REQ_LENGTH=%d\n",
		(val >> ODE_ERR_REQLEN_BIT_POS) & ODE_ERR_REQLEN_MASK);
	HIDMA_READ_SHOW(mgmtdev, "ODE ADDR_LSB", ODE_BUS_ERR1_OFFSET);
	HIDMA_READ_SHOW(mgmtdev, "ODE ADDR_MSB", ODE_BUS_ERR2_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + MSI_BUS_ERR0_OFFSET);
	seq_printf(s, "MSI TR_CHID=%d\n", val & MSI_ERR_TRCHID_MASK);
	seq_printf(s, "MSI RESP_ERROR=%d\n",
		(val >> MSI_ERR_RESP_BIT_POS) & MSI_ERR_RESP_MASK);

	val = readl(mgmtdev->dev_virtaddr + TRE_ERR0_OFFSET);
	seq_printf(s, "TRE TRE_TYPE=%d\n", val & TRE_ERR_TYPE_MASK);
	seq_printf(s, "TRE TRE_EVRIDX=%d\n",
		(val >> TRE_ERR_EVRIDX_BIT_POS) & TRE_ERR_EVRIDX_MASK);
	seq_printf(s, "TRE TRE_IEOB=%d\n",
		(val >> TRE_ERR_IEOB_BIT_POS) & 1);
	seq_printf(s, "TRE TRCHID=%d\n",
		(val >> TRE_ERR_TRCHID_BIT_POS) & TRE_ERR_TRCHID_MASK);

	HIDMA_READ_SHOW(mgmtdev, "HW_EVENTS_CFG_OFFSET",
			HW_EVENTS_CFG_OFFSET);

	HIDMA_RUNTIME_SET(mgmtdev);
	return 0;
}

static int qcom_hidma_mgmt_err_open(struct inode *inode, struct file *file)
{
	return single_open(file, qcom_hidma_mgmt_err, inode->i_private);
}

static const struct file_operations qcom_hidma_mgmt_err_fops = {
	.open = qcom_hidma_mgmt_err_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t qcom_hidma_mgmt_mhiderr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + MHID_BUS_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_mhiderr_clrfops = {
	.write = qcom_hidma_mgmt_mhiderr_clr,
};

static ssize_t qcom_hidma_mgmt_evterr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + EVT_BUS_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_evterr_clrfops = {
	.write = qcom_hidma_mgmt_evterr_clr,
};

static ssize_t qcom_hidma_mgmt_ideerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + IDE_BUS_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_ideerr_clrfops = {
	.write = qcom_hidma_mgmt_ideerr_clr,
};

static ssize_t qcom_hidma_mgmt_odeerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + ODE_BUS_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_odeerr_clrfops = {
	.write = qcom_hidma_mgmt_odeerr_clr,
};

static ssize_t qcom_hidma_mgmt_msierr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + MSI_BUS_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_msierr_clrfops = {
	.write = qcom_hidma_mgmt_msierr_clr,
};

static ssize_t qcom_hidma_mgmt_treerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(1, mgmtdev->dev_virtaddr + TRE_ERR_CLR_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_treerr_clrfops = {
	.write = qcom_hidma_mgmt_treerr_clr,
};

static ssize_t qcom_hidma_mgmt_evtena(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	char temp_buf[16+1];
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;
	u32 event;
	ssize_t ret;
	unsigned long val;

	temp_buf[16] = '\0';
	if (copy_from_user(temp_buf, user_buf, min_t(int, count, 16)))
		goto out;

	ret = kstrtoul(temp_buf, 16, &val);
	if (ret) {
		pr_warn(PREFIX "unknown event\n");
		goto out;
	}

	event = (u32)val & HW_EVENTS_CFG_MASK;

	HIDMA_RUNTIME_GET(mgmtdev);
	writel(event, mgmtdev->dev_virtaddr + HW_EVENTS_CFG_OFFSET);
	HIDMA_RUNTIME_SET(mgmtdev);
out:
	return count;
}

static const struct file_operations qcom_hidma_mgmt_evtena_fops = {
	.write = qcom_hidma_mgmt_evtena,
};

static void qcom_hidma_mgmt_debug_uninit(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	debugfs_remove(mgmtdev->evt_ena);
	debugfs_remove(mgmtdev->tre_errclr);
	debugfs_remove(mgmtdev->msi_errclr);
	debugfs_remove(mgmtdev->ode_errclr);
	debugfs_remove(mgmtdev->ide_errclr);
	debugfs_remove(mgmtdev->evt_errclr);
	debugfs_remove(mgmtdev->mhid_errclr);
	debugfs_remove(mgmtdev->err);
	debugfs_remove(mgmtdev->info);
	debugfs_remove(mgmtdev->debugfs);
}

static int qcom_hidma_mgmt_debug_init(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	int rc = 0;

	mgmtdev->debugfs = debugfs_create_dir(dev_name(&mgmtdev->pdev->dev),
						NULL);
	if (!mgmtdev->debugfs) {
		rc = -ENODEV;
		return rc;
	}

	mgmtdev->info = debugfs_create_file("info", S_IRUGO,
			mgmtdev->debugfs, mgmtdev, &qcom_hidma_mgmt_fops);
	if (!mgmtdev->info) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->err = debugfs_create_file("err", S_IRUGO,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_err_fops);
	if (!mgmtdev->err) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->mhid_errclr = debugfs_create_file("mhiderrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_mhiderr_clrfops);
	if (!mgmtdev->mhid_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->evt_errclr = debugfs_create_file("evterrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_evterr_clrfops);
	if (!mgmtdev->evt_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->ide_errclr = debugfs_create_file("ideerrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_ideerr_clrfops);
	if (!mgmtdev->ide_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->ode_errclr = debugfs_create_file("odeerrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_odeerr_clrfops);
	if (!mgmtdev->ode_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->msi_errclr = debugfs_create_file("msierrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_msierr_clrfops);
	if (!mgmtdev->msi_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->tre_errclr = debugfs_create_file("treerrclr", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_treerr_clrfops);
	if (!mgmtdev->tre_errclr) {
		rc = -ENOMEM;
		goto cleanup;
	}

	mgmtdev->evt_ena = debugfs_create_file("evtena", S_IWUSR,
			mgmtdev->debugfs, mgmtdev,
			&qcom_hidma_mgmt_evtena_fops);
	if (!mgmtdev->evt_ena) {
		rc = -ENOMEM;
		goto cleanup;
	}

	return 0;
cleanup:
	qcom_hidma_mgmt_debug_uninit(mgmtdev);
	return rc;
}
#else
static void qcom_hidma_mgmt_debug_uninit(struct qcom_hidma_mgmt_dev *mgmtdev)
{
}
static int qcom_hidma_mgmt_debug_init(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	return 0;
}
#endif

static irqreturn_t qcom_hidma_mgmt_irq_handler(int irq, void *arg)
{
	/* TODO: handle irq here */
	return IRQ_HANDLED;
}

static int qcom_hidma_mgmt_setup(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	u32 val;
	int i;

	val = readl(mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);

	if (mgmtdev->max_write_request) {
		val = val &
			~(MAX_BUS_REQ_LEN_MASK << MAX_BUS_WR_REQ_BIT_POS);
		val = val |
			(mgmtdev->max_write_request << MAX_BUS_WR_REQ_BIT_POS);
	}
	if (mgmtdev->max_read_request) {
		val = val & ~(MAX_BUS_REQ_LEN_MASK);
		val = val | (mgmtdev->max_read_request);
	}
	writel(val, mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);
	if (mgmtdev->max_wr_xactions) {
		val = val &
			~(MAX_WR_XACTIONS_MASK << MAX_WR_XACTIONS_BIT_POS);
		val = val |
			(mgmtdev->max_wr_xactions << MAX_WR_XACTIONS_BIT_POS);
	}
	if (mgmtdev->max_rd_xactions) {
		val = val & ~(MAX_RD_XACTIONS_MASK);
		val = val | (mgmtdev->max_rd_xactions);
	}
	writel(val, mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);
	mgmtdev->max_write_request =
		(val >> MAX_BUS_WR_REQ_BIT_POS) & MAX_BUS_REQ_LEN_MASK;
	mgmtdev->max_read_request = val & MAX_BUS_REQ_LEN_MASK;

	val = readl(mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);
	mgmtdev->max_wr_xactions =
		(val >> MAX_WR_XACTIONS_BIT_POS) & MAX_WR_XACTIONS_MASK;
	mgmtdev->max_rd_xactions = val & MAX_RD_XACTIONS_MASK;

	mgmtdev->sw_version = readl(mgmtdev->dev_virtaddr + SW_VERSION_OFFSET);

	for (i = 0; i < mgmtdev->nr_channels; i++) {
		val = readl(mgmtdev->dev_virtaddr + QOS_N_OFFSET + (4 * i));
		val = val & ~(1 << PRIORITY_BIT_POS);
		val = val |
			((mgmtdev->priority[i] & 0x1) << PRIORITY_BIT_POS);
		val = val & ~(WEIGHT_MASK << WRR_BIT_POS);
		val = val
			| ((mgmtdev->weight[i] & WEIGHT_MASK) << WRR_BIT_POS);
		writel(val, mgmtdev->dev_virtaddr + QOS_N_OFFSET + (4 * i));
	}

	if (mgmtdev->chreset_timeout > 0) {
		val = readl(mgmtdev->dev_virtaddr + CHRESET_TIMEOUUT_OFFSET);
		val = val & ~CHRESET_TIMEOUUT_MASK;
		val = val | (mgmtdev->chreset_timeout & CHRESET_TIMEOUUT_MASK);
		writel(val, mgmtdev->dev_virtaddr + CHRESET_TIMEOUUT_OFFSET);
	}
	val = readl(mgmtdev->dev_virtaddr + CHRESET_TIMEOUUT_OFFSET);
	mgmtdev->chreset_timeout = val & CHRESET_TIMEOUUT_MASK;

	if (mgmtdev->max_memset_limit > 0) {
		val = readl(mgmtdev->dev_virtaddr + MEMSET_LIMIT_OFFSET);
		val = val & ~MEMSET_LIMIT_MASK;
		val = val | (mgmtdev->max_memset_limit & MEMSET_LIMIT_MASK);
		writel(val, mgmtdev->dev_virtaddr + MEMSET_LIMIT_OFFSET);
	}
	val = readl(mgmtdev->dev_virtaddr + MEMSET_LIMIT_OFFSET);
	mgmtdev->max_memset_limit = val & MEMSET_LIMIT_MASK;

	val = readl(mgmtdev->dev_virtaddr + CFG_OFFSET);
	val = val | 1;
	writel(val, mgmtdev->dev_virtaddr + CFG_OFFSET);

	return 0;
}

static int qcom_hidma_mgmt_probe(struct platform_device *pdev)
{
	struct resource *dma_resource;
	int irq;
	int rc, i;
	struct qcom_hidma_mgmt_dev *mgmtdev;

	pm_runtime_set_autosuspend_delay(&pdev->dev, AUTOSUSPEND_TIMEOUT);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	dma_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!dma_resource) {
		dev_err(&pdev->dev, "No memory resources found\n");
		rc = -ENODEV;
		goto out;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "irq resources not found\n");
		rc = -ENODEV;
		goto out;
	}

	mgmtdev = devm_kzalloc(&pdev->dev, sizeof(*mgmtdev), GFP_KERNEL);
	if (!mgmtdev) {
		rc = -ENOMEM;
		goto out;
	}

	mgmtdev->pdev = pdev;
	HIDMA_RUNTIME_GET(mgmtdev);

	rc = devm_request_irq(&pdev->dev, irq, qcom_hidma_mgmt_irq_handler,
		IRQF_SHARED, "qcom-hidmamgmt", mgmtdev);
	if (rc) {
		dev_err(&pdev->dev, "irq registration failed: %d\n", irq);
		goto out;
	}

	mgmtdev->dev_physaddr = dma_resource->start;
	mgmtdev->dev_addrsize = resource_size(dma_resource);

	dev_dbg(&pdev->dev, "dev_physaddr:%pa\n", &mgmtdev->dev_physaddr);
	dev_dbg(&pdev->dev, "dev_addrsize:%pa\n", &mgmtdev->dev_addrsize);

	mgmtdev->dev_virtaddr = devm_ioremap_resource(&pdev->dev,
							dma_resource);
	if (IS_ERR(mgmtdev->dev_virtaddr)) {
		dev_err(&pdev->dev, "can't map i/o memory at %pa\n",
			&mgmtdev->dev_physaddr);
		rc = -ENOMEM;
		goto out;
	}

	if (device_property_read_u16(&pdev->dev, "nr-channels",
		&mgmtdev->nr_channels)) {
		dev_err(&pdev->dev, "number of channels missing\n");
		rc = -EINVAL;
		goto out;
	}

	device_property_read_u16(&pdev->dev, "max-write",
		&mgmtdev->max_write_request);
	if ((mgmtdev->max_write_request != 128) &&
		(mgmtdev->max_write_request != 256) &&
		(mgmtdev->max_write_request != 512) &&
		(mgmtdev->max_write_request != 1024)) {
		dev_err(&pdev->dev, "invalid write request %d\n",
			mgmtdev->max_write_request);
		rc = -EINVAL;
		goto out;
	}

	device_property_read_u16(&pdev->dev, "max-read",
		&mgmtdev->max_read_request);
	if ((mgmtdev->max_read_request != 128) &&
		(mgmtdev->max_read_request != 256) &&
		(mgmtdev->max_read_request != 512) &&
		(mgmtdev->max_read_request != 1024)) {
		dev_err(&pdev->dev, "invalid read request %d\n",
			mgmtdev->max_read_request);
		rc = -EINVAL;
		goto out;
	}

	device_property_read_u8(&pdev->dev, "max-wxactions",
		&mgmtdev->max_wr_xactions);

	device_property_read_u8(&pdev->dev, "max-rdactions",
		&mgmtdev->max_rd_xactions);

	device_property_read_u8(&pdev->dev, "max-memset-limit",
		&mgmtdev->max_memset_limit);

	/* needs to be at least one */
	if (mgmtdev->max_memset_limit == 0)
		mgmtdev->max_memset_limit = 1;

	mgmtdev->priority = devm_kcalloc(&pdev->dev,
		mgmtdev->nr_channels, sizeof(*mgmtdev->priority), GFP_KERNEL);
	if (!mgmtdev->priority) {
		rc = -ENOMEM;
		goto out;
	}

	mgmtdev->weight = devm_kcalloc(&pdev->dev,
		mgmtdev->nr_channels, sizeof(*mgmtdev->weight), GFP_KERNEL);
	if (!mgmtdev->weight) {
		rc = -ENOMEM;
		goto out;
	}

	for (i = 0; i < mgmtdev->nr_channels; i++) {
		char name[30];

		sprintf(name, "ch-priority-%d", i);
		device_property_read_u8(&pdev->dev, name,
			&mgmtdev->priority[i]);

		sprintf(name, "ch-weight-%d", i);
		device_property_read_u8(&pdev->dev, name,
			&mgmtdev->weight[i]);

		if (mgmtdev->weight[i] > 15) {
			dev_err(&pdev->dev, "max value of weight can be 15.\n");
			rc = -EINVAL;
			goto out;
		}

		/* weight needs to be at least one */
		if (mgmtdev->weight[i] == 0)
			mgmtdev->weight[i] = 1;
	}

	rc = qcom_hidma_mgmt_setup(mgmtdev);
	if (rc) {
		dev_err(&pdev->dev, "setup failed\n");
		goto out;
	}

	rc = qcom_hidma_mgmt_debug_init(mgmtdev);
	if (rc) {
		dev_err(&pdev->dev, "debugfs init failed\n");
		goto out;
	}

	dev_info(&pdev->dev,
		"HI-DMA engine management driver registration complete\n");
	platform_set_drvdata(pdev, mgmtdev);
	HIDMA_RUNTIME_SET(mgmtdev);
	return 0;
out:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_sync_suspend(&pdev->dev);
	return rc;
}

static int qcom_hidma_mgmt_remove(struct platform_device *pdev)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = platform_get_drvdata(pdev);

	HIDMA_RUNTIME_GET(mgmtdev);
	qcom_hidma_mgmt_debug_uninit(mgmtdev);
	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	dev_info(&pdev->dev, "HI-DMA engine management driver removed\n");
	return 0;
}

#if IS_ENABLED(CONFIG_ACPI)
static const struct acpi_device_id qcom_hidma_mgmt_acpi_ids[] = {
	{"QCOM8060"},
	{},
};
#endif

static const struct of_device_id qcom_hidma_mgmt_match[] = {
	{ .compatible = "qcom,hidma_mgmt", },
	{},
};
MODULE_DEVICE_TABLE(of, qcom_hidma_mgmt_match);

static struct platform_driver qcom_hidma_mgmt_driver = {
	.probe = qcom_hidma_mgmt_probe,
	.remove = qcom_hidma_mgmt_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(qcom_hidma_mgmt_match),
		.acpi_match_table = ACPI_PTR(qcom_hidma_mgmt_acpi_ids),
	},
};

static int __init qcom_hidma_mgmt_init(void)
{
	return platform_driver_register(&qcom_hidma_mgmt_driver);
}
device_initcall(qcom_hidma_mgmt_init);

static void __exit qcom_hidma_mgmt_exit(void)
{
	platform_driver_unregister(&qcom_hidma_mgmt_driver);
}
module_exit(qcom_hidma_mgmt_exit);
