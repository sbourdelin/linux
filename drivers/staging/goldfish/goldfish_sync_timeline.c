/*
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/acpi.h>

#include <linux/string.h>

#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/sync_file.h>
#include <linux/dma-fence.h>

#include <linux/goldfish.h>

#include <uapi/linux/goldfish/goldfish_sync.h>

#include "goldfish_sync_timeline_fence.h"

/* The Goldfish sync driver is designed to provide a interface
 * between the underlying host's sync device and the kernel's
 * fence sync framework..
 * The purpose of the device/driver is to enable lightweight
 * creation and signaling of timelines and fences
 * in order to synchronize the guest with host-side graphics events.
 *
 * Each time the interrupt trips, the driver
 * may perform a sync operation.
 */

/* The operations are: */
enum cmd_id {
	/* Ready signal - used to mark when irq should lower */
	CMD_SYNC_READY			= 0,

	/* Create a new timeline. writes timeline handle */
	CMD_CREATE_SYNC_TIMELINE	= 1,

	/* Create a fence object. reads timeline handle and time argument.
	 * Writes fence fd to the SYNC_REG_HANDLE register.
	 */
	CMD_CREATE_SYNC_FENCE		= 2,

	/* Increments timeline. reads timeline handle and time argument */
	CMD_SYNC_TIMELINE_INC		= 3,

	/* Destroys a timeline. reads timeline handle */
	CMD_DESTROY_SYNC_TIMELINE	= 4,

	/* Starts a wait on the host with
	 * the given glsync object and sync thread handle.
	 */
	CMD_TRIGGER_HOST_WAIT		= 5,
};

/* The register layout is: */
enum sync_reg_id {
	/* host->guest batch commands */
	SYNC_REG_BATCH_COMMAND			= 0x00,

	/* guest->host batch commands */
	SYNC_REG_BATCH_GUESTCOMMAND		= 0x04,

	/* communicate physical address of host->guest batch commands */
	SYNC_REG_BATCH_COMMAND_ADDR		= 0x08,

	/* 64-bit part */
	SYNC_REG_BATCH_COMMAND_ADDR_HIGH	= 0x0c,

	/* communicate physical address of guest->host commands */
	SYNC_REG_BATCH_GUESTCOMMAND_ADDR	= 0x10,

	/* 64-bit part */
	SYNC_REG_BATCH_GUESTCOMMAND_ADDR_HIGH	= 0x14,

	/* signals that the device has been probed */
	SYNC_REG_INIT				= 0x18,
};

/* The above definitions (command codes, register layout, ioctl definitions)
 * need to be in sync with the following files:
 *
 * Host-side (emulator):
 * external/qemu/android/emulation/goldfish_sync.h
 * external/qemu-android/hw/misc/goldfish_sync.c
 *
 * Guest-side (system image):
 * device/generic/goldfish-opengl/system/egl/goldfish_sync.h
 * device/generic/goldfish/ueventd.ranchu.rc
 * platform/build/target/board/generic/sepolicy/file_contexts
 */
struct goldfish_sync_hostcmd {
	/* sorted for alignment */
	u64 handle;
	u64 hostcmd_handle;
	u32 cmd;
	u32 time_arg;
};

struct goldfish_sync_guestcmd {
	u64 host_command;
	u64 glsync_handle;
	u64 thread_handle;
	u64 guest_timeline_handle;
};

#define GOLDFISH_SYNC_MAX_CMDS 32

struct goldfish_sync_state {
	char __iomem *reg_base;
	int irq;

	/* Spinlock protects |to_do| / |to_do_end|. */
	spinlock_t lock;

	/* |mutex_lock| protects all concurrent access
	 * to timelines for both kernel and user space.
	 */
	struct mutex mutex_lock;

	/* Buffer holding commands issued from host. */
	struct goldfish_sync_hostcmd to_do[GOLDFISH_SYNC_MAX_CMDS];
	u32 to_do_end;

