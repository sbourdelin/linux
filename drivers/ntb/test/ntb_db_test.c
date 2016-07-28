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
 * PCIe NTB doorbells test Linux driver
 *
 * Contact Information:
 * Serge Semin <fancer.lancer@gmail.com>, <Sergey.Semin@t-platforms.ru>
 */

/*
 *       NOTE of the NTB doorbells pingpong driver design.
 * The driver is designed to implement the pingpong algorithm. After a quick
 * initailization the driver starts from setting the peer doorbell of the last
 * locally set doorbell bit. If there is not any doorbell locally set, then it
 * sets the very first bit. After that the driver unmasks the events of the
 * just set bits and waits until the peer is set the same doorbell. When it's
 * done, the driver iterates to the next doorbell and starts delayed work
 * thread, which will set the corresponding bit and perform doorbell umasking
 * on waking up.
 */

/* Note: You can load this module with either option 'dyndbg=+p' or define the
 * next preprocessor constant */
/*#define DEBUG*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>

#include <linux/ntb.h>

#define DRIVER_NAME		"ntb_db_test"
#define DRIVER_DESCRIPTION	"PCIe NTB Doorbells Pingpong Client"
#define DRIVER_VERSION		"1.0"

MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("T-platforms");

static unsigned int delay_ms = 1000;
module_param(delay_ms, uint, 0644);
MODULE_PARM_DESC(delay_ms,
	"Milliseconds to delay before setting a next doorbell bit");

/*
 * DebugFS directory to place the driver debug file
 */
static struct dentry *dbgfs_dir;

/*
 * Enumeration of the driver states
 * @PP_WAIT:	Driver waits until the peer sets the corresponding doorbell bit
 * @PP_SLEEP:	Driver sleeps before to set the next doorbell bit
 */
enum db_pp_state {
	PP_WAIT = 0,
	PP_SLEEP = 1
};

/*
 * Doorbells pingpong driver context
 * @ntb:	Pointer to the NTB device
 * @cycle:	Doorbells setting cycle made up until now
 * @valid_ids:	Valid Doorbel bits
 * @delay:	Delay between setting the next doorbell bit
 * @state:	Current cycle state
 * @dwork:	Kernel thread used to perform the delayed doorbell bit set
 * @dbgfs_info:	Handler of the DebugFS driver info-file
 */
struct pp_ctx {
	struct ntb_dev *ntb;
	unsigned long long cycle;
	u64 valid_ids;
	unsigned long delay;
	enum db_pp_state state;
	struct delayed_work dwork;
	struct dentry *dbgfs_info;
};
#define to_ctx_dwork(work) \
	container_of(to_delayed_work(work), struct pp_ctx, dwork)

/*
 * Wrapper dev_err/dev_warn/dev_info/dev_dbg macros
 */
