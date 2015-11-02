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

#define MHICFG_OFFSET			0x10
#define QOS_N_OFFSET			0x300
#define CFG_OFFSET			0x400
#define HW_PARAM_OFFSET		0x408
#define MAX_BUS_REQ_LEN_OFFSET		0x41C
#define MAX_XACTIONS_OFFSET		0x420
#define SW_VERSION_OFFSET		0x424
#define CHRESET_TIMEOUT_OFFSET		0x418
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

#define AUTOSUSPEND_TIMEOUT		2000

struct qcom_hidma_mgmt_dev {
	u32 max_wr_xactions;
	u32 max_rd_xactions;
	u32 max_write_request;
	u32 max_read_request;
	u32 dma_channels;
	u32 chreset_timeout;
	u32 sw_version;
	u32 *priority;
	u32 *weight;

	/* Hardware device constants */
	void __iomem *dev_virtaddr;
	resource_size_t dev_addrsize;

	struct dentry	*debugfs;
	struct platform_device *pdev;
};

static unsigned int debug_pm;
module_param(debug_pm, uint, 0644);
MODULE_PARM_DESC(debug_pm,
		 "debug runtime power management transitions (default: 0)");

#if IS_ENABLED(CONFIG_DEBUG_FS)

static inline void HIDMA_READ_SHOW(struct seq_file *s,
		void __iomem *dev_virtaddr, char *name, int offset)
{
	u32 val;

	val = readl(dev_virtaddr + offset);
	seq_printf(s, "%s=0x%x\n", name, val);
}

/**
 * qcom_hidma_mgmt_info: display HIDMA device info
 *
 * Display the info for the current HIDMA device.
 */
static int qcom_hidma_mgmt_info(struct seq_file *s, void *unused)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = s->private;
	u32 val;
	u32 i;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	seq_printf(s, "sw_version=0x%x\n", mgmtdev->sw_version);

	val = readl(mgmtdev->dev_virtaddr + CFG_OFFSET);
	seq_printf(s, "ENABLE=%d\n", val & 0x1);

	val = readl(mgmtdev->dev_virtaddr + CHRESET_TIMEOUT_OFFSET);
	seq_printf(s, "reset_timeout=%d\n", val & CHRESET_TIMEOUUT_MASK);

	val = readl(mgmtdev->dev_virtaddr + MHICFG_OFFSET);
	seq_printf(s, "nr_event_channel=%d\n",
		(val >> NR_EV_CHANNEL_BIT_POS) & NR_CHANNEL_MASK);
	seq_printf(s, "nr_tr_channel=%d\n", (val & NR_CHANNEL_MASK));
	seq_printf(s, "dma_channels=%d\n", mgmtdev->dma_channels);
	seq_printf(s, "dev_addrsize=%pap\n", &mgmtdev->dev_addrsize);

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

	for (i = 0; i < mgmtdev->dma_channels; i++) {
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
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);

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

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	val = readl(mgmtdev->dev_virtaddr + MHID_BUS_ERR0_OFFSET);
	seq_printf(s, "MHID TR_CHID=%d\n", val & MHID_ERR_TRCHID_MASK);
	seq_printf(s, "MHID RESP_ERROR=%d\n",
		(val >> MHID_ERR_RESP_BIT_POS) & MHID_ERR_RESP_MASK);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "MHID READ_PTR",
		MHID_BUS_ERR1_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + EVT_BUS_ERR0_OFFSET);
	seq_printf(s, "EVT TR_CHID=%d\n", val & EVT_ERR_TRCHID_MASK);
	seq_printf(s, "EVT RESP_ERROR=%d\n",
		(val >> EVT_ERR_RESP_BIT_POS) & EVT_ERR_RESP_MASK);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "EVT WRITE_PTR",
		EVT_BUS_ERR1_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + IDE_BUS_ERR0_OFFSET);
	seq_printf(s, "IDE TR_CHID=%d\n", val & IDE_ERR_TRCHID_MASK);
	seq_printf(s, "IDE RESP_ERROR=%d\n",
		(val >> IDE_ERR_RESP_BIT_POS) & IDE_ERR_RESP_MASK);
	seq_printf(s, "IDE REQ_LENGTH=%d\n",
		(val >> IDE_ERR_REQLEN_BIT_POS) & IDE_ERR_REQLEN_MASK);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "IDE ADDR_LSB",
		IDE_BUS_ERR1_OFFSET);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "IDE ADDR_MSB",
		IDE_BUS_ERR2_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + ODE_BUS_ERR0_OFFSET);
	seq_printf(s, "ODE TR_CHID=%d\n", val & ODE_ERR_TRCHID_MASK);
	seq_printf(s, "ODE RESP_ERROR=%d\n",
		(val >> ODE_ERR_RESP_BIT_POS) & ODE_ERR_RESP_MASK);
	seq_printf(s, "ODE REQ_LENGTH=%d\n",
		(val >> ODE_ERR_REQLEN_BIT_POS) & ODE_ERR_REQLEN_MASK);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "ODE ADDR_LSB",
		ODE_BUS_ERR1_OFFSET);
	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "ODE ADDR_MSB",
		ODE_BUS_ERR2_OFFSET);

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

	HIDMA_READ_SHOW(s, mgmtdev->dev_virtaddr, "HW_EVENTS_CFG_OFFSET",
			HW_EVENTS_CFG_OFFSET);

	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
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

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + MHID_BUS_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_mhiderr_clrfops = {
	.write = qcom_hidma_mgmt_mhiderr_clr,
};