	/* Addresses for the reading or writing
	 * of individual commands. The host can directly write
	 * to |batch_hostcmd| (and then this driver immediately
	 * copies contents to |to_do|). This driver either replies
	 * through |batch_hostcmd| or simply issues a
	 * guest->host command through |batch_guestcmd|.
	 */
	struct goldfish_sync_hostcmd *batch_hostcmd;
	struct goldfish_sync_guestcmd *batch_guestcmd;

	/* Used to give this struct itself to a work queue
	 * function for executing actual sync commands.
	 */
	struct work_struct work_item;

	/* A pointer to struct device to use for logging */
	struct device *dev;
};

static struct goldfish_sync_state global_sync_state;

struct goldfish_sync_timeline_obj {
	struct goldfish_sync_timeline *sync_tl;
	u32 current_time;
	/* We need to be careful about when we deallocate
	 * this |goldfish_sync_timeline_obj| struct.
	 * In order to ensure proper cleanup, we need to
	 * consider the triggered host-side wait that may
	 * still be in flight when the guest close()'s a
	 * goldfish_sync device's sync context fd (and
	 * destroys the |sync_tl| field above).
	 * The host-side wait may raise IRQ
	 * and tell the kernel to increment the timeline _after_
	 * the |sync_tl| has already been set to null.
	 *
	 * From observations on OpenGL apps and CTS tests, this
	 * happens at some very low probability upon context
	 * destruction or process close, but it does happen
	 * and it needs to be handled properly. Otherwise,
	 * if we clean up the surrounding |goldfish_sync_timeline_obj|
	 * too early, any |handle| field of any host->guest command
	 * might not even point to a null |sync_tl| field,
	 * but to garbage memory or even a reclaimed |sync_tl|.
	 * If we do not count such "pending waits" and kfree the object
	 * immediately upon |goldfish_sync_timeline_destroy|,
	 * we might get mysterous RCU stalls after running a long
	 * time because the garbage memory that is being read
	 * happens to be interpretable as a |spinlock_t| struct
	 * that is currently in the locked state.
	 *
	 * To track when to free the |goldfish_sync_timeline_obj|
	 * itself, we maintain a kref.
	 * The kref essentially counts the timeline itself plus
	 * the number of waits in flight. kref_init/kref_put
	 * are issued on
	 * |goldfish_sync_timeline_create|/|goldfish_sync_timeline_destroy|
	 * and kref_get/kref_put are issued on
	 * |goldfish_sync_fence_create|/|goldfish_sync_timeline_inc|.
	 *
	 * The timeline is destroyed after reference count
	 * reaches zero, which would happen after
	 * |goldfish_sync_timeline_destroy| and all pending
	 * |goldfish_sync_timeline_inc|'s are fulfilled.
	 *
	 * NOTE (1): We assume that |fence_create| and
	 * |timeline_inc| calls are 1:1, otherwise the kref scheme
	 * will not work. This is a valid assumption as long
	 * as the host-side virtual device implementation
	 * does not insert any timeline increments
	 * that we did not trigger from here.
	 *
	 * NOTE (2): The use of kref by itself requires no locks,
	 * but this does not mean everything works without locks.
	 * Related timeline operations do require a lock of some sort,
	 * or at least are not proven to work without it.
	 * In particualr, we assume that all the operations
	 * done on the |kref| field above are done in contexts where
	 * |global_sync_state->mutex_lock| is held. Do not
	 * remove that lock until everything is proven to work
	 * without it!!!
	 */
	struct kref kref;
};

/* We will call |delete_timeline_obj| when the last reference count
 * of the kref is decremented. This deletes the sync
 * timeline object along with the wrapper itself.
 */
static void delete_timeline_obj(struct kref *kref)
{
	struct goldfish_sync_timeline_obj *obj =
		container_of(kref, struct goldfish_sync_timeline_obj, kref);

	goldfish_sync_timeline_put_internal(obj->sync_tl);
	obj->sync_tl = NULL;
	kfree(obj);
}