#define dev_err_pp(ctx, args...) \
	dev_err(&ctx->ntb->dev, ## args)
#define dev_warn_pp(ctx, args...) \
	dev_warn(&ctx->ntb->dev, ## args)
#define dev_info_pp(ctx, args...) \
	dev_info(&ctx->ntb->dev, ## args)
#define dev_dbg_pp(ctx, args...) \
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
 *                           Helper functions
 *===========================================================================*/

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#endif /* !GENMASK_ULL */

/*
 * Set the corresponding bit in the 64-bits wide word
 */
#ifndef BIT_ULL
#define BIT_ULL(nr) (1ULL << (nr))
#endif /* !BIT_ULL */

/*
 * Method to find a first set bit in 64-bits wide word. The bits numbering is
 * from 0 to 63. If there is no any set bit, then 64 is returned.
 */
static inline unsigned long find_first_bit64(u64 var)
{
	return (0x0ULL == var) ? BITS_PER_LONG_LONG : __ffs64(var);
}

/*
 * Method to find a next set bit in 64-bits wide word starting from the
 * specified position. The bits numbering is from 0 to 63. If there is no any
 * set bit within the position and the last bit of the word, then 64 is
 * returned.
 */
static inline unsigned long find_next_bit64(u64 var, unsigned long pos)
{
	/* Consider only the valuable positions */
	var &= GENMASK_ULL(BITS_PER_LONG_LONG - 1, pos);

	return find_first_bit64(var);
}

/*===========================================================================
 *                Pingpong algorithm functions definition
 *===========================================================================*/

/*
 * Iterate Doorbell PingPong algorithm work thread
 * This function clears the currently set doorbell bit, which has been
 * unmasked before, and masks it back. Then method sets the next doorbell
 * bit and locally unmasks it.
 */
static void pp_iterate_cycle(struct work_struct *work)
{
	struct pp_ctx *ctx = to_ctx_dwork(work);
	struct ntb_dev *ntb = ctx->ntb;
	u64 db_umsk, db_sts;
	unsigned long db_id;
	int ret;

	/* Read the mask of the current disposition */
	db_umsk = ~ntb_db_read_mask(ntb) & ctx->valid_ids;
	if (1 != hweight64(db_umsk)) {
		dev_err_pp(ctx,
			"Got invalid doorbells mask %#018llx", db_umsk);
		return;
	}

	/* Read the currently set doorbells */
	db_sts = ntb_db_read(ntb);
	if (0x0 == (db_sts & db_umsk)) {
		dev_err_pp(ctx, "Got driver bug %#018llx & %#018llx == 0",
			db_sts, db_umsk);
		return;
	}

	/* Find the doorbell id (use db_umsk since db_sts can have several
	 * bits set) */
	db_id = find_first_bit64(db_umsk);

	dev_dbg_pp(ctx, "PingPong the doorbell bit %lu of cycle %llu",
		db_id, ctx->cycle);

	/* Mask the currently unmasked doorbell */
	ret = ntb_db_set_mask(ntb, db_umsk);
	if (SUCCESS != ret) {
		dev_err_pp(ctx, "Failed to mask db %lu by %#018llx",
			db_id, db_umsk);
		return;
	}

	/* Clear the currently set doorbell */
	ret = ntb_db_clear(ntb, db_umsk);
	if (SUCCESS != ret) {
		dev_err_pp(ctx,
			"Failed to clear the db bit %lu", db_id);
		return;
	}

	/* Iterate the doorbell id to set the next doorbell bit */
	db_id = find_next_bit64(ctx->valid_ids, db_id + 1);
	if (BITS_PER_LONG_LONG == db_id) {
		db_id = find_first_bit64(ctx->valid_ids);
		ctx->cycle++;
	}

	/* Calculate the new unmasking field */
	db_umsk = BIT_ULL(db_id);

	/* Set the new peer doorbell bit */
	ret = ntb_peer_db_set(ntb, db_umsk);
	if (SUCCESS != ret) {
		dev_err_pp(ctx,
			"Failed to set the peer doorbell %lu by field %#018llx",
			db_id, db_umsk);
		return;
	}

	/* After this the driver is waiting for the peer response */
	ctx->state = PP_WAIT;

	/* Unmask the corresponding doorbell bit to receive the event */
	ret = ntb_db_clear_mask(ntb, db_umsk);
	if (SUCCESS != ret) {
		dev_err_pp(ctx,
			"Failed to unmask the doorbell %lu by field %#018llx",
			db_id, db_umsk);
		return;
	}
}

/*
 * Handle the event of Doorbell set
 */
static void pp_db_event(void *data, int vec)
{
	struct pp_ctx *ctx = data;

	/* From now the driver is sleeping before sending the response */
	ctx->state = PP_SLEEP;

	/* Schedule the delayed work of the algorithm */
	(void)schedule_delayed_work(&ctx->dwork, ctx->delay);
}

/*===========================================================================
 *                      11. DebugFS callback functions
 *===========================================================================*/

static ssize_t pp_dbgfs_read(struct file *filp, char __user *ubuf,
				  size_t count, loff_t *offp);

/*
 * Driver DebugFS operations
 */
static const struct file_operations pp_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = pp_dbgfs_read
};

/*
 * DebugFS read node info callback
 */
static ssize_t pp_dbgfs_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *offp)
{
	struct pp_ctx *ctx = filp->private_data;
	struct ntb_dev *ntb = ctx->ntb;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;

	/* Limit the buffer size */
	size = min_t(size_t, count, 0x800U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		return -ENOMEM;
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tNTB Doorbells PingPong test driver:\n\n");

	/* Current driver state */
	off += scnprintf(strbuf + off, size - off,
		"Link state\t- %s\n",
		(ON == ntb_link_is_up(ntb, NULL, NULL)) ? "Up" : "Down");
	off += scnprintf(strbuf + off, size - off,
		"Cycle\t\t- %llu\n", ctx->cycle);
	off += scnprintf(strbuf + off, size - off,
		"Algo state\t- %s\n",
		(PP_SLEEP == ctx->state) ? "sleep" : "wait");
	off += scnprintf(strbuf + off, size - off,
		"Delay\t\t- %u ms\n", delay_ms);

	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, count, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * Driver DebugFS initialization function
 */
static int pp_init_dbgfs(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	const char *devname;

	/* If the top directory is not created then do nothing */
	if (IS_ERR_OR_NULL(dbgfs_dir)) {
		dev_warn_pp(ctx,
			"Top DebugFS directory has not been created for "
			DRIVER_NAME);
		return PTR_ERR(dbgfs_dir);
	}

	/* Retrieve the device name */
	devname = dev_name(&ntb->dev);

	/* Create the corresponding file node */
	ctx->dbgfs_info = debugfs_create_file(devname, S_IRUSR,
		dbgfs_dir, ctx, &pp_dbgfs_ops);
	if (IS_ERR(ctx->dbgfs_info)) {
		dev_err_pp(ctx, "Could not create the DebugFS node %s",
			devname);
		return PTR_ERR(ctx->dbgfs_info);
	}

	dev_dbg_pp(ctx, "Doorbell PingPong DebugFS node is created for %s",
		devname);

	return SUCCESS;
}

/*
 * Driver DebugFS deinitialization function
 */
static void pp_deinit_dbgfs(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Remove the DebugFS file */
	debugfs_remove(ctx->dbgfs_info);

	dev_dbg_pp(ctx, "Doorbell PingPong DebugFS node %s is discarded",
		dev_name(&ntb->dev));
}

/*===========================================================================
 *                   NTB device/client driver initialization
 *===========================================================================*/

/*
 * NTB device events handlers
 */
static const struct ntb_ctx_ops pp_ops = {
	.db_event = pp_db_event
};

/*
 * Create the driver context structure
 */
static struct pp_ctx *pp_create_ctx(struct ntb_dev *ntb)
{
	struct pp_ctx *ctx;
	int node;

	/* Allocate the memory at the device NUMA node */
	node = dev_to_node(&ntb->dev);
	ctx = kzalloc_node(sizeof(*ctx), GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(ctx)) {
		dev_err(&ntb->dev,
			"No memory for NTB PingPong driver context");
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize the NTB device descriptor and delayed work */
	ctx->ntb = ntb;
	ctx->cycle = 0;
	ctx->valid_ids = ntb_db_valid_mask(ntb);
	ctx->delay = msecs_to_jiffies(delay_ms);
	ctx->state = PP_WAIT;
	INIT_DELAYED_WORK(&ctx->dwork, pp_iterate_cycle);

	dev_dbg_pp(ctx, "Context structure is created");

	return ctx;
}

/*
 * Free the driver context structure
 */
static void pp_free_ctx(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Just free the memory allocated for the context structure */
	kfree(ctx);

	dev_dbg(&ntb->dev, "Context structure is freed");
}

/*
 * Correspondingly initialize the ntb device structure
 */
static int pp_init_ntb_dev(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	int ret;

	/* Set the NTB device events context */
	ret = ntb_set_ctx(ntb, ctx, &pp_ops);
	if (SUCCESS != ret) {
		dev_err_pp(ctx, "Failed to specify the NTB device context");
		return ret;
	}

	/* Enable the link */
	ntb_link_enable(ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);
	/*ntb_link_event(ntb);*/

	dev_dbg_pp(ctx, "NTB device is initialized");

	return SUCCESS;
}

/*
 * Deinitialize the ntb device structure
 */
static void pp_stop_ntb_dev(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;

	/* Disable the link */
	ntb_link_disable(ntb);

	/* Clear the context to make sure there won't be any doorbell event */
	ntb_clear_ctx(ntb);

	dev_dbg_pp(ctx, "NTB device is deinitialized");
}

/*
 * Initialize the basic algorithm-related fields
 */
static int pp_init_algo(struct pp_ctx *ctx)
{
	struct ntb_dev *ntb = ctx->ntb;
	u64 db_sts, db_umsk;
	int ret;

	/* Read the current mask */
	db_umsk = ~ntb_db_read_mask(ntb) & ctx->valid_ids;

	/* If all doorbell have been unmasked then mask them all */
	if (db_umsk == ctx->valid_ids) {
		ret = ntb_db_set_mask(ntb, db_umsk);
		if (SUCCESS != ret) {
			dev_err_pp(ctx,
				"Failed to mask all the doorbells "
				"%#018llx", db_umsk);
			return ret;
		}
		/* Set the unmasking variable to zero so the algorithm would
		 * initialize the corresponding DB bit */
		db_umsk = 0;
	}

	/* If there is no any unmasked bit then set the very first peer doorbell
	 * bit and locally unmask it */
	if (0x0 == db_umsk) {
		db_umsk = BIT_ULL(0);
		/* Set the new peer doorbell bit */
		ret = ntb_peer_db_set(ntb, db_umsk);
		if (SUCCESS != ret) {
			dev_err_pp(ctx,
				"Failed to set the peer doorbell %u by field "
				"%#018llx", 0, db_umsk);
			return ret;
		}
		/* Clear the mask of the corresponding doorbell bit */
		ret = ntb_db_clear_mask(ntb, db_umsk);
		if (SUCCESS != ret) {
			dev_err_pp(ctx,
				"Failed to unmask the doorbell %u by field "
				"%#018llx", 0, db_umsk);
			return ret;
		}
	}
	/* If there is one umasked bit then just read the doorbell status.
	 * If the bit is set then just start the work thread to handle the
	 * disposition otherwise just don't do anything waiting for the peer
	 * to set the doorbell bit */
	else if (1 == hweight64(db_umsk)) {
		db_sts = ntb_db_read(ntb);
		if (0x0 != (db_sts & db_umsk)) {
			/* Schedule the delayed work of the algorithm */
			(void)schedule_delayed_work(&ctx->dwork, ctx->delay);
		}
	} else /* if (1 < hweight64(db_umsk)) */ {
		dev_err_pp(ctx, "Invalid mask is found %#018llx", db_umsk);
		return -EINVAL;
	}

	dev_dbg_pp(ctx, "Doorbell PingPong algorithm is initialized");

	return SUCCESS;
}

/*
 * Stop the driver algorithm
 */
static void pp_stop_algo(struct pp_ctx *ctx)
{
	/* Make sure the delayed work is not started */
	cancel_delayed_work_sync(&ctx->dwork);

	dev_dbg_pp(ctx, "Doorbell PingPong algorithm is stopped");
}

/*
 * NTB device probe() callback function
 */
static int pp_probe(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct pp_ctx *ctx;
	int ret;

	/* Both synchronous and asynchronous hardware is supported */
	if (!ntb_valid_sync_dev_ops(ntb) && !ntb_valid_async_dev_ops(ntb)) {
		return -EINVAL;
	}

	/* Create the current device context */
	ctx = pp_create_ctx(ntb);
	if (IS_ERR_OR_NULL(ctx)) {
		return PTR_ERR(ctx);
	}

	/* Initialize the NTB device */
	ret = pp_init_ntb_dev(ctx);
	if (SUCCESS != ret) {
		goto err_free_ctx;
	}

	/* Initialize the pingpong algorithm */
	ret = pp_init_algo(ctx);
	if (SUCCESS != ret) {
		goto err_stop_ntb_dev;
	}

	/* Create the DebugFS node */
	(void)pp_init_dbgfs(ctx);

	/* Start  */

	return SUCCESS;

/*err_stop_algo:
	pp_stop_algo(ctx);
*/
err_stop_ntb_dev:
	pp_stop_ntb_dev(ctx);

err_free_ctx:
	pp_free_ctx(ctx);

	return ret;
}

/*
 * NTB device remove() callback function
 */
static void pp_remove(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct pp_ctx *ctx = ntb->ctx;

	/* Remove the DebugFS node */
	pp_deinit_dbgfs(ctx);

	/* Disable the NTB device link and clear the context */
	pp_stop_ntb_dev(ctx);

	/* Stop the algorithm */
	pp_stop_algo(ctx);

	/* Free the allocated context */
	pp_free_ctx(ctx);
}

/*
 * NTB bus client driver structure definition
 */
static struct ntb_client pp_client = {
	.ops = {
		.probe = pp_probe,
		.remove = pp_remove,
	},
};
/* module_ntb_client(pp_client); */

/*
 * Driver initialize method
 */
static int __init ntb_pp_init(void)
{
	/* Create the top DebugFS directory if the FS is initialized */
	if (debugfs_initialized())
		dbgfs_dir = debugfs_create_dir(KBUILD_MODNAME, NULL);

	/* Registers the client driver */
	return ntb_register_client(&pp_client);
}
module_init(ntb_pp_init);

/*
 * Driver exit method
 */
static void __exit ntb_pp_exit(void)
{
	/* Unregister the client driver */
	ntb_unregister_client(&pp_client);

	/* Discard the top DebugFS directory */
	debugfs_remove_recursive(dbgfs_dir);
}
module_exit(ntb_pp_exit);

