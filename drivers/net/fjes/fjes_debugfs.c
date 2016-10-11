/*
 *  FUJITSU Extended Socket Network Device driver
 *  Copyright (c) 2015-2016 FUJITSU LIMITED
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
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 */

/* debugfs support for fjes driver */

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/platform_device.h>

#include "fjes.h"

static struct dentry *fjes_debug_root;

static ssize_t fjes_dbg_dbg_mode_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct fjes_adapter *adapter = file->private_data;
	struct fjes_hw *hw = &adapter->hw;
	char buf[64];
	int size;

	size = sprintf(buf, "%d\n", hw->debug_mode);

	return simple_read_from_buffer(ubuf, count, ppos, buf, size);
}

static ssize_t fjes_dbg_dbg_mode_write(struct file *file,
				       const char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct fjes_adapter *adapter = file->private_data;
	struct fjes_hw *hw = &adapter->hw;
	unsigned int value;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 10, &value);
	if (ret)
		return ret;

	if (value) {
		if (hw->debug_mode)
			return -EPERM;

		hw->debug_mode = value;

		/* enable debug mode */
		mutex_lock(&hw->hw_info.lock);
		ret = fjes_hw_start_debug(hw);
		mutex_unlock(&hw->hw_info.lock);

		if (ret) {
			hw->debug_mode = 0;
			return ret;
		}
	} else {
		if (!hw->debug_mode)
			return -EPERM;

		/* disable debug mode */
		mutex_lock(&hw->hw_info.lock);
		ret = fjes_hw_stop_debug(hw);
		mutex_unlock(&hw->hw_info.lock);

		if (ret)
			return ret;
	}

	(*ppos)++;

	return count;
}

static const struct file_operations fjes_dbg_dbg_mode_fops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= fjes_dbg_dbg_mode_read,
	.write		= fjes_dbg_dbg_mode_write,
};

static const char * const ep_status_string[] = {
	"unshared",
	"shared",
	"waiting",
	"complete",
};

static int fjes_dbg_status_show(struct seq_file *m, void *v)
{
	struct fjes_adapter *adapter = m->private;
	struct fjes_hw *hw = &adapter->hw;
	int max_epid = hw->max_epid;
	int my_epid = hw->my_epid;
	int epidx;

	seq_puts(m, "EPID\tSTATUS           SAME_ZONE        CONNECTED\n");
	for (epidx = 0; epidx < max_epid; epidx++) {
		if (epidx == my_epid) {
			seq_printf(m, "ep%d\t%-16c %-16c %-16c\n",
				   epidx, '-', '-', '-');
		} else {
			seq_printf(m, "ep%d\t%-16s %-16c %-16c\n",
				   epidx,
				   ep_status_string[fjes_hw_get_partner_ep_status(hw, epidx)],
				   fjes_hw_epid_is_same_zone(hw, epidx) ? 'Y' : 'N',
				   fjes_hw_epid_is_shared(hw->hw_info.share, epidx) ? 'Y' : 'N');
		}
	}

	return 0;
}

static int fjes_dbg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, fjes_dbg_status_show, inode->i_private);
}

static const struct file_operations fjes_dbg_status_fops = {
	.owner		= THIS_MODULE,
	.open		= fjes_dbg_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

void fjes_dbg_adapter_init(struct fjes_adapter *adapter)
{
	const char *name = dev_name(&adapter->plat_dev->dev);
	struct fjes_hw *hw = &adapter->hw;
	struct dentry *pfile;

	adapter->dbg_adapter = debugfs_create_dir(name, fjes_debug_root);
	if (!adapter->dbg_adapter) {
		dev_err(&adapter->plat_dev->dev,
			"debugfs entry for %s failed\n", name);
		return;
	}

	pfile = debugfs_create_file("debug_mode", 0644, adapter->dbg_adapter,
				    adapter, &fjes_dbg_dbg_mode_fops);
	if (!pfile)
		dev_err(&adapter->plat_dev->dev,
			"debugfs debug_mode for %s failed\n", name);

	adapter->blob.data = vzalloc(FJES_DEBUG_BUFFER_SIZE);
	adapter->blob.size = FJES_DEBUG_BUFFER_SIZE;
	if (adapter->blob.data) {
		pfile = debugfs_create_blob("debug_data", 0444,
					    adapter->dbg_adapter,
					    &adapter->blob);
		if (!pfile)
			dev_err(&adapter->plat_dev->dev,
				"debugfs debug_data for %s failed\n", name);

		hw->hw_info.trace = adapter->blob.data;
		hw->hw_info.trace_size = adapter->blob.size;
	} else {
		hw->hw_info.trace = NULL;
		hw->hw_info.trace_size = 0;
	}

	pfile = debugfs_create_file("status", 0444, adapter->dbg_adapter,
				    adapter, &fjes_dbg_status_fops);
	if (!pfile)
		dev_err(&adapter->plat_dev->dev,
			"debugfs status for %s failed\n", name);
}

void fjes_dbg_adapter_exit(struct fjes_adapter *adapter)
{
	struct fjes_hw *hw = &adapter->hw;

	debugfs_remove_recursive(adapter->dbg_adapter);
	adapter->dbg_adapter = NULL;

	if (hw->debug_mode) {
		/* disable debug mode */
		mutex_lock(&hw->hw_info.lock);
		fjes_hw_stop_debug(hw);
		mutex_unlock(&hw->hw_info.lock);
	}
	vfree(hw->hw_info.trace);
	hw->hw_info.trace = NULL;
	hw->hw_info.trace_size = 0;
	hw->debug_mode = 0;
}

void fjes_dbg_init(void)
{
	fjes_debug_root = debugfs_create_dir(fjes_driver_name, NULL);
	if (!fjes_debug_root)
		pr_info("init of debugfs failed\n");
}

void fjes_dbg_exit(void)
{
	debugfs_remove_recursive(fjes_debug_root);
	fjes_debug_root = NULL;
}

#endif /* CONFIG_DEBUG_FS */
