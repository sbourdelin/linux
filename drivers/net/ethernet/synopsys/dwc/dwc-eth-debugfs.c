/*
 * Synopsys DesignWare Ethernet Driver
 *
 * Copyright (c) 2014-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *     The Synopsys DWC ETHER XGMAC Software Driver and documentation
 *     (hereinafter "Software") is an unsupported proprietary work of Synopsys,
 *     Inc. unless otherwise expressly agreed to in writing between Synopsys
 *     and you.
 *
 *     The Software IS NOT an item of Licensed Software or Licensed Product
 *     under any End User Software License Agreement or Agreement for Licensed
 *     Product with Synopsys or any supplement thereto.  Permission is hereby
 *     granted, free of charge, to any person obtaining a copy of this software
 *     annotated with this license and the Software, to deal in the Software
 *     without restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *     of the Software, and to permit persons to whom the Software is furnished
 *     to do so, subject to the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS"
 *     BASIS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *     PARTICULAR PURPOSE ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS
 *     BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *     CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *     INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *     ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *     THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"

static ssize_t dwc_eth_common_read(char __user *buffer,
				   size_t count, loff_t *ppos,
				   unsigned int value)
{
	char *buf;
	ssize_t len;

	if (*ppos != 0)
		return 0;

	buf = kasprintf(GFP_KERNEL, "0x%08x\n", value);
	if (!buf)
		return -ENOMEM;

	if (count < strlen(buf)) {
		kfree(buf);
		return -ENOSPC;
	}

	len = simple_read_from_buffer(buffer, count, ppos, buf, strlen(buf));
	kfree(buf);

	return len;
}

static ssize_t dwc_eth_common_write(const char __user *buffer,
				    size_t count, loff_t *ppos,
				    unsigned int *value)
{
	char workarea[32];
	ssize_t len;
	int ret;

	if (*ppos != 0)
		return 0;

	if (count >= sizeof(workarea))
		return -ENOSPC;

	len = simple_write_to_buffer(workarea, sizeof(workarea) - 1, ppos,
				     buffer, count);
	if (len < 0)
		return len;

	workarea[len] = '\0';
	ret = kstrtouint(workarea, 16, value);
	if (ret)
		return -EIO;

	return len;
}

static ssize_t xlgmac_reg_addr_read(struct file *filp,
				    char __user *buffer,
				    size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_read(buffer, count, ppos,
				pdata->debugfs_xlgmac_reg);
}

static ssize_t xlgmac_reg_addr_write(struct file *filp,
				     const char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_write(buffer, count, ppos,
				&pdata->debugfs_xlgmac_reg);
}

static ssize_t xlgmac_reg_value_read(struct file *filp,
				     char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;
	unsigned int value;

	value = DWC_ETH_IOREAD(pdata, pdata->debugfs_xlgmac_reg);

	return dwc_eth_common_read(buffer, count, ppos, value);
}

static ssize_t xlgmac_reg_value_write(struct file *filp,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;
	unsigned int value;
	ssize_t len;

	len = dwc_eth_common_write(buffer, count, ppos, &value);
	if (len < 0)
		return len;

	DWC_ETH_IOWRITE(pdata, pdata->debugfs_xlgmac_reg, value);

	return len;
}

static const struct file_operations xlgmac_reg_addr_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read =  xlgmac_reg_addr_read,
	.write = xlgmac_reg_addr_write,
};

static const struct file_operations xlgmac_reg_value_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read =  xlgmac_reg_value_read,
	.write = xlgmac_reg_value_write,
};

static ssize_t xlgpcs_mmd_read(struct file *filp,
			       char __user *buffer,
			       size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_read(buffer, count, ppos,
				pdata->debugfs_xlgpcs_mmd);
}

static ssize_t xlgpcs_mmd_write(struct file *filp,
				const char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_write(buffer, count, ppos,
				&pdata->debugfs_xlgpcs_mmd);
}

static ssize_t xlgpcs_reg_addr_read(struct file *filp,
				    char __user *buffer,
				    size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_read(buffer, count, ppos,
				pdata->debugfs_xlgpcs_reg);
}

static ssize_t xlgpcs_reg_addr_write(struct file *filp,
				     const char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;

	return dwc_eth_common_write(buffer, count, ppos,
				&pdata->debugfs_xlgpcs_reg);
}

static ssize_t xlgpcs_reg_value_read(struct file *filp,
				     char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;
	unsigned int value;

	value = DWC_ETH_MDIO_READ(pdata, pdata->debugfs_xlgpcs_mmd,
				  pdata->debugfs_xlgpcs_reg);

	return dwc_eth_common_read(buffer, count, ppos, value);
}

static ssize_t xlgpcs_reg_value_write(struct file *filp,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct dwc_eth_pdata *pdata = filp->private_data;
	unsigned int value;
	ssize_t len;

	len = dwc_eth_common_write(buffer, count, ppos, &value);
	if (len < 0)
		return len;

	DWC_ETH_MDIO_WRITE(pdata, pdata->debugfs_xlgpcs_mmd,
			   pdata->debugfs_xlgpcs_reg, value);

	return len;
}

static const struct file_operations xlgpcs_mmd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read =  xlgpcs_mmd_read,
	.write = xlgpcs_mmd_write,
};

static const struct file_operations xlgpcs_reg_addr_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read =  xlgpcs_reg_addr_read,
	.write = xlgpcs_reg_addr_write,
};

static const struct file_operations xlgpcs_reg_value_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read =  xlgpcs_reg_value_read,
	.write = xlgpcs_reg_value_write,
};

void xlgmac_debugfs_init(struct dwc_eth_pdata *pdata)
{
	struct dentry *pfile;
	char *buf;

	TRACE("-->");

	/* Set defaults */
	pdata->debugfs_xlgmac_reg = 0;
	pdata->debugfs_xlgpcs_mmd = 1;
	pdata->debugfs_xlgpcs_reg = 0;

	buf = kasprintf(GFP_KERNEL, "dwc-%s", pdata->netdev->name);
	if (!buf)
		return;

	pdata->dwc_eth_debugfs = debugfs_create_dir(buf, NULL);
	if (!pdata->dwc_eth_debugfs) {
		netdev_err(pdata->netdev, "debugfs_create_dir failed\n");
		kfree(buf);
		return;
	}

	pfile = debugfs_create_file("xlgmac_register", 0600,
				    pdata->dwc_eth_debugfs, pdata,
				    &xlgmac_reg_addr_fops);
	if (!pfile)
		netdev_err(pdata->netdev, "debugfs_create_file failed\n");

	pfile = debugfs_create_file("xlgmac_register_value", 0600,
				    pdata->dwc_eth_debugfs, pdata,
				    &xlgmac_reg_value_fops);
	if (!pfile)
		netdev_err(pdata->netdev, "debugfs_create_file failed\n");

	pfile = debugfs_create_file("xlgpcs_mmd", 0600,
				    pdata->dwc_eth_debugfs, pdata,
				    &xlgpcs_mmd_fops);
	if (!pfile)
		netdev_err(pdata->netdev, "debugfs_create_file failed\n");

	pfile = debugfs_create_file("xlgpcs_register", 0600,
				    pdata->dwc_eth_debugfs, pdata,
				    &xlgpcs_reg_addr_fops);
	if (!pfile)
		netdev_err(pdata->netdev, "debugfs_create_file failed\n");

	pfile = debugfs_create_file("xlgpcs_register_value", 0600,
				    pdata->dwc_eth_debugfs, pdata,
				    &xlgpcs_reg_value_fops);
	if (!pfile)
		netdev_err(pdata->netdev, "debugfs_create_file failed\n");

	kfree(buf);

	TRACE("<--");
}

void xlgmac_debugfs_exit(struct dwc_eth_pdata *pdata)
{
	debugfs_remove_recursive(pdata->dwc_eth_debugfs);
	pdata->dwc_eth_debugfs = NULL;
}