static void gensym(char *dst, int size)
{
	static u64 counter;

	snprintf(dst, size, "goldfish_sync:%s:%llu", __func__, counter);
	counter++;
}

/* |goldfish_sync_timeline_create| assumes that |global_sync_state->mutex_lock|
 * is held.
 */
static struct goldfish_sync_timeline_obj*
goldfish_sync_timeline_create(struct goldfish_sync_state *sync_state)
{
	char timeline_name[64];
	struct goldfish_sync_timeline *res_sync_tl = NULL;
	struct goldfish_sync_timeline_obj *res;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	gensym(timeline_name, sizeof(timeline_name));

	res_sync_tl = goldfish_sync_timeline_create_internal(timeline_name);
	if (!res_sync_tl) {
		dev_err(sync_state->dev,
			"Failed to create goldfish_sw_sync timeline\n");
		return NULL;
	}

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	res->sync_tl = res_sync_tl;
	res->current_time = 0;
	kref_init(&res->kref);

	return res;
}

/* |goldfish_sync_fence_create| assumes that |global_sync_state->mutex_lock|
 * is held.
 */
static int
goldfish_sync_fence_create(struct goldfish_sync_state *sync_state,
			   struct goldfish_sync_timeline_obj *obj,
			   u32 val)
{
	int fd;
	struct sync_pt *syncpt;
	struct sync_file *sync_file_obj;
	struct goldfish_sync_timeline *tl;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	if (!obj)
		return -1;

	tl = obj->sync_tl;

	syncpt =
		goldfish_sync_pt_create_internal(tl,
						 sizeof(struct sync_pt) + 4,
						 val);
	if (!syncpt) {
		dev_err(sync_state->dev,
			"Could not create sync point, val=%d\n", val);
		return -1;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		dev_err(sync_state->dev,
			"Could not get unused fd for sync fence, "
			"errno=%d\n", fd);
		goto err_cleanup_pt;
	}

	sync_file_obj = sync_file_create(&syncpt->base);
	if (!sync_file_obj) {
		dev_err(sync_state->dev,
			"Could not create sync fence! val=%d\n", val);
		goto err_cleanup_fd_pt;
	}

	dev_info(sync_state->dev, "Installing sync fence into fd=%d\n", fd);
	fd_install(fd, sync_file_obj->file);
	kref_get(&obj->kref);

	return fd;

err_cleanup_fd_pt:
	put_unused_fd(fd);
err_cleanup_pt:
	dma_fence_put(&syncpt->base);
	return -1;
}

/* |goldfish_sync_timeline_inc| assumes that |global_sync_state->mutex_lock|
 * is held.
 */
static void
goldfish_sync_timeline_inc(struct goldfish_sync_state *sync_state,
			   struct goldfish_sync_timeline_obj *obj, u32 inc)
{
	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	/* Just give up if someone else nuked the timeline.
	 * Whoever it was won't care that it doesn't get signaled.
	 */
	if (!obj)
		return;

	goldfish_sync_timeline_signal_internal(obj->sync_tl, inc);
	dev_info(sync_state->dev, "Incremented timeline, increment max_time\n");
	obj->current_time += inc;

	/* Here, we will end up deleting the timeline object if it
	 * turns out that this call was a pending increment after
	 * |goldfish_sync_timeline_destroy| was called.
	 */
	kref_put(&obj->kref, delete_timeline_obj);
	dev_info(sync_state->dev, "done\n");
}

/* |goldfish_sync_timeline_destroy| assumes
 * that |global_sync_state->mutex_lock| is held.
 */
static void
goldfish_sync_timeline_destroy(struct goldfish_sync_state *sync_state,
			       struct goldfish_sync_timeline_obj *obj)
{
	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);
	/* See description of |goldfish_sync_timeline_obj| for why we
	 * should not immediately destroy |obj|
	 */
	kref_put(&obj->kref, delete_timeline_obj);
}

