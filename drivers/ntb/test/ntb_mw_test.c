/*
 *   This file is provided under a GPLv2 license.  When using or
 *   redistributing this file, you may do so under that license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright (C) 2016 T-Platforms All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms and conditions of the GNU General Public License,
 *   version 2, as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but WITHOUT
 *   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *   more details.
 *
 *   You should have received a copy of the GNU General Public License along with
 *   this program; if not, one can be found <http://www.gnu.org/licenses/>.
 *
 *   The full GNU General Public License is included in this distribution in
 *   the file called "COPYING".
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * PCIe NTB memory windows test Linux driver
 *
 * Contact Information:
 * Serge Semin <fancer.lancer@gmail.com>, <Sergey.Semin@t-platforms.ru>
 */

/*
 *           NOTE of the NTB memory windows test driver design.
 * The driver implements the simple read/write algorithm. It allocates the
 * necessary inbound shared memory window by demand from the peer. Then it
 * sends the physical address of the memory back to the peer. The corresponding
 * inwndwN and outwndwN files are created at the DebugFS:ntb_mw_test/ntbA_/
 * directory. The inwndwN file can be used to read the data written by a peer.
 * The other outwndwN file is used to write data to the peer memory window.
 */

/* Note: You can load this module with either option 'dyndbg=+p' or define the
 * next preprocessor constant */
/*#define DEBUG*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>

#include <linux/ntb.h>

#define DRIVER_NAME		"ntb_mw_test"
#define DRIVER_DESCRIPTION	"PCIe NTB Memory Window Test Client"
#define DRIVER_VERSION		"1.0"
#define CACHE_NAME		"ntb_mw_cache"

MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("T-platforms");

/*
 * DebugFS directory to place the driver debug file
 */
static struct dentry *dbgfs_topdir;

/*
 * Inbound memory windows (locally allocated) structure
 * @dma_addr:	Address if the locally allocated memory and sent to the peer
 * @virt_addr:	Virtual address of that memory
 * @size:	Size of the allocated memory
 * @addr_align:	Address alignment
 * @size_align:	Size alignment
 * @size_max:	Maximum possible size of the window
 * @dbgfs_node:	DebugFS node to read data from peer
 * @ctx:	Pointer to the driver context
 */
struct mw_ctx;
struct inmw_wrap {
	dma_addr_t dma_addr;
	void *virt_addr;
	resource_size_t size;
	resource_size_t addr_align;
	resource_size_t size_align;
	resource_size_t size_max;
	struct dentry *dbgfs_node;
	struct mw_ctx *ctx;
};

/*
 * Outbound memory windows (remotely allocated) structure
 * @enabled:	Flag whether the window is enabled
 * @dma_addr:	DMA address of the remotely allocated memory window and
 *		retrieved from the peer
 * @phys_addr:	Physical address of the memory to locally map it (retrieved
 *		from the NTB subsystem, shortly it must be from BAR2 of IDT)
 * @virt_addr:	Virtual address of mapped IOMEM physical address
 * @size:	Size of the peer allocated memory
 * @addr_align:	Alignment of the DMA address allocated by the peer
 * @size_align:	Size alignment of the DMA address allocated by the peer
 * @size_max:	Maximum size of the peer allocated memory
 * @dbgfs_node:	DebugFS node to write data to peer
 * @ctx:        Pointer to the driver context
 */
struct outmw_wrap {
	bool enabled;
	dma_addr_t dma_addr;
	phys_addr_t phys_addr;
	void __iomem *virt_addr;
	resource_size_t size;
	resource_size_t addr_align;
	resource_size_t size_align;
	resource_size_t size_max;
	struct dentry *dbgfs_node;
	struct mw_ctx *ctx;
};

/*
 * Doorbells pingpong driver context
 * @ntb:	Pointer to the NTB device
 * @inmw_cnt:	Number of possible inbound memory windows
 * @outmw_cnt:	Number of possible outbound memory windows
 * @dbgfs_dir:	Handler of the DebugFS driver info-file
 */
struct mw_ctx {
	struct ntb_dev *ntb;
	int inmws_cnt;
	struct inmw_wrap *inmws;
	int outmws_cnt;
	struct outmw_wrap *outmws;
	struct dentry *dbgfs_dir;
};

/*
 * Enumeration of commands
 * @MW_GETADDRS:	Get the addresses of all memory windows peer allocated
 * @MW_DMAADDR:		DMA address of the memory window is sent within this msg
 * @MW_FREEADDRS:	Lock the memory windows shared from the local device
 * @MW_TYPEMASK:	Mask of the message type
 */
enum msg_type {
	MW_GETADDRS,
	MW_DMAADDR,
	MW_FREEADDRS,
	MW_TYPEMASK = 0xFFFFU
};

/*
* Helper method to get the type string name
*/
static inline char *mw_get_typename(enum msg_type type)
{
	switch (type) {
	case MW_GETADDRS:
		return "GETADDRS";
	case MW_DMAADDR:
		return "DMAADDR";
	case MW_FREEADDRS:
		return "FREEADDRS";
	default:
		break;
	}

	return "INVALID";
}

/*
 * Wrapper dev_err/dev_warn/dev_info/dev_dbg macros
 */
