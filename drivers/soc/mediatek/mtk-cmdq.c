/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <soc/mediatek/cmdq.h>

#define CMDQ_THR_MAX_COUNT		3 /* main, sub, general(misc) */
#define CMDQ_INST_SIZE			8 /* instruction is 64-bit */
#define CMDQ_TIMEOUT_MS			1000
#define CMDQ_IRQ_MASK			0xffff
#define CMDQ_NUM_CMD(t)			(t->cmd_buf_size / CMDQ_INST_SIZE)

#define CMDQ_CURR_IRQ_STATUS		0x10
#define CMDQ_THR_SLOT_CYCLES		0x30

#define CMDQ_THR_BASE			0x100
#define CMDQ_THR_SIZE			0x80
#define CMDQ_THR_WARM_RESET		0x00
#define CMDQ_THR_ENABLE_TASK		0x04
#define CMDQ_THR_SUSPEND_TASK		0x08
#define CMDQ_THR_CURR_STATUS		0x0c
#define CMDQ_THR_IRQ_STATUS		0x10
#define CMDQ_THR_IRQ_ENABLE		0x14
#define CMDQ_THR_CURR_ADDR		0x20
#define CMDQ_THR_END_ADDR		0x24

#define CMDQ_THR_ENABLED		0x1
#define CMDQ_THR_DISABLED		0x0
#define CMDQ_THR_SUSPEND		0x1
#define CMDQ_THR_RESUME			0x0
#define CMDQ_THR_STATUS_SUSPENDED	BIT(1)
#define CMDQ_THR_DO_WARM_RESET		BIT(0)
#define CMDQ_THR_ACTIVE_SLOT_CYCLES	0x3200
#define CMDQ_THR_IRQ_DONE		0x1
#define CMDQ_THR_IRQ_ERROR		0x12
#define CMDQ_THR_IRQ_EN			(CMDQ_THR_IRQ_ERROR | CMDQ_THR_IRQ_DONE)

#define CMDQ_OP_CODE_SHIFT		24
#define CMDQ_SUBSYS_SHIFT		16

#define CMDQ_ARG_A_WRITE_MASK		0xffff
#define CMDQ_OP_CODE_MASK		(0xff << CMDQ_OP_CODE_SHIFT)

#define CMDQ_WRITE_ENABLE_MASK		BIT(0)
#define CMDQ_JUMP_BY_OFFSET		0x10000000
#define CMDQ_JUMP_BY_PA			0x10000001
#define CMDQ_JUMP_PASS			CMDQ_INST_SIZE
#define CMDQ_WFE_UPDATE			BIT(31)
#define CMDQ_WFE_WAIT			BIT(15)
#define CMDQ_WFE_WAIT_VALUE		0x1
#define CMDQ_EOC_IRQ_EN			BIT(0)

/*
 * CMDQ_CODE_MASK:
 *   set write mask
 *   format: op mask
 * CMDQ_CODE_WRITE:
 *   write value into target register
 *   format: op subsys address value
 * CMDQ_CODE_JUMP:
 *   jump by offset
 *   format: op offset
 * CMDQ_CODE_WFE:
 *   wait for event and clear
 *   it is just clear if no wait
 *   format: [wait]  op event update:1 to_wait:1 wait:1
 *           [clear] op event update:1 to_wait:0 wait:0
 * CMDQ_CODE_EOC:
 *   end of command
 *   format: op irq_flag
 */
enum cmdq_code {
	CMDQ_CODE_MASK = 0x02,
	CMDQ_CODE_WRITE = 0x04,
	CMDQ_CODE_JUMP = 0x10,
	CMDQ_CODE_WFE = 0x20,
	CMDQ_CODE_EOC = 0x40,
};

struct cmdq_task_cb {
	cmdq_async_flush_cb	cb;
	void			*data;
};

struct cmdq_thread {
	struct mbox_chan	*chan;
	void __iomem		*base;
	struct list_head	task_busy_list;
	struct timer_list	timeout;
	bool			atomic_exec;
};

struct cmdq_task {
	struct cmdq		*cmdq;
	struct list_head	list_entry;
	void			*va_base;
	dma_addr_t		pa_base;
	size_t			cmd_buf_size; /* command occupied size */
	size_t			buf_size; /* real buffer size */
	bool			finalized;
	struct cmdq_thread	*thread;
	struct cmdq_task_cb	cb;
};