static int
goldfish_sync_cmd_queue(struct goldfish_sync_state *sync_state,
			u32 cmd,
			u64 handle,
			u32 time_arg,
			u64 hostcmd_handle)
{
	u32 to_do_end = sync_state->to_do_end;
	struct goldfish_sync_hostcmd *to_add;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	if (to_do_end >= GOLDFISH_SYNC_MAX_CMDS)
		return -1;

	to_add = &sync_state->to_do[to_do_end];

	to_add->cmd = cmd;
	to_add->handle = handle;
	to_add->time_arg = time_arg;
	to_add->hostcmd_handle = hostcmd_handle;

	sync_state->to_do_end = to_do_end + 1;
	return 0;
}

static void
goldfish_sync_hostcmd_reply(struct goldfish_sync_state *sync_state,
			    u32 cmd,
			    u64 handle,
			    u32 time_arg,
			    u64 hostcmd_handle)
{
	struct goldfish_sync_hostcmd *batch_hostcmd =
		sync_state->batch_hostcmd;
	unsigned long irq_flags;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	spin_lock_irqsave(&sync_state->lock, irq_flags);

	batch_hostcmd->cmd = cmd;
	batch_hostcmd->handle = handle;
	batch_hostcmd->time_arg = time_arg;
	batch_hostcmd->hostcmd_handle = hostcmd_handle;
	writel(0, sync_state->reg_base + SYNC_REG_BATCH_COMMAND);

	spin_unlock_irqrestore(&sync_state->lock, irq_flags);
}

static void
goldfish_sync_send_guestcmd(struct goldfish_sync_state *sync_state,
			    u32 cmd,
			    u64 glsync_handle,
			    u64 thread_handle,
			    u64 timeline_handle)
{
	unsigned long irq_flags;
	struct goldfish_sync_guestcmd *batch_guestcmd =
		sync_state->batch_guestcmd;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	spin_lock_irqsave(&sync_state->lock, irq_flags);

	batch_guestcmd->host_command = cmd;
	batch_guestcmd->glsync_handle = glsync_handle;
	batch_guestcmd->thread_handle = thread_handle;
	batch_guestcmd->guest_timeline_handle = timeline_handle;
	writel(0, sync_state->reg_base + SYNC_REG_BATCH_GUESTCOMMAND);

	spin_unlock_irqrestore(&sync_state->lock, irq_flags);
}

/* |goldfish_sync_interrupt| handles IRQ raises from the virtual device.
 * In the context of OpenGL, this interrupt will fire whenever we need
 * to signal a fence fd in the guest, with the command
 * |CMD_SYNC_TIMELINE_INC|.
 * However, because this function will be called in an interrupt context,
 * it is necessary to do the actual work of signaling off of interrupt context.
 * The shared work queue is used for this purpose. At the end when
 * all pending commands are intercepted by the interrupt handler,
 * we call |schedule_work|, which will later run the actual
 * desired sync command in |goldfish_sync_work_item_fn|.
 */
static irqreturn_t goldfish_sync_interrupt(int irq, void *dev_id)
{
	struct goldfish_sync_state *sync_state = dev_id;
	int has_cmds = 0;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	spin_lock(&sync_state->lock);
	while (true) {
		struct goldfish_sync_hostcmd *batch_hostcmd;
		u32 nextcmd;

		readl(sync_state->reg_base + SYNC_REG_BATCH_COMMAND);
		batch_hostcmd = sync_state->batch_hostcmd;
		nextcmd = batch_hostcmd->cmd;

		if (nextcmd == CMD_SYNC_READY)
			break;

		if (goldfish_sync_cmd_queue(sync_state,
					    nextcmd,
					    batch_hostcmd->handle,
					    batch_hostcmd->time_arg,
					    batch_hostcmd->hostcmd_handle))
			break;

		has_cmds = 1;
	}
	spin_unlock(&sync_state->lock);

	if (has_cmds) {
		schedule_work(&sync_state->work_item);
		return IRQ_HANDLED;
	} else {
		return IRQ_NONE;
	}
}