#define dev_err_mw(ctx, args...) \
	dev_err(&ctx->ntb->dev, ## args)
#define dev_warn_mw(ctx, args...) \
	dev_warn(&ctx->ntb->dev, ## args)
#define dev_info_mw(ctx, args...) \
	dev_info(&ctx->ntb->dev, ## args)
#define dev_dbg_mw(ctx, args...) \
	dev_dbg(&ctx->ntb->dev, ## args)

/*
 * Some common constant used in the driver for better readability:
 * @ON: Enable something
 * @OFF: Disable something
 * @SUCCESS: Success of a function execution
 * @MIN_MW_CNT:	Minimum memory windows count
 * @MAX_MW_CNT: Maximum memory windows count
 */
#define ON ((u32)0x1)
#define OFF ((u32)0x0)
#define SUCCESS 0
#define MIN_MW_CNT ((unsigned char)1)
#define MAX_MW_CNT ((unsigned char)255)

/*
 * Shared data converter to support the different CPU architectures
 */
#define to_sh32(data) \
	cpu_to_le32((data))
#define from_sh32(data) \
	le32_to_cpu((data))

/*
 * Module parameters:
 * @inmw_cnt:	Number of inbound memory windows [1; 255]
 * @outmw_cnt:	Number of outbound memory windows [1; 255]
 * If the specified value exceeds the maximum possible valiue, then it is
 * initialized with maximum one
 */
static unsigned char inmws_cnt = MAX_MW_CNT;
module_param(inmws_cnt, byte, 0000);
MODULE_PARM_DESC(inmws_cnt,
	"Inbound memory windows count. Those are the memory windows, which are "
	"locally allocated. Their address is sent to the remote host."
	" - Parameter can be set within [1; 255], where 255 means maximum possible"
	"   number of windows");

/*===========================================================================
 *                               Helper methods
 *===========================================================================*/

/*
 * Alter the passed driver paremeters
 */
static void mw_alter_params(struct mw_ctx *ctx)
{
	unsigned char inmws_cnt_bak = ctx->inmws_cnt;

	/* Clamp the inbound memory windows parameter */
	ctx->inmws_cnt = clamp(inmws_cnt,
		MIN_MW_CNT, (unsigned char)ctx->inmws_cnt);
	if (inmws_cnt_bak != ctx->inmws_cnt) {
		dev_warn_mw(ctx,
			"Inbound memory windows count is altered from "
			"%hhu to %hhu", inmws_cnt_bak, ctx->inmws_cnt);
	}

	dev_dbg_mw(ctx, "Memory windows test driver parameter is verified");
}

/*
 * Memory block IO write method
 */
static void iomem_write(void __iomem *dst, const void *src, size_t cnt)
{
	while (cnt--) {
		iowrite8(*(u8 *)src, dst);
		dst++;
		src++;
	}
}

/*
 * Memory block IO read method
 */
static void iomem_read(void __iomem *src, void *dst, size_t cnt)
{
	while (cnt--) {
		*(u8 *)dst = ioread8(src);
		dst++;
		src++;
	}
}

/*===========================================================================
 *                          Message command handlers
 *===========================================================================*/

/*
 * Send MW_GETADDRS command method
 */
static void mw_send_getaddrs_cmd(struct mw_ctx *ctx)
{
	struct ntb_msg msg;
	int sts;

	/* Clear the message structure */
	memset(&msg, 0, sizeof(msg));

	/* Set the message type only */
	msg.type = to_sh32(MW_GETADDRS);

	/* Send the message */
	sts = ntb_msg_post(ctx->ntb, &msg);
	if (SUCCESS != sts) {
		dev_err_mw(ctx, "Failed to send message to get outbound window "
			"addresses");
	}
}

/*
 * Send MW_FREEADDRS command method
 */
static void mw_send_freeaddrs_cmd(struct mw_ctx *ctx)
{
	struct ntb_msg msg;
	int sts;

	/* Clear the message structure */
	memset(&msg, 0, sizeof(msg));

	/* Set the message type only */
	msg.type = to_sh32(MW_FREEADDRS);

	/* Send the message */
	sts = ntb_msg_post(ctx->ntb, &msg);
	if (SUCCESS != sts) {
		dev_err_mw(ctx, "Failed to send a message to disable the peer "
			"outbound windows");
	}
}

/*
 * Callback method for response on the command MW_GETADDRS
 */
static void mw_send_inmw_addrs(struct mw_ctx *ctx)
{
	struct ntb_msg msg;
	struct inmw_wrap *inmw;
	int mwindx, sts;

	/* Clear the message structure */
	memset(&msg, 0, sizeof(msg));

	/* Walk through all the inbound memory windows and send the
	 * corresponding DMA address of the window */
	for (mwindx = 0; mwindx < ctx->inmws_cnt; mwindx++) {
		inmw = &ctx->inmws[mwindx];
		/* Set the type and the memory window index */
		msg.type = to_sh32(MW_DMAADDR | ((u32)mwindx << 16));

		/* First set the size of the memory window */
		msg.payload[0] = to_sh32(inmw->size);

		/* Set the Upper part of the memory window address */
#ifdef CONFIG_64BIT
		msg.payload[1] = to_sh32((u32)(inmw->dma_addr >> 32));
#else
		/* WARNING! NTB entpoints must either have the same architecture
		 * (x32 or x64) or use lower 4Gb for memory windows */
		msg.payload[1] = 0;
#endif /* !CONFIG_64BIT */
		/* Set the Lower part of the memory window address */
		msg.payload[2] = to_sh32((u32)(inmw->dma_addr));

		/* Send the message */
		sts = ntb_msg_post(ctx->ntb, &msg);
		if (SUCCESS != sts) {
			dev_err_mw(ctx,
				"Failed to send a message with window %d "
				"address", mwindx);
		}
	}
}

/*
 * Method to set the corresponding memory window and enable it
 */
static void mw_set_outmw_addr(struct mw_ctx *ctx, const struct ntb_msg *msg)
{
	struct outmw_wrap *outmw;
	int mwindx, sts;

	/* Read the memory windows index (it's the part of the message type) */
	mwindx = from_sh32(msg->type) >> 16;
	if (ctx->outmws_cnt <= mwindx) {
		dev_err_mw(ctx,
			"Retrieved invalid outbound memory window index %d",
			mwindx);
		return;
	}
	outmw = &ctx->outmws[mwindx];

	/* Read the memory window size and check whether it has proper size and
	 * alignment */
	outmw->size = from_sh32(msg->payload[0]);
	if (!IS_ALIGNED(outmw->size, outmw->size_align) ||
	    outmw->size_max < outmw->size) {
		dev_err_mw(ctx,
			"Retrieved invalid memory window %d size %u "
			"(max: %u, align: %u)", mwindx, (unsigned int)outmw->size,
			(unsigned int)outmw->size_max,
			(unsigned int)outmw->size_align);
		return;
	}

	/* Read the DMA address, where the second DWORD is the upper part and
	 * the third DWORD - lower */
	outmw->dma_addr = from_sh32(msg->payload[2]);
#ifdef CONFIG_64BIT
	outmw->dma_addr |= ((dma_addr_t)from_sh32(msg->payload[1]) << 32);
#endif /* CONFIG_64BIT */
	/* Check whether the retrieved address is properly aligned */
	if (!IS_ALIGNED(outmw->dma_addr, outmw->addr_align)) {
		dev_err_mw(ctx,
			"Outbound memory window address %p is not aligned "
			"within %u bytes", (void *)outmw->dma_addr,
			(unsigned int)outmw->addr_align);
		return;
	}

	/* Set the translation address of the outbound memory window */
	sts = ntb_mw_set_trans(ctx->ntb, mwindx, outmw->dma_addr, outmw->size);
	if (SUCCESS != sts) {
		dev_err_mw(ctx, "Failed to set the translated address %p of "
			"outbound memory window %d", (void *)outmw->dma_addr,
			mwindx);
		return;
	}

	/* Enable the memory window */
	outmw->enabled = true;

	dev_dbg_mw(ctx, "Outbound memory window %d is initialized with "
		"address 0x%p", mwindx, (void *)outmw->dma_addr);
}

/*
 * Lock all the outbound memory windows
 */
static void mw_lock_outmw_addrs(struct mw_ctx *ctx)
{
	int mwindx;

	/* Walk through all the memory windows and lock whem by falsing
	 * the flag */
	for (mwindx = 0; mwindx < ctx->outmws_cnt; mwindx++) {
		ctx->outmws[mwindx].enabled = false;
	}

	dev_dbg_mw(ctx, "Outbound memory windows are locked");
}

/*===========================================================================
 *                      Messages and link events handlers
 *===========================================================================*/

/*
 * Handle the retrieved message
 */
static void msg_recv_handler(struct mw_ctx *ctx, const struct ntb_msg *msg)
{
	enum msg_type type = from_sh32(msg->type) & MW_TYPEMASK;

	/* Check the message types */
	switch (type) {
	case MW_GETADDRS:
		mw_send_inmw_addrs(ctx);
		break;
	case MW_DMAADDR:
		mw_set_outmw_addr(ctx, msg);
		break;
	case MW_FREEADDRS:
		mw_lock_outmw_addrs(ctx);
		break;
	default:
		dev_err_mw(ctx, "Invalid message type retrieved %d", type);
		return;
	}

	dev_dbg_mw(ctx, "Message of type %s was received",
		mw_get_typename(type));
}

/*
 * Handler of the transmit errors
 */
static void msg_fail_handler(struct mw_ctx *ctx, const struct ntb_msg *msg)
{
	enum msg_type type = from_sh32(msg->type) & MW_TYPEMASK;

	/* Just print the error */
	dev_err_mw(ctx, "Failed to send the message of type %s",
		mw_get_typename(type));
}

/*
 * Handler of the succeeded transmits
 */
static void msg_sent_handler(struct mw_ctx *ctx, const struct ntb_msg *msg)
{
	enum msg_type type = from_sh32(msg->type) & MW_TYPEMASK;

	/* Just print the debug text and increment the succeeded msgs counter */
	dev_dbg_mw(ctx, "Message of type %s has been successfully sent",
		mw_get_typename(type));
}

/*
 * Message event handler
 */
static void msg_event_handler(void *data, enum NTB_MSG_EVENT ev,
			      struct ntb_msg *msg)
{
	struct mw_ctx *ctx = data;

	/* Call the corresponding event handler */
	switch (ev) {
	case NTB_MSG_NEW:
		msg_recv_handler(ctx, msg);
		break;
	case NTB_MSG_SENT:
		msg_sent_handler(ctx, msg);
		break;
	case NTB_MSG_FAIL:
		msg_fail_handler(ctx, msg);
		break;
	default:
		dev_err_mw(ctx, "Got invalid message event %d", ev);
		break;
	}
}

/*
 * Link Up/Down event handler
 */
static void link_event_handler(void *data)
{
	struct mw_ctx *ctx = data;
	int sts;

	/* If link is up then send the message with GETADDRS command, otherwise
	 * the outbound memory windows must be disabled */
	sts = ntb_link_is_up(ctx->ntb, NULL, NULL);
	if (ON == sts) {
		mw_send_getaddrs_cmd(ctx);
	} else /* if (OFF == sts) */ {
		mw_lock_outmw_addrs(ctx);
	}

	dev_dbg_mw(ctx, "Link %s event was retrieved",
		ON == sts ? "Up" : "Down");
}

/*===========================================================================
 *                      11. DebugFS callback functions
 *===========================================================================*/

static ssize_t mw_dbgfs_outmw_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp);

static ssize_t mw_dbgfs_outmw_write(struct file *filep, const char __user *ubuf,
				    size_t usize, loff_t *offp);

static ssize_t mw_dbgfs_outmw_cfg_read(struct file *filep, char __user *ubuf,
				       size_t usize, loff_t *offp);

static ssize_t mw_dbgfs_inmw_read(struct file *filep, char __user *ubuf,
				  size_t usize, loff_t *offp);

static ssize_t mw_dbgfs_inmw_write(struct file *filep, const char __user *ubuf,
				   size_t usize, loff_t *offp);

static ssize_t mw_dbgfs_inmw_cfg_read(struct file *filep, char __user *ubuf,
				      size_t usize, loff_t *offp);

/*
 * DebugFS outbound memory window node operations
 */
static const struct file_operations mw_dbgfs_outmw_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mw_dbgfs_outmw_read,
	.write = mw_dbgfs_outmw_write
};

/*
 * DebugFS outbound memory window configuration node operations
 */
static const struct file_operations mw_dbgfs_outmw_cfg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mw_dbgfs_outmw_cfg_read,
};

/*
 * DebugFS inbound memory window node operations
 */
static const struct file_operations mw_dbgfs_inmw_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mw_dbgfs_inmw_read,
	.write = mw_dbgfs_inmw_write
};

