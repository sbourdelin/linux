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
 * PCIe NTB messaging test Linux driver
 *
 * Contact Information:
 * Serge Semin <fancer.lancer@gmail.com>, <Sergey.Semin@t-platforms.ru>
 */

/*
 *               NOTE of the NTB Messaging driver design.
 * The driver is designed to implement the simple transmition/reception
 * algorithm. User can send data to a peer by writing it to
 * debugfs:ntb_msg_test/ntbA_/data file, and one can read it by reading the
 * same file on the opposite side.
 */

/* Note: You can load this module with either option 'dyndbg=+p' or define the
 * next preprocessor constant */
/*#define DEBUG*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>

#include <linux/ntb.h>

#define DRIVER_NAME		"ntb_msg_test"
#define DRIVER_DESCRIPTION	"PCIe NTB Simple Messaging Client"
#define DRIVER_VERSION		"1.0"
#define CACHE_NAME		"ntb_msg_cache"

MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("T-platforms");

/*
 * DebugFS directory to place the driver debug file
 */
static struct dentry *dbgfs_topdir;

/*
 * Doorbells pingpong driver context
 * @ntb:	Pointer to the NTB device
 * @msg_cache:	Messages wrapper slab
 * @msg_lock:	Spin lock to synchrnize access to the messages list
 * @msg_list:	List of received messages
 * @msgcnt:	Number of received messages
 * @failed:	Number of failed transfers
 * @succeeded:	Number of succeeded transfers
 * @datasize:	Maximum size of message data (in bytes) excluding the size byte
 * @dbgfs_dir:	Handler of the driver DebugFS directory
 */
struct msg_ctx {
	struct ntb_dev *ntb;
	struct kmem_cache *msg_cache;
	spinlock_t msg_lock;
	struct list_head msg_list;
	unsigned long msgcnt;
	unsigned long failed;
	unsigned long succeeded;
	size_t datasize;
	struct dentry *dbgfs_dir;
};

/*
 * Received messages container
 * @msg:	Message
 * @entry:	List entry
 */
struct ntb_msg_wrap {
	struct ntb_msg msg;
	struct list_head entry;
};

/*
 * Message converter is used to translate the struct ntb_msg to the
 * char sized data structure with size.
 * @size:	Size of the data
 * @data:	Pointer to the data buffer
 */
struct ntb_msg_conv {
	u8 size;
	char data[];
};

/*
 * Wrapper dev_err/dev_warn/dev_info/dev_dbg macros
 */