static ssize_t qcom_hidma_mgmt_evterr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + EVT_BUS_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_evterr_clrfops = {
	.write = qcom_hidma_mgmt_evterr_clr,
};

static ssize_t qcom_hidma_mgmt_ideerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + IDE_BUS_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_ideerr_clrfops = {
	.write = qcom_hidma_mgmt_ideerr_clr,
};

static ssize_t qcom_hidma_mgmt_odeerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + ODE_BUS_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_odeerr_clrfops = {
	.write = qcom_hidma_mgmt_odeerr_clr,
};

static ssize_t qcom_hidma_mgmt_msierr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + MSI_BUS_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return count;
}

static const struct file_operations qcom_hidma_mgmt_msierr_clrfops = {
	.write = qcom_hidma_mgmt_msierr_clr,
};

static ssize_t qcom_hidma_mgmt_treerr_clr(struct file *file,
	const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = file->f_inode->i_private;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(1, mgmtdev->dev_virtaddr + TRE_ERR_CLR_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
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
		dev_warn(&mgmtdev->pdev->dev, "unknown event\n");
		goto out;
	}

	event = (u32)val & HW_EVENTS_CFG_MASK;

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	writel(event, mgmtdev->dev_virtaddr + HW_EVENTS_CFG_OFFSET);
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
out:
	return count;
}

static const struct file_operations qcom_hidma_mgmt_evtena_fops = {
	.write = qcom_hidma_mgmt_evtena,
};

struct fileinfo {
	char *name;
	int mode;
	const struct file_operations *ops;
};

static struct fileinfo files[] = {
	{"info", S_IRUGO, &qcom_hidma_mgmt_fops},
	{"err", S_IRUGO,  &qcom_hidma_mgmt_err_fops},
	{"mhiderrclr", S_IWUSR, &qcom_hidma_mgmt_mhiderr_clrfops},
	{"evterrclr", S_IWUSR, &qcom_hidma_mgmt_evterr_clrfops},
	{"ideerrclr", S_IWUSR, &qcom_hidma_mgmt_ideerr_clrfops},
	{"odeerrclr", S_IWUSR, &qcom_hidma_mgmt_odeerr_clrfops},
	{"msierrclr", S_IWUSR, &qcom_hidma_mgmt_msierr_clrfops},
	{"treerrclr", S_IWUSR, &qcom_hidma_mgmt_treerr_clrfops},
	{"evtena", S_IWUSR, &qcom_hidma_mgmt_evtena_fops},
};