static u32 get_commands_todo_locked(struct goldfish_sync_state *sync_state,
				    struct goldfish_sync_hostcmd *to_run)
{
	unsigned long irq_flags;
	u32 to_do_end;
	u32 i;

	spin_lock_irqsave(&sync_state->lock, irq_flags);
	to_do_end = sync_state->to_do_end;

	dev_info(sync_state->dev, "Num sync todos: %u\n", to_do_end);

	for (i = 0; i < to_do_end; i++)
		to_run[i] = sync_state->to_do[i];

	/* We expect that commands will come in at a slow enough rate
	 * so that incoming items will not be more than
	 * GOLDFISH_SYNC_MAX_CMDS.
	 *
	 * This is because the way the sync device is used,
	 * it's only for managing buffer data transfers per frame,
	 * with a sequential dependency between putting things in
	 * to_do and taking them out. Once a set of commands is
	 * queued up in to_do, the user of the device waits for
	 * them to be processed before queuing additional commands,
	 * which limits the rate at which commands come in
	 * to the rate at which we take them out here.
	 *
	 * We also don't expect more than MAX_CMDS to be issued
	 * at once; there is a correspondence between
	 * which buffers need swapping to the (display / buffer queue)
	 * to particular commands, and we don't expect there to be
	 * enough display or buffer queues in operation at once
	 * to overrun GOLDFISH_SYNC_MAX_CMDS.
	 */
	sync_state->to_do_end = 0;

	spin_unlock_irqrestore(&sync_state->lock, irq_flags);

	return to_do_end;
}

void run_command_locked(const struct goldfish_sync_hostcmd *todo,
			struct goldfish_sync_state *sync_state)
{
	switch (todo->cmd) {
	case CMD_SYNC_READY:
		break;

	case CMD_CREATE_SYNC_TIMELINE:
		dev_info(sync_state->dev, "CMD_CREATE_SYNC_TIMELINE\n");
		{
			struct goldfish_sync_timeline_obj *timeline =
				goldfish_sync_timeline_create(sync_state);

			goldfish_sync_hostcmd_reply(sync_state,
						    CMD_CREATE_SYNC_TIMELINE,
						    (u64)timeline,
						    0,
						    todo->hostcmd_handle);
		}
		break;

	case CMD_CREATE_SYNC_FENCE:
		dev_info(sync_state->dev, "CMD_CREATE_SYNC_FENCE\n");
		{
			struct goldfish_sync_timeline_obj *timeline =
				(struct goldfish_sync_timeline_obj *)
					todo->handle;

			int sync_fence_fd =
				goldfish_sync_fence_create(sync_state,
							   timeline,
							   todo->time_arg);

			goldfish_sync_hostcmd_reply(sync_state,
						    CMD_CREATE_SYNC_FENCE,
						    sync_fence_fd,
						    0,
						    todo->hostcmd_handle);
		}
		break;

	case CMD_SYNC_TIMELINE_INC:
		dev_info(sync_state->dev, "CMD_SYNC_TIMELINE_INC\n");
		{
			struct goldfish_sync_timeline_obj *timeline =
				(struct goldfish_sync_timeline_obj *)
					todo->handle;

			goldfish_sync_timeline_inc(sync_state,
						   timeline,
						   todo->time_arg);
		}
		break;

	case CMD_DESTROY_SYNC_TIMELINE:
		dev_info(sync_state->dev, "CMD_DESTROY_SYNC_TIMELINE\n");
		{
			struct goldfish_sync_timeline_obj *timeline =
				(struct goldfish_sync_timeline_obj *)
					todo->handle;

			goldfish_sync_timeline_destroy(sync_state, timeline);
		}
		break;

	default:
		dev_err(sync_state->dev, "Unexpected command: %u\n", todo->cmd);
		break;
	}

	dev_info(sync_state->dev, "Done executing sync command\n");
}