#define dev_err_msg(ctx, args...) \
	dev_err(&ctx->ntb->dev, ## args)
#define dev_warn_msg(ctx, args...) \
	dev_warn(&ctx->ntb->dev, ## args)
#define dev_info_msg(ctx, args...) \
	dev_info(&ctx->ntb->dev, ## args)
#define dev_dbg_msg(ctx, args...) \
	dev_dbg(&ctx->ntb->dev, ## args)

/*
 * Some common constant used in the driver for better readability:
 * @ON: Enable something
 * @OFF: Disable something
 * @SUCCESS: Success of a function execution
 */
#define ON ((u32)0x1)
#define OFF ((u32)0x0)
#define SUCCESS 0

/*===========================================================================
 *                         Incoming messages handlers
 *===========================================================================*/

/*
 * Save the receive message
 */
static void msg_recv_handler(struct msg_ctx *ctx, const struct ntb_msg *msg)
{
	struct ntb_msg_wrap *wrap;
	struct ntb_msg_conv *conv;

	/* Cast the message to the converted one */
	conv = (struct ntb_msg_conv *)msg;

	/* Allocate the memory from the slab */
	wrap = kmem_cache_alloc(ctx->msg_cache, GFP_KERNEL);
	if (NULL == wrap) {
		dev_err_msg(ctx,
			"Failed to allocate memory for incoming message %.*s",
			conv->size, conv->data);
		return;
	}

	/* Copy the message to the buffer */
	memcpy(&wrap->msg, msg, conv->size + 1);

	/* Add the wrapped message to the list of received messages */
	spin_lock(&ctx->msg_lock);
	list_add_tail(&wrap->entry, &ctx->msg_list);
	/* Increment the number of received messages in the buffer */
	ctx->msgcnt++;
	spin_unlock(&ctx->msg_lock);

	dev_dbg_msg(ctx, "Message '%.*s' was received",
		conv->size, conv->data);
}

/*
 * Handler of the transmit errors
 */
static void msg_fail_handler(struct msg_ctx *ctx, const struct ntb_msg *msg)
{
	struct ntb_msg_conv *conv = (struct ntb_msg_conv *)msg;

	/* Just print the error increment the errors counter */
	dev_err_msg(ctx,
		"Failed to send the submessage '%.*s'",
			conv->size, conv->data);
	ctx->failed++;
}

/*
 * Handler of the succeeded transmits
 */
static void msg_sent_handler(struct msg_ctx *ctx, const struct ntb_msg *msg)
{
	struct ntb_msg_conv *conv = (struct ntb_msg_conv *)msg;

	/* Just print the debug text and increment the succeeded msgs counter */
	dev_dbg_msg(ctx,
		"Submessage '%.*s' has been successfully sent",
			conv->size, conv->data);
	ctx->succeeded++;
}

/*
 * Message event handler
 */
static void msg_event_handler(void *data, enum NTB_MSG_EVENT ev,
			      struct ntb_msg *msg)
{
	struct msg_ctx *ctx = data;

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
		dev_err_msg(ctx, "Got invalid message event %d", ev);
		break;
	}
}

/*===========================================================================
 *                      11. DebugFS callback functions
 *===========================================================================*/

static ssize_t msg_dbgfs_data_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp);

static ssize_t msg_dbgfs_data_write(struct file *filep, const char __user *ubuf,
				    size_t usize, loff_t *offp);

static ssize_t msg_dbgfs_stat_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp);

/*
 * DebugFS data node operations
 */
static const struct file_operations msg_dbgfs_data_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = msg_dbgfs_data_read,
	.write = msg_dbgfs_data_write
};

/*
 * DebugFS statistics node operations
 */
static const struct file_operations msg_dbgfs_stat_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = msg_dbgfs_stat_read,
};

/*
 * DebugFS callback of read messages node
 */
static ssize_t msg_dbgfs_data_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp)
{
	struct msg_ctx *ctx = filep->private_data;
	struct list_head *entry, *safe_entry;
	struct ntb_msg_wrap *wrap;
	struct ntb_msg_conv *conv;
	size_t datasize, retsize;
	char *databuf;
	ssize_t ret;

	/* Find the size of the retrieved data in messages */
	datasize = 0;
	spin_lock(&ctx->msg_lock);
	list_for_each(entry, &ctx->msg_list) {
		wrap = list_entry(entry, struct ntb_msg_wrap, entry);
		conv = (struct ntb_msg_conv *)&wrap->msg;
		datasize += conv->size;
	}
	spin_unlock(&ctx->msg_lock);

	/* Calculate the size of the output buffer */
	datasize = min(datasize, usize);

	/* Allocate the buffer */
	databuf = kmalloc(datasize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_msg(ctx, "No memory to allocate the output buffer");
		return -ENOMEM;
	}

	/* Copy the data from the messages to the output buffer */
	retsize = 0;
	spin_lock(&ctx->msg_lock);
	list_for_each_safe(entry, safe_entry, &ctx->msg_list) {
		/* Get the message and copy it to the buffer */
		wrap = list_entry(entry, struct ntb_msg_wrap, entry);
		conv = (struct ntb_msg_conv *)&wrap->msg;

		/* If there is no enough space left in the buffer then stop the
		 * loop */
		if ((datasize - retsize) < conv->size) {
			break;
		}

		/* Copy the data to the output buffer */
		memcpy(&databuf[retsize], conv->data, conv->size);

		/* Increment the size of the retrieved data */
		retsize += conv->size;

		/* Delete the list entry and free the memory */
		list_del(&wrap->entry);
		kmem_cache_free(ctx->msg_cache, wrap);

		/* Decrement the number of messages in the buffer */
		ctx->msgcnt--;
	}
	spin_unlock(&ctx->msg_lock);

	/* Copy the text to the output buffer */
	ret = simple_read_from_buffer(ubuf, usize, offp, databuf, retsize);

	/* Free the memory allocated for the buffer */
	kfree(databuf);

	return ret;
}