/*
 * DebugFS inbound memory window configuration node operations
 */
static const struct file_operations mw_dbgfs_inmw_cfg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mw_dbgfs_inmw_cfg_read,
};

/*
 * DebugFS read callback of outbound memory window node
 */
static ssize_t mw_dbgfs_outmw_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp)
{
	struct outmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *databuf;
	size_t datasize;
	ssize_t ret = 0;
	int sts;

	/* Check whether the link is up and the outbound window is enabled */
	sts = ntb_link_is_up(ctx->ntb, NULL, NULL);
	if (OFF == sts || !wrap->enabled) {
		dev_err_mw(ctx, "NTB link is %s, memory window status is %s",
			OFF == sts ? "Down" : "Up",
			wrap->enabled ? "enabled" : "disabled");
		return -ENODEV;
	}

	/* Read the first DWORD with the size of the message */
	datasize = readl(wrap->virt_addr);

	/* Check the read data size */
	if (wrap->size < datasize) {
		dev_err_mw(ctx, "Data size %u exceeds the memory window size %u",
			(unsigned int)datasize, (unsigned int)wrap->size);
		return -EINVAL;
	}

	/* Calculate the size of the output buffer */
	datasize = min(datasize, usize);

	/* If there is nothing to copy then just return from the function */
	if (0 == datasize) {
		return 0;
	}

	/* Allocate the buffer */
	databuf = kmalloc(datasize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_mw(ctx, "No memory to allocate the output buffer");
		return -ENOMEM;
	}

	/* Copy the data from the shared memory to the temporary buffer */
	/* NOTE memcpy_toio could be used instead, but it weirdly works so
	 * the traditional looping is used */
	iomem_read((wrap->virt_addr + 4), databuf, datasize);
	/*memcpy_fromio(databuf, wrap->virt_addr + 4, datasize);*/

	/* Copy the data to the output buffer */
	ret = simple_read_from_buffer(ubuf, usize, offp, databuf, datasize);

	/* Free the memory allocated for the buffer */
	kfree(databuf);

	return ret;
}