void run_commands_locked(struct goldfish_sync_state *sync_state,
			 struct goldfish_sync_hostcmd *to_run,
			 u32 to_do_end)
{
	u32 i;

	for (i = 0; i < to_do_end; ++i) {
		dev_info(sync_state->dev, "todo index: %u\n", i);
		run_command_locked(&to_run[i], sync_state);
	}
}

/* |goldfish_sync_work_item_fn| does the actual work of servicing
 * host->guest sync commands. This function is triggered whenever
 * the IRQ for the goldfish sync device is raised. Once it starts
 * running, it grabs the contents of the buffer containing the
 * commands it needs to execute (there may be multiple, because
 * our IRQ is active high and not edge triggered), and then
 * runs all of them one after the other.
 */
static void goldfish_sync_work_item_fn(struct work_struct *input)
{
	struct goldfish_sync_state *sync_state =
		container_of(input, struct goldfish_sync_state, work_item);
	struct goldfish_sync_hostcmd to_run[GOLDFISH_SYNC_MAX_CMDS];
	u32 to_do_end;

	mutex_lock(&sync_state->mutex_lock);
	to_do_end = get_commands_todo_locked(sync_state, to_run);
	run_commands_locked(sync_state, to_run, to_do_end);
	mutex_unlock(&sync_state->mutex_lock);
}

/* Guest-side interface: file operations */

/* Goldfish sync context and ioctl info.
 *
 * When a sync context is created by open()-ing the goldfish sync device, we
 * create a sync context (|goldfish_sync_context|).
 *
 * Currently, the only data required to track is the sync timeline itself
 * along with the current time, which are all packed up in the
 * |goldfish_sync_timeline_obj| field. We use a |goldfish_sync_context|
 * as the filp->private_data.
 *
 * Next, when a sync context user requests that work be queued and a fence
 * fd provided, we use the |goldfish_sync_ioctl_info| struct, which holds
 * information about which host handles to touch for this particular
 * queue-work operation. We need to know about the host-side sync thread
 * and the particular host-side GLsync object. We also possibly write out
 * a file descriptor.
 */
struct goldfish_sync_context {
	struct goldfish_sync_timeline_obj *timeline;
};

static int goldfish_sync_open(struct inode *inode, struct file *file)
{
	struct goldfish_sync_context *sync_context;

	dev_dbg(global_sync_state.dev, "%s:%d\n", __func__, __LINE__);

	mutex_lock(&global_sync_state.mutex_lock);

	sync_context = kzalloc(sizeof(*sync_context), GFP_ATOMIC);
	if (!sync_context) {
		mutex_unlock(&global_sync_state.mutex_lock);
		return -ENOMEM;
	}

	sync_context->timeline = NULL;

	file->private_data = sync_context;

	mutex_unlock(&global_sync_state.mutex_lock);

	return 0;
}

static int goldfish_sync_release(struct inode *inode, struct file *file)
{
	struct goldfish_sync_context *sync_context;

	dev_dbg(global_sync_state.dev, "%s:%d\n", __func__, __LINE__);

	mutex_lock(&global_sync_state.mutex_lock);

	sync_context = file->private_data;

	if (sync_context->timeline)
		goldfish_sync_timeline_destroy(&global_sync_state,
					       sync_context->timeline);

	kfree(sync_context);

	mutex_unlock(&global_sync_state.mutex_lock);

	return 0;
}

/* |goldfish_sync_ioctl| is the guest-facing interface of goldfish sync
 * and is used in conjunction with eglCreateSyncKHR to queue up the
 * actual work of waiting for the EGL sync command to complete,
 * possibly returning a fence fd to the guest.
 */