struct cmdq_clk_release {
	struct cmdq		*cmdq;
	struct work_struct	release_work;
};

struct cmdq {
	struct mbox_controller	mbox;
	void __iomem		*base;
	u32			irq;
	struct workqueue_struct	*clk_release_wq;
	struct cmdq_thread	thread[CMDQ_THR_MAX_COUNT];
	struct mutex		task_mutex;
	struct clk		*clock;
	bool			suspended;
};

struct cmdq_subsys {
	u32	base;
	int	id;
};

static const struct cmdq_subsys gce_subsys[] = {
	{0x1400, 1},
	{0x1401, 2},
	{0x1402, 3},
};

static int cmdq_subsys_base_to_id(u32 base)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gce_subsys); i++)
		if (gce_subsys[i].base == base)
			return gce_subsys[i].id;
	return -EFAULT;
}

static int cmdq_thread_suspend(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	u32 status;

	writel(CMDQ_THR_SUSPEND, thread->base + CMDQ_THR_SUSPEND_TASK);

	/* If already disabled, treat as suspended successful. */
	if (!(readl(thread->base + CMDQ_THR_ENABLE_TASK) & CMDQ_THR_ENABLED))
		return 0;

	if (readl_poll_timeout_atomic(thread->base + CMDQ_THR_CURR_STATUS,
			status, status & CMDQ_THR_STATUS_SUSPENDED, 0, 10)) {
		dev_err(cmdq->mbox.dev, "suspend GCE thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}

	return 0;
}

static void cmdq_thread_resume(struct cmdq_thread *thread)
{
	writel(CMDQ_THR_RESUME, thread->base + CMDQ_THR_SUSPEND_TASK);
}

static int cmdq_thread_reset(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	u32 warm_reset;

	writel(CMDQ_THR_DO_WARM_RESET, thread->base + CMDQ_THR_WARM_RESET);
	if (readl_poll_timeout_atomic(thread->base + CMDQ_THR_WARM_RESET,
			warm_reset, !(warm_reset & CMDQ_THR_DO_WARM_RESET),
			0, 10)) {
		dev_err(cmdq->mbox.dev, "reset GCE thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}
	writel(CMDQ_THR_ACTIVE_SLOT_CYCLES, cmdq->base + CMDQ_THR_SLOT_CYCLES);
	return 0;
}

static void cmdq_thread_disable(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	cmdq_thread_reset(cmdq, thread);
	writel(CMDQ_THR_DISABLED, thread->base + CMDQ_THR_ENABLE_TASK);
}

/* notify GCE to re-fetch commands by setting GCE thread PC */
static void cmdq_thread_invalidate_fetched_data(struct cmdq_thread *thread)
{
	writel(readl(thread->base + CMDQ_THR_CURR_ADDR),
	       thread->base + CMDQ_THR_CURR_ADDR);
}

static void cmdq_task_insert_into_thread(struct cmdq_task *task)
{
	struct device *dev = task->cmdq->mbox.dev;
	struct cmdq_thread *thread = task->thread;
	struct cmdq_task *prev_task = list_last_entry(
			&thread->task_busy_list, typeof(*task), list_entry);
	u64 *prev_task_base = prev_task->va_base;

	/* let previous task jump to this task */
	dma_sync_single_for_cpu(dev, prev_task->pa_base,
				prev_task->cmd_buf_size, DMA_TO_DEVICE);
	prev_task_base[CMDQ_NUM_CMD(prev_task) - 1] =
		(u64)CMDQ_JUMP_BY_PA << 32 | task->pa_base;
	dma_sync_single_for_device(dev, prev_task->pa_base,
				   prev_task->cmd_buf_size, DMA_TO_DEVICE);

	cmdq_thread_invalidate_fetched_data(thread);
}

static bool cmdq_command_is_wfe(u64 cmd)
{
	u64 wfe_option = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT | CMDQ_WFE_WAIT_VALUE;
	u64 wfe_op = (u64)(CMDQ_CODE_WFE << CMDQ_OP_CODE_SHIFT) << 32;
	u64 wfe_mask = (u64)CMDQ_OP_CODE_MASK << 32 | 0xffffffff;

	return ((cmd & wfe_mask) == (wfe_op | wfe_option));
}

/* we assume tasks in the same display GCE thread are waiting the same event. */
static void cmdq_task_remove_wfe(struct cmdq_task *task)
{
	struct device *dev = task->cmdq->mbox.dev;
	u64 *base = task->va_base;
	int i;

	dma_sync_single_for_cpu(dev, task->pa_base, task->cmd_buf_size,
				DMA_TO_DEVICE);
	for (i = 0; i < CMDQ_NUM_CMD(task); i++)
		if (cmdq_command_is_wfe(base[i]))
			base[i] = (u64)CMDQ_JUMP_BY_OFFSET << 32 |
				  CMDQ_JUMP_PASS;
	dma_sync_single_for_device(dev, task->pa_base, task->cmd_buf_size,
				   DMA_TO_DEVICE);
}

static bool cmdq_thread_is_in_wfe(struct cmdq_thread *thread,
				  unsigned long curr_pa)
{
	struct device *dev = thread->chan->mbox->dev;
	struct cmdq_task *task;
	u32 task_end_pa;
	u64 *va;
	bool ret;

	task = list_first_entry(&thread->task_busy_list, struct cmdq_task,
				list_entry);
	task_end_pa = task->pa_base + task->cmd_buf_size;
	if (!(curr_pa >= task->pa_base &&
	      curr_pa < task_end_pa - CMDQ_INST_SIZE))
		return false;

	va = task->va_base + (curr_pa - task->pa_base);
	dma_sync_single_for_cpu(dev, task->pa_base, task->cmd_buf_size,
				DMA_TO_DEVICE);
	ret = cmdq_command_is_wfe(*va);
	dma_sync_single_for_device(dev, task->pa_base, task->cmd_buf_size,
				   DMA_TO_DEVICE);
	return ret;
}

static void cmdq_thread_wait_end(struct cmdq_thread *thread,
				 unsigned long end_pa)
{
	struct device *dev = thread->chan->mbox->dev;
	unsigned long curr_pa;

	if (readl_poll_timeout_atomic(thread->base + CMDQ_THR_CURR_ADDR,
			curr_pa, curr_pa == end_pa, 1, 20))
		dev_err(dev, "GCE thread cannot run to end.\n");
}

static void cmdq_task_exec(struct cmdq_task *task, struct cmdq_thread *thread)
{
	struct cmdq *cmdq = task->cmdq;
	unsigned long curr_pa, end_pa, flags;

	task->thread = thread;
	if (list_empty(&thread->task_busy_list)) {
		/*
		 * Unlock for clk prepare (sleeping function).
		 * We are safe to do that since we have task_mutex and
		 * only flush will add task.
		 */
		spin_unlock_irqrestore(&thread->chan->lock, flags);
		WARN_ON(clk_prepare_enable(cmdq->clock) < 0);
		spin_lock_irqsave(&thread->chan->lock, flags);

		WARN_ON(cmdq_thread_reset(cmdq, thread) < 0);

		writel(task->pa_base, thread->base + CMDQ_THR_CURR_ADDR);
		writel(task->pa_base + task->cmd_buf_size,
		       thread->base + CMDQ_THR_END_ADDR);
		writel(CMDQ_THR_IRQ_EN, thread->base + CMDQ_THR_IRQ_ENABLE);
		writel(CMDQ_THR_ENABLED, thread->base + CMDQ_THR_ENABLE_TASK);

		mod_timer(&thread->timeout,
			  jiffies + msecs_to_jiffies(CMDQ_TIMEOUT_MS));
	} else {
		WARN_ON(cmdq_thread_suspend(cmdq, thread) < 0);
		curr_pa = readl(thread->base + CMDQ_THR_CURR_ADDR);
		end_pa = readl(thread->base + CMDQ_THR_END_ADDR);

		/*
		 * Atomic exection should remove the following wfe, i.e. only
		 * wait event at first task, and prevent to pause when running.
		 */
		if (thread->atomic_exec) {
			/* GCE is executing if command is not WFE */
			if (!cmdq_thread_is_in_wfe(thread, curr_pa)) {
				cmdq_thread_resume(thread);
				cmdq_thread_wait_end(thread, end_pa);
				WARN_ON(cmdq_thread_suspend(cmdq, thread) < 0);
				/* set to this task directly */
				writel(task->pa_base,
				       thread->base + CMDQ_THR_CURR_ADDR);
			} else {
				cmdq_task_insert_into_thread(task);
				cmdq_task_remove_wfe(task);
				smp_mb(); /* modify jump before enable thread */
			}
		} else {
			/* check boundary */
			if (curr_pa == end_pa - CMDQ_INST_SIZE ||
			    curr_pa == end_pa) {
				/* set to this task directly */
				writel(task->pa_base,
				       thread->base + CMDQ_THR_CURR_ADDR);
			} else {
				cmdq_task_insert_into_thread(task);
				smp_mb(); /* modify jump before enable thread */
			}
		}
		writel(task->pa_base + task->cmd_buf_size,
		       thread->base + CMDQ_THR_END_ADDR);
		cmdq_thread_resume(thread);
	}
	list_move_tail(&task->list_entry, &thread->task_busy_list);
}

static void cmdq_task_exec_done(struct cmdq_task *task, bool err)
{
	struct device *dev = task->cmdq->mbox.dev;
	struct cmdq_cb_data cmdq_cb_data;

	if (task->cb.cb) {
		cmdq_cb_data.err = err;
		cmdq_cb_data.data = task->cb.data;
		task->cb.cb(cmdq_cb_data);
	}
	list_del(&task->list_entry);
	dma_unmap_single(dev, task->pa_base, task->cmd_buf_size, DMA_TO_DEVICE);
	kfree(task->va_base);
}

static void cmdq_task_handle_error(struct cmdq_task *task)
{
	struct cmdq_thread *thread = task->thread;
	struct cmdq_task *next_task;

	dev_err(task->cmdq->mbox.dev, "task 0x%p error\n", task);
	WARN_ON(cmdq_thread_suspend(task->cmdq, thread) < 0);
	next_task = list_first_entry_or_null(&thread->task_busy_list,
			struct cmdq_task, list_entry);
	if (next_task)
		writel(next_task->pa_base, thread->base + CMDQ_THR_CURR_ADDR);
	cmdq_thread_resume(thread);
}

static void cmdq_clk_release_work(struct work_struct *work_item)
{
	struct cmdq_clk_release *clk_release = container_of(work_item,
			struct cmdq_clk_release, release_work);
	struct cmdq *cmdq = clk_release->cmdq;

	clk_disable_unprepare(cmdq->clock);
	kfree(clk_release);
}

static void cmdq_clk_release_schedule(struct cmdq *cmdq)
{
	struct cmdq_clk_release *clk_release;

	clk_release = kmalloc(sizeof(*clk_release), GFP_ATOMIC);
	clk_release->cmdq = cmdq;
	INIT_WORK(&clk_release->release_work, cmdq_clk_release_work);
	queue_work(cmdq->clk_release_wq, &clk_release->release_work);
}

static void cmdq_thread_irq_handler(struct cmdq *cmdq,
				    struct cmdq_thread *thread)
{
	struct cmdq_task *task, *tmp, *curr_task = NULL;
	u32 curr_pa, irq_flag, task_end_pa;
	bool err;

	irq_flag = readl(thread->base + CMDQ_THR_IRQ_STATUS);
	writel(~irq_flag, thread->base + CMDQ_THR_IRQ_STATUS);

	/*
	 * When ISR call this function, another CPU core could run
	 * "release task" right before we acquire the spin lock, and thus
	 * reset / disable this GCE thread, so we need to check the enable
	 * bit of this GCE thread.
	 */
	if (!(readl(thread->base + CMDQ_THR_ENABLE_TASK) & CMDQ_THR_ENABLED))
		return;

	if (irq_flag & CMDQ_THR_IRQ_ERROR)
		err = true;
	else if (irq_flag & CMDQ_THR_IRQ_DONE)
		err = false;
	else
		return;

	curr_pa = readl(thread->base + CMDQ_THR_CURR_ADDR);

	list_for_each_entry_safe(task, tmp, &thread->task_busy_list,
				 list_entry) {
		task_end_pa = task->pa_base + task->cmd_buf_size;
		if (curr_pa >= task->pa_base && curr_pa < task_end_pa)
			curr_task = task;

		if (!curr_task || curr_pa == task_end_pa - CMDQ_INST_SIZE) {
			cmdq_task_exec_done(task, false);
			kfree(task);
		} else if (err) {
			cmdq_task_exec_done(task, true);
			cmdq_task_handle_error(curr_task);
			kfree(task);
		}

		if (curr_task)
			break;
	}

	if (list_empty(&thread->task_busy_list)) {
		cmdq_thread_disable(cmdq, thread);
		cmdq_clk_release_schedule(cmdq);
	} else {
		mod_timer(&thread->timeout,
			  jiffies + msecs_to_jiffies(CMDQ_TIMEOUT_MS));
	}
}

static irqreturn_t cmdq_irq_handler(int irq, void *dev)
{
	struct cmdq *cmdq = dev;
	unsigned long irq_status, flags = 0L;
	int bit;

	irq_status = readl(cmdq->base + CMDQ_CURR_IRQ_STATUS) & CMDQ_IRQ_MASK;
	if (!(irq_status ^ CMDQ_IRQ_MASK))
		return IRQ_NONE;

	for_each_clear_bit(bit, &irq_status, fls(CMDQ_IRQ_MASK)) {
		struct cmdq_thread *thread = &cmdq->thread[bit];

		spin_lock_irqsave(&thread->chan->lock, flags);
		cmdq_thread_irq_handler(cmdq, thread);
		spin_unlock_irqrestore(&thread->chan->lock, flags);
	}
	return IRQ_HANDLED;
}

static void cmdq_thread_handle_timeout(unsigned long data)
{
	struct cmdq_thread *thread = (struct cmdq_thread *)data;
	struct cmdq *cmdq = container_of(thread->chan->mbox, struct cmdq, mbox);
	struct cmdq_task *task, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&thread->chan->lock, flags);
	WARN_ON(cmdq_thread_suspend(cmdq, thread) < 0);

	/*
	 * Although IRQ is disabled, GCE continues to execute.
	 * It may have pending IRQ before GCE thread is suspended,
	 * so check this condition again.
	 */
	cmdq_thread_irq_handler(cmdq, thread);

	if (list_empty(&thread->task_busy_list)) {
		cmdq_thread_resume(thread);
		spin_unlock_irqrestore(&thread->chan->lock, flags);
		return;
	}

	dev_err(cmdq->mbox.dev, "timeout\n");
	list_for_each_entry_safe(task, tmp, &thread->task_busy_list,
				 list_entry) {
		cmdq_task_exec_done(task, true);
		kfree(task);
	}

	cmdq_thread_resume(thread);
	cmdq_thread_disable(cmdq, thread);
	cmdq_clk_release_schedule(cmdq);
	spin_unlock_irqrestore(&thread->chan->lock, flags);
}

static int cmdq_task_realloc_cmd_buffer(struct cmdq_task *task, size_t size)
{
	void *new_buf;

	new_buf = krealloc(task->va_base, size, GFP_KERNEL | __GFP_ZERO);
	if (!new_buf)
		return -ENOMEM;
	task->va_base = new_buf;
	task->buf_size = size;
	return 0;
}

struct cmdq_base *cmdq_register_device(struct device *dev)
{
	struct cmdq_base *cmdq_base;
	struct resource res;
	int subsys;
	u32 base;

	if (of_address_to_resource(dev->of_node, 0, &res))
		return NULL;
	base = (u32)res.start;

	subsys = cmdq_subsys_base_to_id(base >> 16);
	if (subsys < 0)
		return NULL;

	cmdq_base = devm_kmalloc(dev, sizeof(*cmdq_base), GFP_KERNEL);
	if (!cmdq_base)
		return NULL;
	cmdq_base->subsys = subsys;
	cmdq_base->base = base;

	return cmdq_base;
}
EXPORT_SYMBOL(cmdq_register_device);

struct cmdq_client *cmdq_mbox_create(struct device *dev, int index)
{
	struct cmdq_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	client->client.dev = dev;
	client->client.tx_block = false;
	client->chan = mbox_request_channel(&client->client, index);
	return client;
}
EXPORT_SYMBOL(cmdq_mbox_create);

int cmdq_task_create(struct device *dev, struct cmdq_task **task_ptr)
{
	struct cmdq_task *task;
	int err;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return -ENOMEM;
	task->cmdq = dev_get_drvdata(dev);
	err = cmdq_task_realloc_cmd_buffer(task, PAGE_SIZE);
	if (err < 0) {
		kfree(task);
		return err;
	}
	*task_ptr = task;
	return 0;
}
EXPORT_SYMBOL(cmdq_task_create);

static int cmdq_task_append_command(struct cmdq_task *task, enum cmdq_code code,
				    u32 arg_a, u32 arg_b)
{
	u64 *cmd_ptr;
	int err;

	if (WARN_ON(task->finalized))
		return -EBUSY;
	if (unlikely(task->cmd_buf_size + CMDQ_INST_SIZE > task->buf_size)) {
		err = cmdq_task_realloc_cmd_buffer(task, task->buf_size * 2);
		if (err < 0)
			return err;
	}
	cmd_ptr = task->va_base + task->cmd_buf_size;
	(*cmd_ptr) = (u64)((code << CMDQ_OP_CODE_SHIFT) | arg_a) << 32 | arg_b;
	task->cmd_buf_size += CMDQ_INST_SIZE;
	return 0;
}

int cmdq_task_write(struct cmdq_task *task, u32 value, struct cmdq_base *base,
		    u32 offset)
{
	u32 arg_a = ((base->base + offset) & CMDQ_ARG_A_WRITE_MASK) |
		    (base->subsys << CMDQ_SUBSYS_SHIFT);
	return cmdq_task_append_command(task, CMDQ_CODE_WRITE, arg_a, value);
}
EXPORT_SYMBOL(cmdq_task_write);

int cmdq_task_write_mask(struct cmdq_task *task, u32 value,
			 struct cmdq_base *base, u32 offset, u32 mask)
{
	u32 offset_mask = offset;
	int err;

	if (mask != 0xffffffff) {
		err = cmdq_task_append_command(task, CMDQ_CODE_MASK, 0, ~mask);
		if (err < 0)
			return err;
		offset_mask |= CMDQ_WRITE_ENABLE_MASK;
	}
	return cmdq_task_write(task, value, base, offset_mask);
}
EXPORT_SYMBOL(cmdq_task_write_mask);

static const u32 cmdq_event_value[CMDQ_MAX_EVENT] = {
	/* Display start of frame(SOF) events */
	[CMDQ_EVENT_DISP_OVL0_SOF] = 11,
	[CMDQ_EVENT_DISP_OVL1_SOF] = 12,
	[CMDQ_EVENT_DISP_RDMA0_SOF] = 13,
	[CMDQ_EVENT_DISP_RDMA1_SOF] = 14,
	[CMDQ_EVENT_DISP_RDMA2_SOF] = 15,
	[CMDQ_EVENT_DISP_WDMA0_SOF] = 16,
	[CMDQ_EVENT_DISP_WDMA1_SOF] = 17,
	/* Display end of frame(EOF) events */
	[CMDQ_EVENT_DISP_OVL0_EOF] = 39,
	[CMDQ_EVENT_DISP_OVL1_EOF] = 40,
	[CMDQ_EVENT_DISP_RDMA0_EOF] = 41,
	[CMDQ_EVENT_DISP_RDMA1_EOF] = 42,
	[CMDQ_EVENT_DISP_RDMA2_EOF] = 43,
	[CMDQ_EVENT_DISP_WDMA0_EOF] = 44,
	[CMDQ_EVENT_DISP_WDMA1_EOF] = 45,
	/* Mutex end of frame(EOF) events */
	[CMDQ_EVENT_MUTEX0_STREAM_EOF] = 53,
	[CMDQ_EVENT_MUTEX1_STREAM_EOF] = 54,
	[CMDQ_EVENT_MUTEX2_STREAM_EOF] = 55,
	[CMDQ_EVENT_MUTEX3_STREAM_EOF] = 56,
	[CMDQ_EVENT_MUTEX4_STREAM_EOF] = 57,
	/* Display underrun events */
	[CMDQ_EVENT_DISP_RDMA0_UNDERRUN] = 63,
	[CMDQ_EVENT_DISP_RDMA1_UNDERRUN] = 64,
	[CMDQ_EVENT_DISP_RDMA2_UNDERRUN] = 65,
};

int cmdq_task_wfe(struct cmdq_task *task, enum cmdq_event event)
{
	u32 arg_b;

	if (event >= CMDQ_MAX_EVENT || event < 0)
		return -EINVAL;

	/*
	 * WFE arg_b
	 * bit 0-11: wait value
	 * bit 15: 1 - wait, 0 - no wait
	 * bit 16-27: update value
	 * bit 31: 1 - update, 0 - no update
	 */
	arg_b = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT | CMDQ_WFE_WAIT_VALUE;
	return cmdq_task_append_command(task, CMDQ_CODE_WFE,
			cmdq_event_value[event], arg_b);
}
EXPORT_SYMBOL(cmdq_task_wfe);

int cmdq_task_clear_event(struct cmdq_task *task, enum cmdq_event event)
{
	if (event >= CMDQ_MAX_EVENT || event < 0)
		return -EINVAL;

	return cmdq_task_append_command(task, CMDQ_CODE_WFE,
			cmdq_event_value[event], CMDQ_WFE_UPDATE);
}
EXPORT_SYMBOL(cmdq_task_clear_event);

static int cmdq_task_finalize(struct cmdq_task *task)
{
	int err;

	if (task->finalized)
		return 0;

	/* insert EOC and generate IRQ for each command iteration */
	err = cmdq_task_append_command(task, CMDQ_CODE_EOC, 0, CMDQ_EOC_IRQ_EN);
	if (err < 0)
		return err;

	/* JUMP to end */
	err = cmdq_task_append_command(task, CMDQ_CODE_JUMP, 0, CMDQ_JUMP_PASS);
	if (err < 0)
		return err;

	task->finalized = true;
	return 0;
}

int cmdq_task_flush_async(struct cmdq_client *client, struct cmdq_task *task,
			  cmdq_async_flush_cb cb, void *data)
{
	struct cmdq *cmdq = task->cmdq;
	int err;

	mutex_lock(&cmdq->task_mutex);
	if (cmdq->suspended) {
		dev_err(cmdq->mbox.dev, "%s is called after suspended\n",
			__func__);
		mutex_unlock(&cmdq->task_mutex);
		return -EPERM;
	}

	err = cmdq_task_finalize(task);
	if (err < 0) {
		mutex_unlock(&cmdq->task_mutex);
		return err;
	}

	INIT_LIST_HEAD(&task->list_entry);
	task->cb.cb = cb;
	task->cb.data = data;
	task->pa_base = dma_map_single(cmdq->mbox.dev, task->va_base,
				       task->cmd_buf_size, DMA_TO_DEVICE);

	mbox_send_message(client->chan, task);
	/* We can send next task immediately, so just call txdone. */
	mbox_client_txdone(client->chan, 0);
	mutex_unlock(&cmdq->task_mutex);
	return 0;
}
EXPORT_SYMBOL(cmdq_task_flush_async);

struct cmdq_flush_completion {
	struct completion cmplt;
	bool err;
};

static void cmdq_task_flush_cb(struct cmdq_cb_data data)
{
	struct cmdq_flush_completion *cmplt = data.data;

	cmplt->err = data.err;
	complete(&cmplt->cmplt);
}

int cmdq_task_flush(struct cmdq_client *client, struct cmdq_task *task)
{
	struct cmdq_flush_completion cmplt;
	int err;

	init_completion(&cmplt.cmplt);
	err = cmdq_task_flush_async(client, task, cmdq_task_flush_cb, &cmplt);
	if (err < 0)
		return err;
	wait_for_completion(&cmplt.cmplt);
	return cmplt.err ? -EFAULT : 0;
}
EXPORT_SYMBOL(cmdq_task_flush);

void cmdq_mbox_free(struct cmdq_client *client)
{
	mbox_free_channel(client->chan);
	kfree(client);
}
EXPORT_SYMBOL(cmdq_mbox_free);

static int cmdq_suspend(struct device *dev)
{
	struct cmdq *cmdq = dev_get_drvdata(dev);
	struct cmdq_thread *thread;
	int i;
	bool task_running = false;

	mutex_lock(&cmdq->task_mutex);
	cmdq->suspended = true;
	mutex_unlock(&cmdq->task_mutex);

	for (i = 0; i < ARRAY_SIZE(cmdq->thread); i++) {
		thread = &cmdq->thread[i];
		if (!list_empty(&thread->task_busy_list)) {
			mod_timer(&thread->timeout, jiffies + 1);
			task_running = true;
		}
	}

	if (task_running) {
		dev_warn(dev, "exist running task(s) in suspend\n");
		msleep(20);
	}

	flush_workqueue(cmdq->clk_release_wq);
	return 0;
}

static int cmdq_resume(struct device *dev)
{
	struct cmdq *cmdq = dev_get_drvdata(dev);

	cmdq->suspended = false;
	return 0;
}

static int cmdq_remove(struct platform_device *pdev)
{
	struct cmdq *cmdq = platform_get_drvdata(pdev);

	destroy_workqueue(cmdq->clk_release_wq);
	mbox_controller_unregister(&cmdq->mbox);
	return 0;
}

static int cmdq_mbox_send_data(struct mbox_chan *chan, void *data)
{
	cmdq_task_exec(data, chan->con_priv);
	return 0;
}

static int cmdq_mbox_startup(struct mbox_chan *chan)
{
	return 0;
}

static void cmdq_mbox_shutdown(struct mbox_chan *chan)
{
}

static bool cmdq_mbox_last_tx_done(struct mbox_chan *chan)
{
	return true;
}

static const struct mbox_chan_ops cmdq_mbox_chan_ops = {
	.send_data = cmdq_mbox_send_data,
	.startup = cmdq_mbox_startup,
	.shutdown = cmdq_mbox_shutdown,
	.last_tx_done = cmdq_mbox_last_tx_done,
};

static struct mbox_chan *cmdq_xlate(struct mbox_controller *mbox,
		const struct of_phandle_args *sp)
{
	int ind = sp->args[0];
	struct cmdq_thread *thread;

	if (ind >= mbox->num_chans)
		return ERR_PTR(-EINVAL);

	thread = mbox->chans[ind].con_priv;
	thread->atomic_exec = (sp->args[1] != 0);
	thread->chan = &mbox->chans[ind];

	return &mbox->chans[ind];
}

static int cmdq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *res;
	struct cmdq *cmdq;
	int err, i;

	cmdq = devm_kzalloc(dev, sizeof(*cmdq), GFP_KERNEL);
	if (!cmdq)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cmdq->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(cmdq->base)) {
		dev_err(dev, "failed to ioremap gce\n");
		return PTR_ERR(cmdq->base);
	}

	cmdq->irq = irq_of_parse_and_map(node, 0);
	if (!cmdq->irq) {
		dev_err(dev, "failed to get irq\n");
		return -EINVAL;
	}
	err = devm_request_irq(dev, cmdq->irq, cmdq_irq_handler, IRQF_SHARED,
			       "mtk_cmdq", cmdq);
	if (err < 0) {
		dev_err(dev, "failed to register ISR (%d)\n", err);
		return err;
	}

	dev_dbg(dev, "cmdq device: addr:0x%p, va:0x%p, irq:%d\n",
		dev, cmdq->base, cmdq->irq);

	cmdq->clock = devm_clk_get(dev, "gce");
	if (IS_ERR(cmdq->clock)) {
		dev_err(dev, "failed to get gce clk\n");
		return PTR_ERR(cmdq->clock);
	}

	cmdq->mbox.dev = dev;
	cmdq->mbox.chans = devm_kcalloc(dev, CMDQ_THR_MAX_COUNT,
					sizeof(*cmdq->mbox.chans), GFP_KERNEL);
	if (!cmdq->mbox.chans)
		return -ENOMEM;

	cmdq->mbox.num_chans = CMDQ_THR_MAX_COUNT;
	cmdq->mbox.ops = &cmdq_mbox_chan_ops;
	cmdq->mbox.of_xlate = cmdq_xlate;

	/* make use of TXDONE_BY_ACK */
	cmdq->mbox.txdone_irq = false;
	cmdq->mbox.txdone_poll = false;

	for (i = 0; i < ARRAY_SIZE(cmdq->thread); i++) {
		cmdq->thread[i].base = cmdq->base + CMDQ_THR_BASE +
				CMDQ_THR_SIZE * i;
		INIT_LIST_HEAD(&cmdq->thread[i].task_busy_list);
		init_timer(&cmdq->thread[i].timeout);
		cmdq->thread[i].timeout.function = cmdq_thread_handle_timeout;
		cmdq->thread[i].timeout.data = (unsigned long)&cmdq->thread[i];
		cmdq->mbox.chans[i].con_priv = &cmdq->thread[i];
	}

	err = mbox_controller_register(&cmdq->mbox);
	if (err < 0) {
		dev_err(dev, "failed to register mailbox: %d\n", err);
		return err;
	}

	mutex_init(&cmdq->task_mutex);

	cmdq->clk_release_wq = alloc_ordered_workqueue(
			"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI,
			"cmdq_clk_release");

	platform_set_drvdata(pdev, cmdq);

	return 0;
}

static const struct dev_pm_ops cmdq_pm_ops = {
	.suspend = cmdq_suspend,
	.resume = cmdq_resume,
};

static const struct of_device_id cmdq_of_ids[] = {
	{.compatible = "mediatek,mt8173-gce",},
	{}
};

static struct platform_driver cmdq_drv = {
	.probe = cmdq_probe,
	.remove = cmdq_remove,
	.driver = {
		.name = "mtk_cmdq",
		.owner = THIS_MODULE,
		.pm = &cmdq_pm_ops,
		.of_match_table = cmdq_of_ids,
	}
};

builtin_platform_driver(cmdq_drv);