/*
 * DebugFS write callback of outbound memory window node
 */
static ssize_t mw_dbgfs_outmw_write(struct file *filep, const char __user *ubuf,
				    size_t usize, loff_t *offp)
{
	struct outmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *databuf;
	size_t datasize;
	ssize_t ret = 0;
	int sts;

	/* Check whether the link is up and the outbound window is enabled */
	sts = ntb_link_is_up(ctx->ntb, NULL, NULL);
	if (OFF == sts || !wrap->enabled) {
		dev_err_mw(ctx, "NTB link is %s, memory window status is %s",
			OFF == sts ? "Down" : "Up",
			wrap->enabled ? "enabled" : "disabled");
		return -ENODEV;
	}

	/* Calculate the data size */
	datasize = min(((size_t)wrap->size - 4), usize);

	/* Allocate the memory for sending data */
	databuf = kmalloc(datasize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_mw(ctx, "No memory to allocate the input data buffer");
		return -ENOMEM;
	}

	/* Copy the data to the output buffer */
	ret = simple_write_to_buffer(databuf, datasize, offp, ubuf, usize);
	if (0 > ret) {
		dev_err_mw(ctx, "Failed to copy the data from the User-space");
		kfree(databuf);
		return ret;
	}

	/* First DWORD is the data size */
	writel((u32)datasize, wrap->virt_addr);

	/* Copy the data to the memory window */
	/* NOTE memcpy_toio could be used instead, but it weirdly works so
	 * the traditional looping is used */
	iomem_write((wrap->virt_addr + 4), databuf, datasize);
	/*memcpy_toio((wrap->virt_addr + 4), databuf, datasize);*/

	/* Ensure that the data is fully copied out by setting the memory
	 * barrier */
	wmb();

	/* Free the memory allocated for the buffer */
	kfree(databuf);

	return ret;
}

/*
 * DebugFS read callback of outbound memory window configurations
 */