static long goldfish_sync_ioctl(struct file *file,
				unsigned int cmd,
				unsigned long arg)
{
	struct device *dev = global_sync_state.dev;
	struct goldfish_sync_context *sync_context_data;
	struct goldfish_sync_timeline_obj *timeline;
	struct goldfish_sync_ioctl_info ioctl_data;
	int fd_out;
	u32 current_time;

	sync_context_data = file->private_data;
	fd_out = -1;

	switch (cmd) {
	case GOLDFISH_SYNC_IOC_QUEUE_WORK:
		dev_info(dev, "exec GOLDFISH_SYNC_IOC_QUEUE_WORK\n");

		mutex_lock(&global_sync_state.mutex_lock);

		if (copy_from_user(&ioctl_data,
				   (void __user *)arg,
				   sizeof(ioctl_data))) {
			dev_err(dev,
				"Failed to copy memory for ioctl_data from user\n");
			mutex_unlock(&global_sync_state.mutex_lock);
			return -EFAULT;
		}

		if (ioctl_data.host_syncthread_handle_in == 0) {
			dev_err(dev, "Error: zero host syncthread handle\n");
			mutex_unlock(&global_sync_state.mutex_lock);
			return -EFAULT;
		}

		timeline = sync_context_data->timeline;
		if (!timeline) {
			dev_info(dev, "No timeline yet, create one\n");
			timeline = goldfish_sync_timeline_create(&global_sync_state);
			sync_context_data->timeline = timeline;
		}

		current_time = timeline->current_time + 1;

		fd_out = goldfish_sync_fence_create(&global_sync_state,
						    timeline,
						    current_time);
		dev_info(dev, "Created fence with fd %d and current time %u\n",
			 fd_out, current_time);

		ioctl_data.fence_fd_out = fd_out;

		if (copy_to_user((void __user *)arg,
				 &ioctl_data,
				 sizeof(ioctl_data))) {
			dev_err(dev, "copy_to_user failed\n");

			ksys_close(fd_out);
			/* We won't be doing an increment,
			 * kref_put immediately.
			 */
			kref_put(&timeline->kref, delete_timeline_obj);
			mutex_unlock(&global_sync_state.mutex_lock);
			return -EFAULT;
		}

		/* We are now about to trigger a host-side wait;
		 * accumulate on |pending_waits|.
		 */
		goldfish_sync_send_guestcmd(&global_sync_state,
					    CMD_TRIGGER_HOST_WAIT,
					ioctl_data.host_glsync_handle_in,
					ioctl_data.host_syncthread_handle_in,
					(u64)timeline);

		mutex_unlock(&global_sync_state.mutex_lock);
		return 0;

	default:
		dev_err(dev, "Unexpected ioctl command: %u\n", cmd);
		return -ENOTTY;
	}
}

static const struct file_operations goldfish_sync_fops = {
	.owner = THIS_MODULE,
	.open = goldfish_sync_open,
	.release = goldfish_sync_release,
	.unlocked_ioctl = goldfish_sync_ioctl,
	.compat_ioctl = goldfish_sync_ioctl,
};

static struct miscdevice goldfish_sync_device = {
	.name = "goldfish_sync",
	.fops = &goldfish_sync_fops,
};

static bool setup_verify_batch_cmd_addr(struct goldfish_sync_state *sync_state,
					void *batch_addr,
					u32 addr_offset,
					u32 addr_offset_high)
{
	u64 batch_addr_phys;
	u64 batch_addr_phys_test;

	if (!batch_addr) {
		dev_err(sync_state->dev,
			"Could not use batch command address\n");
		return false;
	}

	batch_addr_phys = virt_to_phys(batch_addr);
	gf_write_u64(batch_addr_phys,
		     sync_state->reg_base + addr_offset,
		     sync_state->reg_base + addr_offset_high);

	batch_addr_phys_test =
		gf_read_u64(sync_state->reg_base + addr_offset,
			    sync_state->reg_base + addr_offset_high);

	if (virt_to_phys(batch_addr) != batch_addr_phys_test) {
		dev_err(sync_state->dev, "Invalid batch command address\n");
		return false;
	}

	return true;
}