/*
 * DebugFS callback of write messages node
 */
static ssize_t msg_dbgfs_data_write(struct file *filep, const char __user *ubuf,
				    size_t usize, loff_t *offp)
{
	struct msg_ctx *ctx = filep->private_data;
	struct ntb_dev *ntb = ctx->ntb;
	struct ntb_msg msg;
	struct ntb_msg_conv *conv;
	char *databuf;
	int pos, copied, sts = SUCCESS;
	ssize_t ret;

	/* Allocate the memory for sending data */
	databuf = kmalloc(usize, GFP_KERNEL);
	if (NULL == databuf) {
		dev_err_msg(ctx, "No memory to allocate the sending data buffer");
		return -ENOMEM;
	}

	/* Copy the data to the output buffer */
	ret = simple_write_to_buffer(databuf, usize, offp, ubuf, usize);
	if (0 > ret) {
		dev_err_msg(ctx, "Failed to copy the data from the User-space");
		kfree(databuf);
		return ret;
	}

	/* Start copying data to the message structure and send it straight away
	 * to the peer */
	conv = (struct ntb_msg_conv *)&msg;
	for (pos = 0, copied = 0; pos < usize; pos += copied) {
		/* Calculate the size of data to copy to the message */
		copied = min(ctx->datasize, (usize - pos));
		/* Set the data size of the message */
		conv->size = copied;
		/* Copy the data */
		memcpy(conv->data, &databuf[pos], copied);

		/* Send the data stright away */
		sts = ntb_msg_post(ntb, &msg);
		if (SUCCESS != sts) {
			dev_err_msg(ctx, "Failed to post the submessage %.*s",
				copied, conv->data);
		}
	}

	return (SUCCESS == sts) ? usize : -EINVAL;
}

/*
 * DebugFS callback to read statistics
 */