static void qcom_hidma_mgmt_debug_uninit(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	debugfs_remove_recursive(mgmtdev->debugfs);
}

static int qcom_hidma_mgmt_debug_init(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	int rc = 0;
	u32 i;
	struct dentry	*fs_entry;

	mgmtdev->debugfs = debugfs_create_dir(dev_name(&mgmtdev->pdev->dev),
						NULL);
	if (!mgmtdev->debugfs) {
		rc = -ENODEV;
		return rc;
	}

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		fs_entry = debugfs_create_file(files[i].name,
					files[i].mode, mgmtdev->debugfs,
					mgmtdev, files[i].ops);
		if (!fs_entry) {
			rc = -ENOMEM;
			goto cleanup;
		}
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

static int qcom_hidma_mgmt_setup(struct qcom_hidma_mgmt_dev *mgmtdev)
{
	u32 val;
	u32 i;

	val = readl(mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);
	val = val &
		~(MAX_BUS_REQ_LEN_MASK << MAX_BUS_WR_REQ_BIT_POS);
	val = val |
		(mgmtdev->max_write_request << MAX_BUS_WR_REQ_BIT_POS);
	val = val & ~(MAX_BUS_REQ_LEN_MASK);
	val = val | (mgmtdev->max_read_request);
	writel(val, mgmtdev->dev_virtaddr + MAX_BUS_REQ_LEN_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);
	val = val &
		~(MAX_WR_XACTIONS_MASK << MAX_WR_XACTIONS_BIT_POS);
	val = val |
		(mgmtdev->max_wr_xactions << MAX_WR_XACTIONS_BIT_POS);
	val = val & ~(MAX_RD_XACTIONS_MASK);
	val = val | (mgmtdev->max_rd_xactions);
	writel(val, mgmtdev->dev_virtaddr + MAX_XACTIONS_OFFSET);

	mgmtdev->sw_version = readl(mgmtdev->dev_virtaddr + SW_VERSION_OFFSET);

	for (i = 0; i < mgmtdev->dma_channels; i++) {
		val = readl(mgmtdev->dev_virtaddr + QOS_N_OFFSET + (4 * i));
		val = val & ~(1 << PRIORITY_BIT_POS);
		val = val |
			((mgmtdev->priority[i] & 0x1) << PRIORITY_BIT_POS);
		val = val & ~(WEIGHT_MASK << WRR_BIT_POS);
		val = val
			| ((mgmtdev->weight[i] & WEIGHT_MASK) << WRR_BIT_POS);
		writel(val, mgmtdev->dev_virtaddr + QOS_N_OFFSET + (4 * i));
	}

	val = readl(mgmtdev->dev_virtaddr + CHRESET_TIMEOUT_OFFSET);
	val = val & ~CHRESET_TIMEOUUT_MASK;
	val = val | (mgmtdev->chreset_timeout & CHRESET_TIMEOUUT_MASK);
	writel(val, mgmtdev->dev_virtaddr + CHRESET_TIMEOUT_OFFSET);

	val = readl(mgmtdev->dev_virtaddr + CFG_OFFSET);
	val = val | 1;
	writel(val, mgmtdev->dev_virtaddr + CFG_OFFSET);

	return 0;
}

static int qcom_hidma_mgmt_probe(struct platform_device *pdev)
{
	struct resource *dma_resource;
	int irq;
	int rc;
	u32 i;
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

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
	mgmtdev->dev_addrsize = resource_size(dma_resource);
	mgmtdev->dev_virtaddr = devm_ioremap_resource(&pdev->dev,
							dma_resource);
	if (IS_ERR(mgmtdev->dev_virtaddr)) {
		dev_err(&pdev->dev, "can't map i/o memory at %pa\n",
			&dma_resource->start);
		rc = -ENOMEM;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "dma-channels",
		&mgmtdev->dma_channels)) {
		dev_err(&pdev->dev, "number of channels missing\n");
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "channel-reset-timeout",
		&mgmtdev->chreset_timeout)) {
		dev_err(&pdev->dev, "channel reset timeout missing\n");
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "max-write-burst-bytes",
		&mgmtdev->max_write_request)) {
		dev_err(&pdev->dev, "max-write-burst-bytes missing\n");
		rc = -EINVAL;
		goto out;
	}
	if ((mgmtdev->max_write_request != 128) &&
		(mgmtdev->max_write_request != 256) &&
		(mgmtdev->max_write_request != 512) &&
		(mgmtdev->max_write_request != 1024)) {
		dev_err(&pdev->dev, "invalid write request %d\n",
			mgmtdev->max_write_request);
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "max-read-burst-bytes",
		&mgmtdev->max_read_request)) {
		dev_err(&pdev->dev, "max-read-burst-bytes missing\n");
		rc = -EINVAL;
		goto out;
	}

	if ((mgmtdev->max_read_request != 128) &&
		(mgmtdev->max_read_request != 256) &&
		(mgmtdev->max_read_request != 512) &&
		(mgmtdev->max_read_request != 1024)) {
		dev_err(&pdev->dev, "invalid read request %d\n",
			mgmtdev->max_read_request);
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "max-write-transactions",
		&mgmtdev->max_wr_xactions)) {
		dev_err(&pdev->dev, "max-write-transactions missing\n");
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32(&pdev->dev, "max-read-transactions",
		&mgmtdev->max_rd_xactions)) {
		dev_err(&pdev->dev, "max-read-transactions missing\n");
		rc = -EINVAL;
		goto out;
	}

	mgmtdev->priority = devm_kcalloc(&pdev->dev,
		mgmtdev->dma_channels, sizeof(*mgmtdev->priority), GFP_KERNEL);
	if (!mgmtdev->priority) {
		rc = -ENOMEM;
		goto out;
	}

	mgmtdev->weight = devm_kcalloc(&pdev->dev,
		mgmtdev->dma_channels, sizeof(*mgmtdev->weight), GFP_KERNEL);
	if (!mgmtdev->weight) {
		rc = -ENOMEM;
		goto out;
	}

	if (device_property_read_u32_array(&pdev->dev, "channel-priority",
				mgmtdev->priority, mgmtdev->dma_channels)) {
		dev_err(&pdev->dev, "channel-priority missing\n");
		rc = -EINVAL;
		goto out;
	}

	if (device_property_read_u32_array(&pdev->dev, "channel-weight",
				mgmtdev->weight, mgmtdev->dma_channels)) {
		dev_err(&pdev->dev, "channel-weight missing\n");
		rc = -EINVAL;
		goto out;
	}

	for (i = 0; i < mgmtdev->dma_channels; i++) {
		if (mgmtdev->weight[i] > 15) {
			dev_err(&pdev->dev,
				"max value of weight can be 15.\n");
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
	pm_runtime_mark_last_busy(&mgmtdev->pdev->dev);
	pm_runtime_put_autosuspend(&mgmtdev->pdev->dev);
	return 0;
out:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_sync_suspend(&pdev->dev);
	return rc;
}

static int qcom_hidma_mgmt_remove(struct platform_device *pdev)
{
	struct qcom_hidma_mgmt_dev *mgmtdev = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&mgmtdev->pdev->dev);
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
	{ .compatible = "qcom,hidma-mgmt-1.0", },
	{},
};
MODULE_DEVICE_TABLE(of, qcom_hidma_mgmt_match);

static struct platform_driver qcom_hidma_mgmt_driver = {
	.probe = qcom_hidma_mgmt_probe,
	.remove = qcom_hidma_mgmt_remove,
	.driver = {
		.name = "hidma-mgmt",
		.of_match_table = qcom_hidma_mgmt_match,
		.acpi_match_table = ACPI_PTR(qcom_hidma_mgmt_acpi_ids),
	},
};
module_platform_driver(qcom_hidma_mgmt_driver);
MODULE_LICENSE("GPL v2");