int goldfish_sync_probe(struct platform_device *pdev)
{
	struct goldfish_sync_state *sync_state = &global_sync_state;
	struct device *dev = &pdev->dev;
	struct resource *ioresource;
	int status;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	sync_state->dev = dev;
	sync_state->to_do_end = 0;

	spin_lock_init(&sync_state->lock);
	mutex_init(&sync_state->mutex_lock);

	platform_set_drvdata(pdev, sync_state);

	ioresource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!ioresource) {
		dev_err(dev, "platform_get_resource failed\n");
		return -ENODEV;
	}

	sync_state->reg_base =
		devm_ioremap(dev, ioresource->start, PAGE_SIZE);
	if (!sync_state->reg_base) {
		dev_err(dev, "devm_ioremap failed\n");
		return -ENOMEM;
	}

	sync_state->irq = platform_get_irq(pdev, 0);
	if (sync_state->irq < 0) {
		dev_err(dev, "platform_get_irq failed\n");
		return -ENODEV;
	}

	status = devm_request_irq(dev,
				  sync_state->irq,
				  goldfish_sync_interrupt,
				  IRQF_SHARED,
				  pdev->name,
				  sync_state);
	if (status) {
		dev_err(dev, "devm_request_irq failed\n");
		return -ENODEV;
	}

	INIT_WORK(&sync_state->work_item,
		  goldfish_sync_work_item_fn);

	misc_register(&goldfish_sync_device);

	/* Obtain addresses for batch send/recv of commands. */
	{
		struct goldfish_sync_hostcmd *batch_addr_hostcmd;
		struct goldfish_sync_guestcmd *batch_addr_guestcmd;

		batch_addr_hostcmd =
			devm_kzalloc(dev, sizeof(*batch_addr_hostcmd),
				     GFP_KERNEL);
		batch_addr_guestcmd =
			devm_kzalloc(dev, sizeof(*batch_addr_guestcmd),
				     GFP_KERNEL);

		if (!setup_verify_batch_cmd_addr(sync_state,
						 batch_addr_hostcmd,
					SYNC_REG_BATCH_COMMAND_ADDR,
					SYNC_REG_BATCH_COMMAND_ADDR_HIGH)) {
			dev_err(dev, "Could not setup batch command address\n");
			return -ENODEV;
		}

		if (!setup_verify_batch_cmd_addr(sync_state,
						 batch_addr_guestcmd,
					SYNC_REG_BATCH_GUESTCOMMAND_ADDR,
					SYNC_REG_BATCH_GUESTCOMMAND_ADDR_HIGH)) {
			dev_err(dev, "Could not setup batch guest command address\n");
			return -ENODEV;
		}

		sync_state->batch_hostcmd = batch_addr_hostcmd;
		sync_state->batch_guestcmd = batch_addr_guestcmd;
	}

	dev_info(dev, "goldfish_sync: Initialized goldfish sync device\n");

	writel(0, sync_state->reg_base + SYNC_REG_INIT);

	return 0;
}

static int goldfish_sync_remove(struct platform_device *pdev)
{
	struct goldfish_sync_state *sync_state = &global_sync_state;

	dev_dbg(sync_state->dev, "%s:%d\n", __func__, __LINE__);

	misc_deregister(&goldfish_sync_device);
	memset(sync_state, 0, sizeof(struct goldfish_sync_state));
	return 0;
}

static const struct of_device_id goldfish_sync_of_match[] = {
	{ .compatible = "google,goldfish-sync", },
	{},
};
MODULE_DEVICE_TABLE(of, goldfish_sync_of_match);

static const struct acpi_device_id goldfish_sync_acpi_match[] = {
	{ "GFSH0006", 0 },
	{ },
};

MODULE_DEVICE_TABLE(acpi, goldfish_sync_acpi_match);

static struct platform_driver goldfish_sync = {
	.probe = goldfish_sync_probe,
	.remove = goldfish_sync_remove,
	.driver = {
		.name = "goldfish_sync",
		.of_match_table = goldfish_sync_of_match,
		.acpi_match_table = ACPI_PTR(goldfish_sync_acpi_match),
	}
};

module_platform_driver(goldfish_sync);

MODULE_AUTHOR("Google, Inc.");
MODULE_DESCRIPTION("Android QEMU Sync Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