static ssize_t msg_dbgfs_stat_read(struct file *filep, char __user *ubuf,
				   size_t usize, loff_t *offp)
{
	struct msg_ctx *ctx = filep->private_data;
	struct ntb_dev *ntb = ctx->ntb;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;

	/* Limit the buffer size */
	size = min_t(size_t, usize, 0x800U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		dev_dbg_msg(ctx,
			"Failed to allocate the memory for statistics "
			"output buffer");
		return -ENOMEM;
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tNTB Messaging Test driver:\n\n");

	/* Current driver state */
	off += scnprintf(strbuf + off, size - off,
		"Link state\t\t- %s\n",
		(ON == ntb_link_is_up(ntb, NULL, NULL)) ? "Up" : "Down");
	off += scnprintf(strbuf + off, size - off,
		"Message count\t\t- %lu\n", ctx->msgcnt);
	off += scnprintf(strbuf + off, size - off,
		"Message size\t\t- %u\n", ntb_msg_size(ntb));
	off += scnprintf(strbuf + off, size - off,
		"Data size\t\t- %lu\n", (unsigned long)ctx->datasize);
	off += scnprintf(strbuf + off, size - off,
		"Successfully sent\t- %lu\n", ctx->succeeded);
	off += scnprintf(strbuf + off, size - off,
		"Failed to send\t\t- %lu\n", ctx->failed);

	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, usize, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * DebugFS initialization function
 */
static int msg_init_dbgfs(struct msg_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct dentry *dbgfs_data, *dbgfs_stat;
	const char *devname;
	int ret;

	/* If the top directory is not created then do nothing */
	if (IS_ERR_OR_NULL(dbgfs_topdir)) {
		dev_warn_msg(ctx,
			"Top DebugFS directory has not been created for "
			DRIVER_NAME);
		return PTR_ERR(dbgfs_topdir);
	}

	/* Retrieve the device name */
	devname = dev_name(&ntb->dev);

	/* Create the device related subdirectory */
	ctx->dbgfs_dir = debugfs_create_dir(devname, dbgfs_topdir);
	if (IS_ERR_OR_NULL(ctx->dbgfs_dir)) {
		dev_warn_msg(ctx,
			"Failed to create the DebugFS subdirectory %s",
			devname);
		return PTR_ERR(ctx->dbgfs_dir);
	}

	/* Create the file node for data io operations */
	dbgfs_data = debugfs_create_file("data", S_IRWXU, ctx->dbgfs_dir, ctx,
					 &msg_dbgfs_data_ops);
	if (IS_ERR(dbgfs_data)) {
		dev_err_msg(ctx, "Could not create DebugFS data node");
		ret = PTR_ERR(dbgfs_data);
		goto err_rm_dir;
	}

	/* Create the file node for statistics io operations */
	dbgfs_stat = debugfs_create_file("stat", S_IRWXU, ctx->dbgfs_dir, ctx,
					 &msg_dbgfs_stat_ops);
	if (IS_ERR(dbgfs_stat)) {
		dev_err_msg(ctx, "Could not create DebugFS statistics node");
		ret = PTR_ERR(dbgfs_stat);
		goto err_rm_dir;
	}

	dev_dbg_msg(ctx, "NTB Messaging DebugFS nodes are created for %s",
		devname);

	return SUCCESS;

err_rm_dir:
	debugfs_remove_recursive(ctx->dbgfs_dir);

	return ret;
}

/*
 * DebugFS deinitialization function
 */
static void msg_deinit_dbgfs(struct msg_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Remove the DebugFS directory */
	debugfs_remove_recursive(ctx->dbgfs_dir);

	dev_dbg_msg(ctx, "NTB Messaging DebugFS nodes %s/ are discarded",
		dev_name(&ntb->dev));
}

/*===========================================================================
 *                   NTB device/client driver initialization
 *===========================================================================*/

/*
 * NTB device events handlers
 */
static const struct ntb_ctx_ops msg_ops = {
	.msg_event = msg_event_handler
};

/*
 * Create the driver context structure
 */
static struct msg_ctx *msg_create_ctx(struct ntb_dev *ntb)
{
	struct msg_ctx *ctx;
	int node;

	/* Allocate the memory at the device NUMA node */
	node = dev_to_node(&ntb->dev);
	ctx = kzalloc_node(sizeof(*ctx), GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(ctx)) {
		dev_err(&ntb->dev,
			"No memory for NTB Messaging driver context");
		return ERR_PTR(-ENOMEM);
	}

	/* Create the message cache */
	ctx->msg_cache = kmem_cache_create(CACHE_NAME,
		sizeof(struct ntb_msg_wrap), 0, 0, NULL);
	if (NULL == ctx->msg_cache) {
		dev_err(&ntb->dev,
			"Failed to allocate the message wrap structures cache");
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize the context NTB device pointer */
	ctx->ntb = ntb;

	/* Initialize the message list lock and the list head */
	spin_lock_init(&ctx->msg_lock);
	INIT_LIST_HEAD(&ctx->msg_list);

	/* Initialize the counters */
	ctx->msgcnt = 0;
	ctx->failed = 0;
	ctx->succeeded = 0;

	/* Initialize the data size of one message excluding the size byte */
	ctx->datasize = 4*ntb_msg_size(ntb) - 1;

	dev_dbg_msg(ctx, "Context structure is created");

	return ctx;
}

/*
 * Free the driver context structure
 */
static void msg_free_ctx(struct msg_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	struct ntb_msg_wrap *wrap;
	struct list_head *entry, *safe_entry;

	/* Walk through the list of messages and destroy all the allocated
	 * memory */
	spin_lock(&ctx->msg_lock);
	list_for_each_safe(entry, safe_entry, &ctx->msg_list) {
		/* Get the message wrapper */
		wrap = list_entry(entry, struct ntb_msg_wrap, entry);

		/* Delete the list entry and free the memory */
		list_del(entry);
		kmem_cache_free(ctx->msg_cache, wrap);

		/* Decrement the number of messages in the buffer */
		ctx->msgcnt--;
	}
	spin_unlock(&ctx->msg_lock);

	/* Destroy the IDT messages cache */
	kmem_cache_destroy(ctx->msg_cache);

	/* Free the memory allocated for the context structure */
	kfree(ctx);

	dev_dbg(&ntb->dev, "Context structure is freed");
}

/*
 * Initialize the ntb device structure
 */
static int msg_init_ntb_dev(struct msg_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	int ret;

	/* Set the NTB device events context */
	ret = ntb_set_ctx(ntb, ctx, &msg_ops);
	if (SUCCESS != ret) {
		dev_err_msg(ctx, "Failed to specify the NTB device context");
		return ret;
	}

	/* Enable the link */
	ntb_link_enable(ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);
	/*ntb_link_event(ntb);*/

	dev_dbg_msg(ctx, "NTB device is initialized");

	return SUCCESS;
}

/*
 * Deinitialize the ntb device structure
 */
static void msg_stop_ntb_dev(struct msg_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Disable the link */
	ntb_link_disable(ntb);

	/* Clear the context */
	ntb_clear_ctx(ntb);

	dev_dbg_msg(ctx, "NTB device is deinitialized");
}

/*
 * NTB device probe() callback function
 */
static int msg_probe(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct msg_ctx *ctx;
	int ret;

	/* Only asynchronous hardware is supported */
	if (!ntb_valid_async_dev_ops(ntb)) {
		return -EINVAL;
	}

	/* Create the current device context */
	ctx = msg_create_ctx(ntb);
	if (IS_ERR_OR_NULL(ctx)) {
		return PTR_ERR(ctx);
	}

	/* Initialize the NTB device */
	ret = msg_init_ntb_dev(ctx);
	if (SUCCESS != ret) {
		msg_free_ctx(ctx);
		return ret;
	}

	/* Create the DebugFS node */
	(void)msg_init_dbgfs(ctx);

	return SUCCESS;
}

/*
 * NTB device remove() callback function
 */
static void msg_remove(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct msg_ctx *ctx = ntb->ctx;

	/* Remove the DebugFS node */
	msg_deinit_dbgfs(ctx);

	/* Disable the NTB device link and clear the context */
	msg_stop_ntb_dev(ctx);

	/* Free the allocated context */
	msg_free_ctx(ctx);
}

/*
 * NTB bus client driver structure definition
 */
static struct ntb_client msg_client = {
	.ops = {
		.probe = msg_probe,
		.remove = msg_remove,
	},
};
/* module_ntb_client(msg_client); */

/*
 * Driver initialize method
 */
static int __init ntb_msg_init(void)
{
	/* Create the top DebugFS directory if the FS is initialized */
	if (debugfs_initialized())
		dbgfs_topdir = debugfs_create_dir(KBUILD_MODNAME, NULL);

	/* Registers the client driver */
	return ntb_register_client(&msg_client);
}
module_init(ntb_msg_init);

/*
 * Driver exit method
 */
static void __exit ntb_msg_exit(void)
{
	/* Unregister the client driver */
	ntb_unregister_client(&msg_client);

	/* Discard the top DebugFS directory */
	debugfs_remove_recursive(dbgfs_topdir);
}
module_exit(ntb_msg_exit);