static ssize_t mw_dbgfs_outmw_cfg_read(struct file *filep, char __user *ubuf,
				       size_t usize, loff_t *offp)
{
	struct outmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;
	int id;

	/* Limit the buffer size */
	size = min_t(size_t, usize, 0x800U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		dev_dbg_mw(ctx,
			"Failed to allocated the memory for outbound memory "
			"window configuration");
		return -ENOMEM;
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tNTB Outbound Memory Window configuration:\n\n");

	/* Current driver state */
	off += scnprintf(strbuf + off, size - off,
		"Status\t\t\t- %s\n",
		wrap->enabled ? "enabled" : "disabled");
	off += scnprintf(strbuf + off, size - off,
		"DMA address\t\t- 0x%p\n", (void *)wrap->dma_addr);
	off += scnprintf(strbuf + off, size - off,
		"DMA address alignment\t- %lu\n",
		(unsigned long int)wrap->addr_align);
	off += scnprintf(strbuf + off, size - off,
		"Physycal map address\t- 0x%p\n", (void *)wrap->phys_addr);
	off += scnprintf(strbuf + off, size - off,
		"Virtual map address\t- 0x%p\n", (void *)wrap->virt_addr);
	off += scnprintf(strbuf + off, size - off,
		"Size of the window\t- %lu\n",
		(unsigned long int)wrap->size);
	off += scnprintf(strbuf + off, size - off,
		"Size alignment\t\t- %lu\n",
		(unsigned long int)wrap->size_align);
	off += scnprintf(strbuf + off, size - off,
		"Maximum size\t\t- %lu\n",
		(unsigned long int)wrap->size_max);
	/* Print raw data from the inbound window */
	off += scnprintf(strbuf + off, size - off,
		"Raw data (16 bytes)\t- ");
	for (id = 0; id < 16; id++) {
		off += scnprintf(strbuf + off, size - off,
			"%02hhx ", ioread8(wrap->virt_addr + id));
	}
	off += scnprintf(strbuf + off, size - off, "\n");


	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, usize, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * DebugFS read callback of inbound memory window node
 */
static ssize_t mw_dbgfs_inmw_read(struct file *filep, char __user *ubuf,
				  size_t usize, loff_t *offp)
{
	struct inmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *databuf;
	size_t datasize;
	ssize_t ret = 0;

	/* Read the first DWORD with the size of the data */
	datasize = le32_to_cpu(*(u32 *)wrap->virt_addr);

	/* Calculate the size of the output buffer */
	datasize = min(datasize, usize);

	/* If there is nothing to copy then just return from the function */
	if (0 == datasize) {
		return 0;
	}

	/* Allocate the buffer */
	databuf = kmalloc(datasize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_mw(ctx, "No memory to allocate the output buffer");
		return -ENOMEM;
	}

	/* Copy the data from the shared memory to the temporary buffer */
	memcpy(databuf, wrap->virt_addr + 4, datasize);

	/* Copy the data to the output buffer */
	ret = simple_read_from_buffer(ubuf, usize, offp, databuf, datasize);

	/* Free the memory allocated for the buffer */
	kfree(databuf);

	return ret;
}

/*
 * DebugFS write callback of inbound memory window node
 */
static ssize_t mw_dbgfs_inmw_write(struct file *filep, const char __user *ubuf,
				   size_t usize, loff_t *offp)
{
	struct inmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *databuf;
	size_t datasize;
	ssize_t ret = 0;

	/* Calculate the data size */
	datasize = min((size_t)wrap->size - 4, usize);

	/* Allocate the memory for sending data */
	databuf = kmalloc(datasize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_mw(ctx, "No memory to allocate the input data buffer");
		return -ENOMEM;
	}

	/* Copy the data to the output buffer */
	ret = simple_write_to_buffer(databuf, usize, offp, ubuf, usize);
	if (0 > ret) {
		dev_err_mw(ctx, "Failed to copy the data from the User-space");
		kfree(databuf);
		return ret;
	}

	/* First DWORD is the data size */
	*(u32 *)wrap->virt_addr = cpu_to_le32(datasize);

	/* Copy the data to the memory window */
	memcpy(wrap->virt_addr + 4, databuf, datasize);

	/* Free the memory allocated for the buffer */
	kfree(databuf);

	return datasize;
}

/*
 * DebugFS read callback of outbound memory window configurations
 */
static ssize_t mw_dbgfs_inmw_cfg_read(struct file *filep, char __user *ubuf,
				      size_t usize, loff_t *offp)
{
	struct inmw_wrap *wrap = filep->private_data;
	struct mw_ctx *ctx = wrap->ctx;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;
	int id;

	/* Limit the buffer size */
	size = min_t(size_t, usize, 0x800U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		dev_dbg_mw(ctx,
			"Failed to allocated the memory for inbound memory "
			"window configuration");
		return -ENOMEM;
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tNTB Inbound Memory Window configuration:\n\n");

	/* Current driver state */
	off += scnprintf(strbuf + off, size - off,
		"DMA address\t\t- 0x%p\n", (void *)wrap->dma_addr);
	off += scnprintf(strbuf + off, size - off,
		"DMA address alignment\t- %lu\n",
		(unsigned long int)wrap->addr_align);
	off += scnprintf(strbuf + off, size - off,
		"Virtual address\t\t- 0x%p\n", (void *)wrap->virt_addr);
	off += scnprintf(strbuf + off, size - off,
		"Size of the window\t- %lu\n",
		(unsigned long int)wrap->size);
	off += scnprintf(strbuf + off, size - off,
		"Size alignment\t\t- %lu\n",
		(unsigned long int)wrap->size_align);
	off += scnprintf(strbuf + off, size - off,
		"Maximum size\t\t- %lu\n",
		(unsigned long int)wrap->size_max);

	/* Print raw data from the inbound window */
	off += scnprintf(strbuf + off, size - off,
		"Raw data (16 bytes)\t- ");
	for (id = 0; id < 16; id++) {
		off += scnprintf(strbuf + off, size - off,
			"%02hhx ", *(char *)(wrap->virt_addr + id));
	}
	off += scnprintf(strbuf + off, size - off, "\n");


	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, usize, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * DebugFS initialization function
 */
#define NAMESIZE 16
static int mw_init_dbgfs(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct dentry *dbgfs_node;
	char nodename[NAMESIZE];
	const char *devname;
	int outmwindx, inmwindx, ret;

	/* If the top directory is not created then do nothing */
	if (IS_ERR_OR_NULL(dbgfs_topdir)) {
		dev_warn_mw(ctx,
			"Top DebugFS directory has not been created for "
			DRIVER_NAME);
		return PTR_ERR(dbgfs_topdir);
	}

	/* Retrieve the device name */
	devname = dev_name(&ntb->dev);

	/* Create the device related subdirectory */
	ctx->dbgfs_dir = debugfs_create_dir(devname, dbgfs_topdir);
	if (IS_ERR_OR_NULL(ctx->dbgfs_dir)) {
		dev_warn_mw(ctx,
			"Failed to create the DebugFS subdirectory %s",
			devname);
		return PTR_ERR(ctx->dbgfs_dir);
	}

	/* Walk through all the outbound memory windows creating the
	 * corresponding nodes */
	for (outmwindx = 0; outmwindx < ctx->outmws_cnt; outmwindx++) {
		/* Create the name of the read/write node */
		snprintf(nodename, NAMESIZE, "outmw%d", outmwindx);
		/* Create the data read/write node */
		dbgfs_node = debugfs_create_file(nodename, S_IRWXU,
			ctx->dbgfs_dir, &ctx->outmws[outmwindx],
			&mw_dbgfs_outmw_ops);
		if (IS_ERR(dbgfs_node)) {
			dev_err_mw(ctx, "Could not create DebugFS '%s' node",
			nodename);
			ret = PTR_ERR(dbgfs_node);
			goto err_rm_dir;
		}

		/* Create the name of the configuration node */
		snprintf(nodename, NAMESIZE, "outmwcfg%d", outmwindx);
		/* Create the data read/write node */
		dbgfs_node = debugfs_create_file(nodename, S_IRWXU,
			ctx->dbgfs_dir, &ctx->outmws[outmwindx],
			&mw_dbgfs_outmw_cfg_ops);
		if (IS_ERR(dbgfs_node)) {
			dev_err_mw(ctx, "Could not create DebugFS '%s' node",
			nodename);
			ret = PTR_ERR(dbgfs_node);
			goto err_rm_dir;
		}
	}

	/* Walk through all the inbound memory windows creating the
	 * corresponding nodes */
	for (inmwindx = 0; inmwindx < ctx->inmws_cnt; inmwindx++) {
		/* Create the name of the read/write node */
		snprintf(nodename, NAMESIZE, "inmw%d", inmwindx);
		/* Create the data read/write node */
		dbgfs_node = debugfs_create_file(nodename, S_IRWXU,
			ctx->dbgfs_dir, &ctx->inmws[inmwindx],
			&mw_dbgfs_inmw_ops);
		if (IS_ERR(dbgfs_node)) {
			dev_err_mw(ctx, "Could not create DebugFS '%s' node",
			nodename);
			ret = PTR_ERR(dbgfs_node);
			goto err_rm_dir;
		}

		/* Create the name of the configuration node */
		snprintf(nodename, NAMESIZE, "inmwcfg%d", inmwindx);
		/* Create the data read/write node */
		dbgfs_node = debugfs_create_file(nodename, S_IRWXU,
			ctx->dbgfs_dir, &ctx->inmws[inmwindx],
			&mw_dbgfs_inmw_cfg_ops);
		if (IS_ERR(dbgfs_node)) {
			dev_err_mw(ctx, "Could not create DebugFS '%s' node",
			nodename);
			ret = PTR_ERR(dbgfs_node);
			goto err_rm_dir;
		}
	}

	dev_dbg_mw(ctx, "NTB Memory Windows DebugFS top diretory is created "
		"for %s", devname);

	return SUCCESS;

err_rm_dir:
	debugfs_remove_recursive(ctx->dbgfs_dir);

	return ret;
}

/*
 * DebugFS deinitialization function
 */
static void mw_deinit_dbgfs(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Remove the DebugFS directory */
	debugfs_remove_recursive(ctx->dbgfs_dir);

	dev_dbg_mw(ctx, "Memory Windows DebugFS nodes %s/ are discarded",
		dev_name(&ntb->dev));
}

/*===========================================================================
 *                   NTB device/client driver initialization
 *===========================================================================*/

/*
 * NTB device events handlers
 */
static const struct ntb_ctx_ops mw_ops = {
	.link_event = link_event_handler,
	.msg_event = msg_event_handler
};

/*
 * Create the outbound memory windows
 */
static int mw_create_outmws(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct outmw_wrap *outmw;
	int ret, mwindx;

	/* Loop over all the outbound memory window descriptors initializing the
	 * corresponding fields */
	for (mwindx = 0; mwindx < ctx->outmws_cnt; mwindx++) {
		outmw = &ctx->outmws[mwindx];
		/* Outbound memory windows are disabled by default */
		outmw->enabled = false;

		/* Set the context */
		outmw->ctx = ctx;

		/* Retrieve the physical address of the memory to map */
		ret = ntb_mw_get_maprsc(ntb, mwindx, &outmw->phys_addr,
			&outmw->size);
		if (SUCCESS != ret) {
			dev_err_mw(ctx, "Failed to get map resources of "
				"outbound window %d", mwindx);
			mwindx--;
			goto err_unmap_rsc;
		}

		/* Map the memory window resources */
		outmw->virt_addr = ioremap_nocache(outmw->phys_addr, outmw->size);

		/* Retrieve the memory windows maximum size and alignments */
		ret = ntb_mw_get_align(ntb, mwindx, &outmw->addr_align,
			&outmw->size_align, &outmw->size_max);
		if (SUCCESS != ret) {
			dev_err_mw(ctx, "Failed to get alignment options of "
				"outbound window %d", mwindx);
			goto err_unmap_rsc;
		}
	}

	dev_dbg_mw(ctx, "Outbound memory windows are created");

	return SUCCESS;

err_unmap_rsc:
	for (; 0 <= mwindx; mwindx--) {
		outmw = &ctx->outmws[mwindx];
		iounmap(outmw->virt_addr);
	}

	return ret;
}

/*
 * Free the outbound memory windows
 */
static void mw_free_outmws(struct mw_ctx *ctx)
{
	struct outmw_wrap *outmw;
	int mwindx;

	/* Loop over all the outbound memory window descriptors and unmap the
	 * resources */
	for (mwindx = 0; mwindx < ctx->outmws_cnt; mwindx++) {
		outmw = &ctx->outmws[mwindx];

		/* Disable the memory window */
		outmw->enabled = false;

		/* Unmap the resource */
		iounmap(outmw->virt_addr);
	}

	dev_dbg_mw(ctx, "Outbound memory windows are freed");
}

/*
 * Create the inbound memory windows
 */
static int mw_create_inmws(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct inmw_wrap *inmw;
	int mwindx, ret = SUCCESS;

	/* Loop over all the inbound memory window descriptors initializing the
	 * corresponding fields */
	for (mwindx = 0; mwindx < ctx->inmws_cnt; mwindx++) {
		inmw = &ctx->inmws[mwindx];
		/* Set the context */
		inmw->ctx = ctx;
		/* Retrieve the memory windows maximum size and alignments */
		ret = ntb_peer_mw_get_align(ntb, mwindx, &inmw->addr_align,
			&inmw->size_align, &inmw->size_max);
		if (SUCCESS != ret) {
			dev_err_mw(ctx, "Failed to get alignment options of "
				"inbound window %d", mwindx);
			mwindx--;
			ret = -ENOMEM;
			goto err_free_dma_bufs;
		}
		/* Allocate all the maximum possible size */
		inmw->size = inmw->size_max;

		/* Allocate the cache coherent DMA memory windows */
		inmw->virt_addr = dma_zalloc_coherent(&ntb->dev, inmw->size,
			&inmw->dma_addr, GFP_KERNEL);
		if (IS_ERR_OR_NULL(inmw->virt_addr)) {
			dev_err_mw(ctx,
				"Failed to allocate the inbound buffer for %d",
				mwindx);
			mwindx--;
			ret = -ENOMEM;
			goto err_free_dma_bufs;
		}
		/* Make sure the allocated address is properly aligned */
		if (!IS_ALIGNED(inmw->dma_addr, inmw->addr_align)) {
			dev_err_mw(ctx, "DMA address %p of inbound mw %d isn't "
				"aligned with %lu", (void *)inmw->dma_addr, mwindx,
				(unsigned long int)inmw->addr_align);
			ret = -EINVAL;
			goto err_free_dma_bufs;
		}
	}

	dev_dbg_mw(ctx, "Inbound memory windows are created");

	return SUCCESS;

err_free_dma_bufs:
	for (; 0 <= mwindx; mwindx--) {
		inmw = &ctx->inmws[mwindx];
		dma_free_coherent(&ntb->dev, inmw->size, inmw->virt_addr,
			inmw->dma_addr);
	}

	return ret;
}

/*
 * Free the inbound memory windows
 */
static void mw_free_inmws(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct inmw_wrap *inmw;
	int mwindx;

	/* Loop over all the inbound memory window descriptors and free
	 * the allocated memory */
	for (mwindx = 0; mwindx < ctx->inmws_cnt; mwindx++) {
		inmw = &ctx->inmws[mwindx];

		/* Free the cache coherent DMA memory window */
		dma_free_coherent(&ntb->dev, inmw->size, inmw->virt_addr,
			inmw->dma_addr);
	}

	dev_dbg_mw(ctx, "Inbound memory windows are freed");
}

/*
 * Create the driver context structure
 */
static struct mw_ctx *mw_create_ctx(struct ntb_dev *ntb)
{
	struct mw_ctx *ctx, *ret;
	int node;

	/* Allocate the memory at the device NUMA node */
	node = dev_to_node(&ntb->dev);
	ctx = kzalloc_node(sizeof(*ctx), GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(ctx)) {
		dev_err(&ntb->dev,
			"No memory for NTB Memory windows driver context");
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize the context NTB device pointer */
	ctx->ntb = ntb;

	/* Read the number of memory windows */
	/* Number of memory windows local NTB device can set to the translated
	 * address register */
	ctx->outmws_cnt = ntb_mw_count(ntb);
	/* Number of memory windows peer can set to his translated address
	 * register */
	ctx->inmws_cnt = ntb_peer_mw_count(ntb);

	/* Alter the inbound memory windows count with respect to the driver
	 * parameter */
	mw_alter_params(ctx);

	/* Allocate the memory for memory window descriptors */
	ctx->outmws = kzalloc_node(ctx->outmws_cnt * sizeof(*ctx->outmws),
		GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(ctx->outmws)) {
		dev_err_mw(ctx,
			"Failed to allocate memory for outbound MW descriptors");
		ret = ERR_PTR(-ENOMEM);
		goto err_free_ctx;
	}
	ctx->inmws = kzalloc_node(ctx->inmws_cnt * sizeof(*ctx->inmws),
		GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(ctx->inmws)) {
		dev_err_mw(ctx,
			"Failed to allocate memory for inbound MW descriptors");
		ret = ERR_PTR(-ENOMEM);
		goto err_free_outmws;
	}

	dev_dbg_mw(ctx, "Context structure is created");

	return ctx;

/*err_free_inmws:
	kfree(ctx->inmws);
*/
err_free_outmws:
	kfree(ctx->outmws);
err_free_ctx:
	kfree(ctx);

	return ret;
}

/*
 * Free the driver context structure
 */
static void mw_free_ctx(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Free the outbound memory windows descriptors */
	kfree(ctx->outmws);

	/* Free the inbound memory windows descriptors */
	kfree(ctx->inmws);

	/* Free the memory allocated for the context structure */
	kfree(ctx);

	dev_dbg(&ntb->dev, "Context structure is freed");
}

/*
 * Initialize the ntb device structure
 */
static int mw_init_ntb_dev(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	int ret;

	/* Set the NTB device events context */
	ret = ntb_set_ctx(ntb, ctx, &mw_ops);
	if (SUCCESS != ret) {
		dev_err_mw(ctx, "Failed to specify the NTB device context");
		return ret;
	}

	/* Enable the link and rise the event to check the link state */
	ntb_link_enable(ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);
	/*ntb_link_event(ntb);*/

	dev_dbg_mw(ctx, "NTB device is initialized");

	return SUCCESS;
}

/*
 * Deinitialize the ntb device structure
 */
static void mw_stop_ntb_dev(struct mw_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Clear the context */
	ntb_clear_ctx(ntb);

	/* Disable the link */
	ntb_link_disable(ntb);

	dev_dbg_mw(ctx, "NTB device is deinitialized");
}

/*
 * Initialize the DMA masks
 */
static int __maybe_unused mw_ntb_set_dma_mask(struct ntb_dev *ntb)
{
	struct device *dev;
	int ret = SUCCESS;

	/* Get the NTB device structure */
	dev = &ntb->dev;
	/* Try to set the Highmem DMA address */
	ret = dma_set_mask(dev, DMA_BIT_MASK(64));
	if (SUCCESS == ret) {
		/* Next call won't fail of the upper one returned OK */
		dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
		return SUCCESS;
	}

	/* Warn if the HIGHMEM can be used for DMA */
	dev_warn(dev, "Cannot set the NTB device DMA highmem mask");

	/* Try the Low 32-bits DMA addresses */
	ret = dma_set_mask(dev, DMA_BIT_MASK(32));
	if (SUCCESS == ret) {
		/* The same is here */
		dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
		return SUCCESS;
	}

	dev_err(dev, "Failed to set the NTB device DMA lowmem mask");

	return ret;
}

/*
 * NTB device probe() callback function
 */
static int mw_probe(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct mw_ctx *ctx;
	int ret;

	/* Only asynchronous hardware is supported */
	if (!ntb_valid_async_dev_ops(ntb)) {
		return -EINVAL;
	}

	/* Check whether the messaging supports at least 4 DWORDS */
	ret = ntb_msg_size(ntb);
	if (4 > ret) {
		dev_err(&ntb->dev, "NTB Messaging supports just %d < 4 dwords",
			ret);
		return -EINVAL;
	}

	/* Set the NTB device DMA mask */
	/*ret = mw_ntb_set_dma_mask(ntb);
	if (SUCCESS != ret) {
		return ret;
	}*/

	/* Create the current device context */
	ctx = mw_create_ctx(ntb);
	if (IS_ERR_OR_NULL(ctx)) {
		return PTR_ERR(ctx);
	}

	/* Allocate the inbound memory windows */
	ret = mw_create_inmws(ctx);
	if (SUCCESS != ret) {
		goto err_free_ctx;
	}

	/* Map the outbound memory windows */
	ret = mw_create_outmws(ctx);
	if (SUCCESS != ret) {
		goto err_free_inmws;
	}

	/* Initialize the NTB device */
	ret = mw_init_ntb_dev(ctx);
	if (SUCCESS != ret) {
		goto err_free_outmws;
	}

	/* Create the DebugFS nodes */
	(void)mw_init_dbgfs(ctx);

	return SUCCESS;

/*err_stop_ntb_dev:
	mw_stop_ntb_dev(ctx);
*/
err_free_outmws:
	mw_free_outmws(ctx);
err_free_inmws:
	mw_free_inmws(ctx);
err_free_ctx:
	mw_free_ctx(ctx);

	return ret;

}

/*
 * NTB device remove() callback function
 */
static void mw_remove(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct mw_ctx *ctx = ntb->ctx;

	/* Send the message so the peer would lock outbound memory windows */
	mw_send_freeaddrs_cmd(ctx);

	/* Remove the DebugFS node */
	mw_deinit_dbgfs(ctx);

	/* Disable the NTB device link and clear the context */
	mw_stop_ntb_dev(ctx);

	/* Clear the outbound memory windows */
	mw_free_outmws(ctx);

	/* Clear the inbound memory windows */
	mw_free_inmws(ctx);

	/* Free the allocated context */
	mw_free_ctx(ctx);
}

/*
 * NTB bus client driver structure definition
 */
static struct ntb_client mw_client = {
	.ops = {
		.probe = mw_probe,
		.remove = mw_remove,
	},
};
/* module_ntb_client(mw_client); */

/*
 * Driver initialize method
 */
static int __init ntb_mw_init(void)
{
	/* Create the top DebugFS directory if the FS is initialized */
	if (debugfs_initialized())
		dbgfs_topdir = debugfs_create_dir(KBUILD_MODNAME, NULL);

	/* Registers the client driver */
	return ntb_register_client(&mw_client);
}
module_init(ntb_mw_init);

/*
 * Driver exit method
 */
static void __exit ntb_mw_exit(void)
{
	/* Unregister the client driver */
	ntb_unregister_client(&mw_client);

	/* Discard the top DebugFS directory */
	debugfs_remove_recursive(dbgfs_topdir);
}
module_exit(ntb_mw_exit);

