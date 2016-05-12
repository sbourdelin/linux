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

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <soc/mediatek/cmdq.h>

#define CMDQ_INITIAL_CMD_BLOCK_SIZE	PAGE_SIZE
#define CMDQ_INST_SIZE			8 /* instruction is 64-bit */

#define CMDQ_DEFAULT_TIMEOUT_MS		1000
#define CMDQ_ACQUIRE_THREAD_TIMEOUT_MS	5000

#define CMDQ_DRIVER_DEVICE_NAME		"mtk_cmdq"
#define CMDQ_CLK_NAME			"gce"

#define CMDQ_CURR_IRQ_STATUS_OFFSET	0x010
#define CMDQ_THR_SLOT_CYCLES_OFFSET	0x030

#define CMDQ_THR_BASE			0x100
#define CMDQ_THR_SHIFT			0x080
#define CMDQ_THR_WARM_RESET_OFFSET	0x00
#define CMDQ_THR_ENABLE_TASK_OFFSET	0x04
#define CMDQ_THR_SUSPEND_TASK_OFFSET	0x08
#define CMDQ_THR_CURR_STATUS_OFFSET	0x0c
#define CMDQ_THR_IRQ_STATUS_OFFSET	0x10
#define CMDQ_THR_IRQ_ENABLE_OFFSET	0x14
#define CMDQ_THR_CURR_ADDR_OFFSET	0x20
#define CMDQ_THR_END_ADDR_OFFSET	0x24
#define CMDQ_THR_CFG_OFFSET		0x40

#define CMDQ_IRQ_MASK			0xffff

#define CMDQ_THR_ENABLED		0x1
#define CMDQ_THR_DISABLED		0x0
#define CMDQ_THR_SUSPEND		0x1
#define CMDQ_THR_RESUME			0x0
#define CMDQ_THR_STATUS_SUSPENDED	BIT(1)
#define CMDQ_THR_WARM_RESET		BIT(0)
#define CMDQ_THR_SLOT_CYCLES		0x3200
#define CMDQ_THR_PRIORITY		3
#define CMDQ_THR_IRQ_DONE		0x1
#define CMDQ_THR_IRQ_ERROR		0x12
#define CMDQ_THR_IRQ_EN			0x13 /* done + error */
#define CMDQ_THR_IRQ_MASK		0x13
#define CMDQ_THR_EXECUTING		BIT(31)

#define CMDQ_ARG_A_WRITE_MASK		0xffff
#define CMDQ_SUBSYS_MASK		0x1f

#define CMDQ_OP_CODE_SHIFT		24
#define CMDQ_SUBSYS_SHIFT		16

#define CMDQ_JUMP_BY_OFFSET		0x10000000
#define CMDQ_JUMP_BY_PA			0x10000001
#define CMDQ_JUMP_PASS			CMDQ_INST_SIZE

#define CMDQ_WFE_UPDATE			BIT(31)
#define CMDQ_WFE_WAIT			BIT(15)
#define CMDQ_WFE_WAIT_VALUE		0x1

#define CMDQ_EOC_IRQ_EN			BIT(0)

#define CMDQ_ENABLE_MASK		BIT(0)

#define CMDQ_OP_CODE_MASK		0xff000000

enum cmdq_thread_index {
	CMDQ_THR_DISP_MAIN_IDX,	/* main */
	CMDQ_THR_DISP_SUB_IDX,	/* sub */
	CMDQ_THR_DISP_MISC_IDX,	/* misc */
	CMDQ_THR_MAX_COUNT,	/* max */
};

struct cmdq_command {
	struct cmdq	*cmdq;
	u64		engine_flag;
	void		*base; /* command buffer pointer */
	size_t		size; /* command buffer size (bytes) */
};

/*
 * CMDQ_CODE_MOVE:
 *   move value into internal register as mask
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
	CMDQ_CODE_MOVE = 0x02,
	CMDQ_CODE_WRITE = 0x04,
	CMDQ_CODE_JUMP = 0x10,
	CMDQ_CODE_WFE = 0x20,
	CMDQ_CODE_EOC = 0x40,
};

enum cmdq_task_state {
	TASK_STATE_WAITING,	/* allocated but waiting for available thread */
	TASK_STATE_BUSY,	/* task running on a thread */
	TASK_STATE_ERROR,	/* task execution error */
	TASK_STATE_DONE,	/* task finished */
};

struct cmdq_task_cb {
	cmdq_async_flush_cb	cb;
	void			*data;
};

struct cmdq_thread;

struct cmdq_task {
	struct cmdq		*cmdq;
	struct list_head	list_entry;
	enum cmdq_task_state	task_state;
	void			*va_base; /* va */
	dma_addr_t		mva_base; /* pa */
	u64			engine_flag;
	size_t			command_size;
	u32			num_cmd; /* 2 * number of commands */
	struct cmdq_thread	*thread;
	struct cmdq_task_cb	cb; /* callback */
	/* work item when auto release is used */
	struct work_struct	auto_release_work;
};

struct cmdq_thread {
	void __iomem		*base;
	u32			task_count;
	struct list_head	task_busy_list;
	wait_queue_head_t	wait_queue; /* wait task done */
};

struct cmdq {
	struct device		*dev;
	void __iomem		*base;
	u32			irq;

	/*
	 * task_cache: struct cmdq_task object cache
	 * task_consume_wait_queue_item: task consuming work
	 * task_consume_wq: task consuming workqueue
	 * task_auto_release_wq: auto-release workqueue
	 */
	struct kmem_cache	*task_cache;
	struct list_head	task_wait_list;
	struct work_struct	task_consume_wait_queue_item;
	struct workqueue_struct	*task_consume_wq;
	struct workqueue_struct	*task_auto_release_wq;

	struct cmdq_thread	thread[CMDQ_THR_MAX_COUNT];
	struct mutex		task_mutex;	/* for task list */
	spinlock_t		exec_lock;	/* for exec task */
	wait_queue_head_t	thread_dispatch_queue;
	struct clk		*clock;
};

struct cmdq_subsys {
	u32	base_addr;
	int	id;
};

static const struct cmdq_subsys g_subsys[] = {
	{0x1400, 1},
	{0x1401, 2},
	{0x1402, 3},
};

static int cmdq_clk_enable(struct cmdq *cmdq)
{
	struct device *dev = cmdq->dev;
	int err;

	err = clk_prepare_enable(cmdq->clock);
	if (err < 0) {
		dev_err(dev, "prepare and enable clk:%s fail\n",
			CMDQ_CLK_NAME);
		return err;
	}
	return 0;
}

static void cmdq_clk_disable(struct cmdq *cmdq)
{
	clk_disable_unprepare(cmdq->clock);
}

static int cmdq_subsys_base_addr_to_id(u32 base_addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(g_subsys); i++)
		if (g_subsys[i].base_addr == base_addr)
			return g_subsys[i].id;
	return -EFAULT;
}

static int cmdq_eng_get_thread(u64 flag)
{
	if (flag & BIT_ULL(CMDQ_ENG_DISP_DSI0))
		return CMDQ_THR_DISP_MAIN_IDX;
	else if (flag & BIT_ULL(CMDQ_ENG_DISP_DPI0))
		return CMDQ_THR_DISP_SUB_IDX;
	else
		return CMDQ_THR_DISP_MISC_IDX;
}

static void cmdq_task_release_internal(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;

	mutex_lock(&cmdq->task_mutex);
	dma_free_coherent(cmdq->dev, task->command_size, task->va_base,
			  task->mva_base);
	kmem_cache_free(cmdq->task_cache, task);
	mutex_unlock(&cmdq->task_mutex);
}

static struct cmdq_task *cmdq_task_acquire(struct cmdq_command *command,
					   struct cmdq_task_cb *cb)
{
	struct cmdq *cmdq = command->cmdq;
	struct device *dev = cmdq->dev;
	struct cmdq_task *task;

	mutex_lock(&cmdq->task_mutex);
	task = kmem_cache_zalloc(cmdq->task_cache, GFP_KERNEL);
	INIT_LIST_HEAD(&task->list_entry);
	task->va_base = dma_alloc_coherent(dev, command->size, &task->mva_base,
					   GFP_KERNEL);
	if (!task->va_base) {
		dev_err(dev, "allocate command buffer failed\n");
		kmem_cache_free(cmdq->task_cache, task);
		mutex_unlock(&cmdq->task_mutex);
		return NULL;
	}

	task->cmdq = cmdq;
	task->command_size = command->size;
	task->engine_flag = command->engine_flag;
	task->task_state = TASK_STATE_WAITING;
	if (cb)
		task->cb = *cb;
	memcpy(task->va_base, command->base, command->size);
	task->num_cmd = task->command_size / sizeof(u32);
	list_add_tail(&task->list_entry, &cmdq->task_wait_list);
	mutex_unlock(&cmdq->task_mutex);
	return task;
}

static void cmdq_thread_writel(struct cmdq_thread *thread, u32 value,
			       u32 offset)
{
	writel(value, thread->base + offset);
}

static u32 cmdq_thread_readl(struct cmdq_thread *thread, u32 offset)
{
	return readl(thread->base + offset);
}

static struct cmdq_thread *cmdq_thread_get(struct cmdq *cmdq, int tid)
{
	struct cmdq_thread *thread = &cmdq->thread[tid];

	cmdq_clk_enable(cmdq);
	return thread;
}

static void cmdq_thread_put(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	if (WARN_ON(thread == NULL))
		return;
	cmdq_clk_disable(cmdq);
}

static int cmdq_thread_suspend(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	u32 enabled;
	u32 status;

	/* write suspend bit */
	cmdq_thread_writel(thread, CMDQ_THR_SUSPEND,
			   CMDQ_THR_SUSPEND_TASK_OFFSET);

	/* If already disabled, treat as suspended successful. */
	enabled = cmdq_thread_readl(thread, CMDQ_THR_ENABLE_TASK_OFFSET);
	if (!(enabled & CMDQ_THR_ENABLED))
		return 0;

	/* poll suspended status */
	if (readl_poll_timeout_atomic(thread->base +
				      CMDQ_THR_CURR_STATUS_OFFSET,
				      status,
				      status & CMDQ_THR_STATUS_SUSPENDED,
				      0, 10)) {
		dev_err(cmdq->dev, "Suspend HW thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}

	return 0;
}

static void cmdq_thread_resume(struct cmdq_thread *thread)
{
	cmdq_thread_writel(thread, CMDQ_THR_RESUME,
			   CMDQ_THR_SUSPEND_TASK_OFFSET);
}

static int cmdq_thread_reset(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	void __iomem *gce_base = cmdq->base;
	u32 warm_reset;

	cmdq_thread_writel(thread, CMDQ_THR_WARM_RESET,
			   CMDQ_THR_WARM_RESET_OFFSET);
	if (readl_poll_timeout_atomic(thread->base + CMDQ_THR_WARM_RESET_OFFSET,
				      warm_reset,
				      !(warm_reset & CMDQ_THR_WARM_RESET),
				      0, 10)) {
		dev_err(cmdq->dev, "Reset HW thread 0x%x failed\n",
			(u32)(thread->base - cmdq->base));
		return -EFAULT;
	}
	writel(CMDQ_THR_SLOT_CYCLES, gce_base + CMDQ_THR_SLOT_CYCLES_OFFSET);
	return 0;
}

static void cmdq_thread_disable(struct cmdq *cmdq, struct cmdq_thread *thread)
{
	cmdq_thread_reset(cmdq, thread);
	cmdq_thread_writel(thread, CMDQ_THR_DISABLED,
			   CMDQ_THR_ENABLE_TASK_OFFSET);
}

/* notify GCE to re-fetch commands by setting HW thread PC */
static void cmdq_thread_invalidate_fetched_data(struct cmdq_thread *thread)
{
	cmdq_thread_writel(thread,
			   cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET),
			   CMDQ_THR_CURR_ADDR_OFFSET);
}

static void cmdq_task_insert_into_thread(struct cmdq_task *task)
{
	struct cmdq_thread *thread = task->thread;
	struct cmdq_task *prev_task = list_last_entry(
			&thread->task_busy_list, typeof(*task), list_entry);
	u32 *prev_task_base;

	/* insert task, and let previous task jump to this task */
	list_move_tail(&task->list_entry, &thread->task_busy_list);
	prev_task_base = prev_task->va_base;
	prev_task_base[prev_task->num_cmd - 1] = CMDQ_JUMP_BY_PA;
	prev_task_base[prev_task->num_cmd - 2] = task->mva_base;

	/* re-fetch command buffer */
	cmdq_thread_invalidate_fetched_data(thread);
}

/* we assume tasks in the same display thread are waiting the same event. */
static void cmdq_task_remove_wfe(struct cmdq_task *task)
{
	u32 wfe_option = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT | CMDQ_WFE_WAIT_VALUE;
	u32 wfe_op = CMDQ_CODE_WFE << CMDQ_OP_CODE_SHIFT;
	u32 *base = task->va_base;
	int i;

	for (i = 0; i < task->num_cmd; i += 2) {
		if (base[i] == wfe_option &&
		    (base[i + 1] & CMDQ_OP_CODE_MASK) == wfe_op) {
			base[i] = CMDQ_JUMP_PASS;
			base[i + 1] = CMDQ_JUMP_BY_OFFSET;
		}
	}
}

static int cmdq_task_exec_async(struct cmdq_task *task,
				struct cmdq_thread *thread)
{
	struct cmdq *cmdq = task->cmdq;
	unsigned long flags;

	spin_lock_irqsave(&cmdq->exec_lock, flags);
	task->thread = thread;
	task->task_state = TASK_STATE_BUSY;
	if (thread->task_count <= 0) {
		if (WARN_ON(cmdq_thread_reset(cmdq, thread) < 0)) {
			spin_unlock_irqrestore(&cmdq->exec_lock, flags);
			return -EFAULT;
		}

		cmdq_thread_writel(thread, task->mva_base,
				   CMDQ_THR_CURR_ADDR_OFFSET);
		cmdq_thread_writel(thread, task->mva_base + task->command_size,
				   CMDQ_THR_END_ADDR_OFFSET);
		cmdq_thread_writel(thread, CMDQ_THR_PRIORITY,
				   CMDQ_THR_CFG_OFFSET);
		cmdq_thread_writel(thread, CMDQ_THR_IRQ_EN,
				   CMDQ_THR_IRQ_ENABLE_OFFSET);

		list_move_tail(&task->list_entry,
			       &thread->task_busy_list);
		thread->task_count = 1;

		/* enable HW thread */
		cmdq_thread_writel(thread, CMDQ_THR_ENABLED,
				   CMDQ_THR_ENABLE_TASK_OFFSET);
	} else {
		unsigned long curr_pa, end_pa;
		int err;

		err = cmdq_thread_suspend(cmdq, thread);
		if (WARN_ON(err < 0)) {
			spin_unlock_irqrestore(&cmdq->exec_lock, flags);
			return err;
		}

		/*
		 * check boundary condition
		 * PC = END - 8, EOC is executed
		 * PC = END - 0, all CMDs are executed
		 */
		curr_pa = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);
		end_pa = cmdq_thread_readl(thread, CMDQ_THR_END_ADDR_OFFSET);
		if ((curr_pa == end_pa - 8) || (curr_pa == end_pa - 0)) {
			/* set to task directly */
			cmdq_thread_writel(thread, task->mva_base,
					   CMDQ_THR_CURR_ADDR_OFFSET);
			cmdq_thread_writel(thread,
					   task->mva_base + task->command_size,
					   CMDQ_THR_END_ADDR_OFFSET);
			list_move_tail(&task->list_entry,
				       &thread->task_busy_list);
			thread->task_count++;
		} else {
			cmdq_task_insert_into_thread(task);

			if (thread == &cmdq->thread[CMDQ_THR_DISP_MAIN_IDX] ||
			    thread == &cmdq->thread[CMDQ_THR_DISP_SUB_IDX])
				cmdq_task_remove_wfe(task);

			smp_mb(); /* modify jump before enable thread */
			cmdq_thread_writel(thread,
					   task->mva_base + task->command_size,
					   CMDQ_THR_END_ADDR_OFFSET);
			thread->task_count++;
		}

		cmdq_thread_resume(thread);
	}
	spin_unlock_irqrestore(&cmdq->exec_lock, flags);
	return 0;
}

static void cmdq_handle_error_done(struct cmdq *cmdq,
				   struct cmdq_thread *thread, bool err)
{
	struct cmdq_task *task, *tmp, *curr_task = NULL;
	u32 curr_pa = cmdq_thread_readl(thread, CMDQ_THR_CURR_ADDR_OFFSET);
	struct cmdq_cb_data cmdq_cb_data;

	list_for_each_entry_safe(task, tmp, &thread->task_busy_list,
				 list_entry) {
		if (curr_pa >= task->mva_base &&
		    curr_pa < (task->mva_base + task->command_size)) {
			curr_task = task;
			break;
		}

		if (task->cb.cb) {
			cmdq_cb_data.err = false;
			cmdq_cb_data.data = task->cb.data;
			task->cb.cb(cmdq_cb_data);
		}
		task->task_state = TASK_STATE_DONE;
		list_del(&task->list_entry);
		thread->task_count--;
	}

	if (curr_task) {
		if (curr_task->cb.cb) {
			cmdq_cb_data.err = err;
			cmdq_cb_data.data = curr_task->cb.data;
			curr_task->cb.cb(cmdq_cb_data);
		}
		if (err)
			task->task_state = TASK_STATE_ERROR;
		else
			task->task_state = TASK_STATE_DONE;
		list_del(&curr_task->list_entry);
		thread->task_count--;
	}

	wake_up(&thread->wait_queue);
}

static void cmdq_handle_irq(struct cmdq *cmdq, int tid)
{
	struct device *dev = cmdq->dev;
	struct cmdq_thread *thread = &cmdq->thread[tid];
	unsigned long flags = 0L;
	int value;
	int enabled;

	spin_lock_irqsave(&cmdq->exec_lock, flags);

	/*
	 * it is possible for another CPU core
	 * to run "release task" right before we acquire the spin lock
	 * and thus reset / disable this HW thread
	 * so we check both the IRQ flag and the enable bit of this thread
	 */
	value = cmdq_thread_readl(thread, CMDQ_THR_IRQ_STATUS_OFFSET);
	if (!(value & CMDQ_THR_IRQ_MASK)) {
		dev_err(dev,
			"IRQ: thread 0x%x got interrupt but IRQ flag=0x%x\n",
			(u32)(thread->base - cmdq->base), value);
		spin_unlock_irqrestore(&cmdq->exec_lock, flags);
		return;
	}

	enabled = cmdq_thread_readl(thread, CMDQ_THR_ENABLE_TASK_OFFSET);
	if (!(enabled & CMDQ_THR_ENABLED)) {
		dev_err(dev,
			"IRQ: thread 0x%x got interrupt but enabled=0x%x\n",
			(u32)(thread->base - cmdq->base), enabled);
		spin_unlock_irqrestore(&cmdq->exec_lock, flags);
		return;
	}

	/*
	 * Move the reset IRQ before read HW cookie
	 * to prevent race condition and save the cost of suspend
	 */
	cmdq_thread_writel(thread, ~value, CMDQ_THR_IRQ_STATUS_OFFSET);

	if (value & CMDQ_THR_IRQ_ERROR)
		cmdq_handle_error_done(cmdq, thread, true);
	else if (value & CMDQ_THR_IRQ_DONE)
		cmdq_handle_error_done(cmdq, thread, false);

	spin_unlock_irqrestore(&cmdq->exec_lock, flags);
}

static void cmdq_consume_waiting_list(struct work_struct *work)
{
	struct cmdq *cmdq = container_of(work, struct cmdq,
					 task_consume_wait_queue_item);
	struct device *dev = cmdq->dev;
	struct cmdq_task *task, *tmp;
	u32 err_bits = 0;
	const u32 disp_mask = BIT(CMDQ_THR_DISP_MAIN_IDX) |
			      BIT(CMDQ_THR_DISP_SUB_IDX) |
			      BIT(CMDQ_THR_DISP_MISC_IDX);

	mutex_lock(&cmdq->task_mutex);

	if (list_empty(&cmdq->task_wait_list)) {
		mutex_unlock(&cmdq->task_mutex);
		return;
	}

	list_for_each_entry_safe(task, tmp, &cmdq->task_wait_list, list_entry) {
		struct cmdq_thread *thread = NULL;
		int candidate_tid = cmdq_eng_get_thread(task->engine_flag);
		int err;

		/*
		 * Once waiting occur,
		 * skip following tasks to keep order of display tasks.
		 */
		if (err_bits & disp_mask & candidate_tid)
			continue;

		thread = cmdq_thread_get(cmdq, candidate_tid);
		err = cmdq_task_exec_async(task, thread);
		if (err < 0) {
			dev_warn(dev, "start task fail. wait\n");
			err_bits |= BIT(candidate_tid);
			continue;
		}
	}

	/*
	 * Wake up waiting task(s) whether success or not
	 * because wake up condition will check task's thread.
	 * (cmdq_task_wait_and_release)
	 */
	wake_up_all(&cmdq->thread_dispatch_queue);

	mutex_unlock(&cmdq->task_mutex);
}

static int cmdq_task_submit_async(struct cmdq_command *command,
				  struct cmdq_task **task_out,
				  struct cmdq_task_cb *cb)
{
	struct cmdq *cmdq = command->cmdq;

	/* creates a new task and put into tail of waiting list */
	*task_out = cmdq_task_acquire(command, cb);
	if (!(*task_out))
		return -EFAULT;
	/* Do consumption here to gain some time if HW thread is available. */
	cmdq_consume_waiting_list(&cmdq->task_consume_wait_queue_item);
	return 0;
}

static int cmdq_task_handle_error_result(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	struct cmdq_thread *thread = task->thread;
	int err;
	struct cmdq_task *next_task, *prev_task;
	u32 *prev_va, *curr_va;
	u32 irq_flag;

	dev_err(dev,
		"task(0x%p) state is not TASK_STATE_DONE, but %d.\n",
		task, task->task_state);

	/* suspend HW thread to ensure consistency */
	err = cmdq_thread_suspend(cmdq, thread);
	if (WARN_ON(err < 0))
		return err;

	/*
	 * Save next_task and prev_task in advance
	 * since cmdq_handle_error_done will remove list_entry.
	 */
	next_task = prev_task = NULL;
	if (task->list_entry.next != &thread->task_busy_list)
		next_task = list_next_entry(task, list_entry);
	if (task->list_entry.prev != &thread->task_busy_list)
		prev_task = list_prev_entry(task, list_entry);

	/*
	 * Although IRQ is disabled, GCE continues to execute.
	 * It may have pending IRQ before HW thread is suspended,
	 * so check this condition again.
	 */
	irq_flag = cmdq_thread_readl(thread, CMDQ_THR_IRQ_STATUS_OFFSET);
	if (irq_flag & CMDQ_THR_IRQ_ERROR)
		cmdq_handle_error_done(cmdq, thread, true);
	else if (irq_flag & CMDQ_THR_IRQ_DONE)
		cmdq_handle_error_done(cmdq, thread, false);
	cmdq_thread_writel(thread, ~irq_flag, CMDQ_THR_IRQ_STATUS_OFFSET);

	/* success after handling pending irq */
	if (task->task_state == TASK_STATE_DONE) {
		cmdq_thread_resume(thread);
		return 0;
	}

	/* error in this task */
	if (task->task_state == TASK_STATE_ERROR) {
		if (next_task) /* move to next task */
			cmdq_thread_writel(thread, next_task->mva_base,
					   CMDQ_THR_CURR_ADDR_OFFSET);
		cmdq_thread_resume(thread);
		return -ECANCELED;
	}

	/* If task is running, force to remove it. */
	dev_err(dev, "task 0x%p timeout or killed\n", task);

	if (task->task_state == TASK_STATE_BUSY)
		task->task_state = TASK_STATE_ERROR;

	if (prev_task) {
		u32 prev_num, curr_num;

		prev_va = prev_task->va_base;
		prev_num = prev_task->num_cmd;

		curr_va = task->va_base;
		curr_num = task->num_cmd;

		/* copy JUMP instruction */
		prev_va[prev_num - 2] = curr_va[curr_num - 2];
		prev_va[prev_num - 1] = curr_va[curr_num - 1];

		/* re-fetch command */
		cmdq_thread_invalidate_fetched_data(thread);
	} else if (next_task) { /* move to next task */
		cmdq_thread_writel(thread, next_task->mva_base,
				   CMDQ_THR_CURR_ADDR_OFFSET);
	}

	list_del(&task->list_entry);
	thread->task_count--;
	cmdq_thread_resume(thread);
	return -ECANCELED;
}

static int cmdq_task_wait_result(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct cmdq_thread *thread = task->thread;
	int err = 0;
	unsigned long flags;

	spin_lock_irqsave(&cmdq->exec_lock, flags);
	if (task->task_state != TASK_STATE_DONE)
		err = cmdq_task_handle_error_result(task);
	if (thread->task_count <= 0)
		cmdq_thread_disable(cmdq, thread);
	spin_unlock_irqrestore(&cmdq->exec_lock, flags);
	return err;
}

static int cmdq_task_wait_done(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;
	struct device *dev = cmdq->dev;
	int wait_q;
	unsigned long timeout = msecs_to_jiffies(
			CMDQ_ACQUIRE_THREAD_TIMEOUT_MS);

	/* wait for acquiring thread (cmdq_consume_waiting_list) */
	wait_q = wait_event_timeout(
			cmdq->thread_dispatch_queue,
			task->thread, timeout);
	if (!wait_q) {
		mutex_lock(&cmdq->task_mutex);
		/* Check if task was just consumed. */
		if (!task->thread) {
			dev_err(dev,
				"task(0x%p) timeout with invalid thread\n",
				task);
			/*
			 * Remove from waiting list,
			 * so it won't be consumed in the future.
			 */
			list_del_init(&task->list_entry);
			mutex_unlock(&cmdq->task_mutex);
			return -EINVAL;
		}
		/* valid thread, so we keep going */
		mutex_unlock(&cmdq->task_mutex);
	}

	/* wait for execution */
	wait_q = wait_event_timeout(task->thread->wait_queue,
				    (task->task_state != TASK_STATE_BUSY &&
				     task->task_state != TASK_STATE_WAITING),
				    msecs_to_jiffies(CMDQ_DEFAULT_TIMEOUT_MS));
	if (!wait_q)
		dev_dbg(dev, "timeout!\n");

	/* wake up and continue */
	return cmdq_task_wait_result(task);
}

static int cmdq_task_wait_and_release(struct cmdq_task *task)
{
	int err = cmdq_task_wait_done(task);

	/* release regardless of success or not */
	cmdq_thread_put(task->cmdq, task->thread);
	cmdq_task_release_internal(task);
	return err;
}

static void cmdq_auto_release(struct work_struct *work_item)
{
	struct cmdq_task *task = container_of(work_item, struct cmdq_task,
					      auto_release_work);
	struct cmdq *cmdq = task->cmdq;
	struct cmdq_task_cb cb = task->cb;
	int err = cmdq_task_wait_and_release(task);
	struct cmdq_cb_data cmdq_cb_data;

	/* isr fail, so call cb here to prevent lock */
	if (err < 0 && cb.cb) {
		cmdq_cb_data.err = true;
		cmdq_cb_data.data = cb.data;
		cb.cb(cmdq_cb_data);
	}

	/* prevent no more flush or interrupt to consume waiting tasks */
	if (err < 0)
		queue_work(cmdq->task_consume_wq,
			   &cmdq->task_consume_wait_queue_item);
}

static void cmdq_task_auto_release(struct cmdq_task *task)
{
	struct cmdq *cmdq = task->cmdq;

	/*
	 * the work item is embeded in task already
	 * but we need to initialized it
	 */
	INIT_WORK(&task->auto_release_work, cmdq_auto_release);
	queue_work(cmdq->task_auto_release_wq, &task->auto_release_work);
}

static int cmdq_task_submit(struct cmdq_command *command)
{
	struct device *dev = command->cmdq->dev;
	int err;
	struct cmdq_task *task;

	err = cmdq_task_submit_async(command, &task, NULL);
	if (err < 0) {
		dev_err(dev, "cmdq_task_submit_async failed=%d\n", err);
		return err;
	}
	err = cmdq_task_wait_and_release(task);
	if (err < 0)
		dev_err(dev, "task(0x%p) wait fail\n", task);
	return err;
}

static int cmdq_remove(struct platform_device *pdev)
{
	struct cmdq *cmdq = platform_get_drvdata(pdev);
	struct cmdq_task *task, *tmp;

	destroy_workqueue(cmdq->task_consume_wq);
	cmdq->task_consume_wq = NULL;
	destroy_workqueue(cmdq->task_auto_release_wq);
	cmdq->task_auto_release_wq = NULL;

	/* release task_wait_list */
	list_for_each_entry_safe(task, tmp, &cmdq->task_wait_list, list_entry) {
		dma_free_coherent(cmdq->dev, task->command_size, task->va_base,
				  task->mva_base);
		list_del(&task->list_entry);
		kmem_cache_free(cmdq->task_cache, task);
	}

	kmem_cache_destroy(cmdq->task_cache);
	cmdq->task_cache = NULL;
	return 0;
}

static irqreturn_t cmdq_irq_handler(int irq, void *dev)
{
	struct cmdq *cmdq = dev;
	u32 irq_status;
	int i;

	irq_status = readl(cmdq->base + CMDQ_CURR_IRQ_STATUS_OFFSET);
	irq_status &= CMDQ_IRQ_MASK;
	irq_status ^= CMDQ_IRQ_MASK;

	if (!irq_status)
		return IRQ_NONE;

	while (irq_status) {
		i = ffs(irq_status) - 1;
		irq_status &= ~BIT(i);
		cmdq_handle_irq(cmdq, i);
	}

	queue_work(cmdq->task_consume_wq, &cmdq->task_consume_wait_queue_item);
	return IRQ_HANDLED;
}

static int cmdq_initialize(struct cmdq *cmdq)
{
	int i;

	mutex_init(&cmdq->task_mutex);
	spin_lock_init(&cmdq->exec_lock);
	init_waitqueue_head(&cmdq->thread_dispatch_queue);
	cmdq->task_cache = kmem_cache_create(
			CMDQ_DRIVER_DEVICE_NAME "_task",
			sizeof(struct cmdq_task), __alignof__(struct cmdq_task),
			SLAB_POISON | SLAB_HWCACHE_ALIGN | SLAB_RED_ZONE, NULL);
	INIT_LIST_HEAD(&cmdq->task_wait_list);
	INIT_WORK(&cmdq->task_consume_wait_queue_item,
		  cmdq_consume_waiting_list);
	cmdq->task_auto_release_wq = alloc_ordered_workqueue(
			"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "cmdq_auto_release");
	cmdq->task_consume_wq = alloc_ordered_workqueue(
			"%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "cmdq_task");

	/* initialize cmdq thread */
	for (i = 0; i < ARRAY_SIZE(cmdq->thread); i++) {
		cmdq->thread[i].base = cmdq->base + CMDQ_THR_BASE +
				CMDQ_THR_SHIFT * i;
		init_waitqueue_head(&cmdq->thread[i].wait_queue);
		INIT_LIST_HEAD(&cmdq->thread[i].task_busy_list);
	}

	return 0;
}

static int cmdq_rec_realloc_cmd_buffer(struct cmdq_rec *rec, size_t size)
{
	void *new_buf = krealloc(rec->buf, size, GFP_KERNEL | __GFP_ZERO);

	if (!new_buf)
		return -ENOMEM;
	rec->buf = new_buf;
	rec->buf_size = size;
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

	subsys = cmdq_subsys_base_addr_to_id(base >> 16);
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

int cmdq_rec_create(struct device *dev, u64 engine_flag,
		    struct cmdq_rec **rec_ptr)
{
	struct cmdq_rec *rec;
	int err;

	rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		return -ENOMEM;
	rec->cmdq = dev_get_drvdata(dev);
	rec->engine_flag = engine_flag;
	err = cmdq_rec_realloc_cmd_buffer(rec, CMDQ_INITIAL_CMD_BLOCK_SIZE);
	if (err < 0) {
		kfree(rec);
		return err;
	}
	*rec_ptr = rec;
	return 0;
}
EXPORT_SYMBOL(cmdq_rec_create);

static int cmdq_rec_append_command(struct cmdq_rec *rec, enum cmdq_code code,
				   u32 arg_a, u32 arg_b)
{
	u32 *cmd_ptr;
	int err;

	if (WARN_ON(rec->finalized))
		return -EBUSY;
	if (code < CMDQ_CODE_MOVE || code > CMDQ_CODE_EOC)
		return -EINVAL;
	if (unlikely(rec->command_size + CMDQ_INST_SIZE > rec->buf_size)) {
		err = cmdq_rec_realloc_cmd_buffer(rec, rec->buf_size * 2);
		if (err < 0)
			return err;
	}
	cmd_ptr = (u32 *)(rec->buf + rec->command_size);
	cmd_ptr[0] = arg_b;
	cmd_ptr[1] = (code << CMDQ_OP_CODE_SHIFT) | arg_a;
	rec->command_size += CMDQ_INST_SIZE;
	return 0;
}

int cmdq_rec_write(struct cmdq_rec *rec, u32 value, struct cmdq_base *base,
		   u32 offset)
{
	u32 arg_a = ((base->base + offset) & CMDQ_ARG_A_WRITE_MASK) |
		    ((base->subsys & CMDQ_SUBSYS_MASK) << CMDQ_SUBSYS_SHIFT);

	return cmdq_rec_append_command(rec, CMDQ_CODE_WRITE, arg_a, value);
}
EXPORT_SYMBOL(cmdq_rec_write);

int cmdq_rec_write_mask(struct cmdq_rec *rec, u32 value,
			struct cmdq_base *base, u32 offset, u32 mask)
{
	u32 offset_mask = offset;
	int err;

	if (mask != 0xffffffff) {
		err = cmdq_rec_append_command(rec, CMDQ_CODE_MOVE, 0, ~mask);
		if (err < 0)
			return err;
		offset_mask |= CMDQ_ENABLE_MASK;
	}
	return cmdq_rec_write(rec, value, base, offset_mask);
}
EXPORT_SYMBOL(cmdq_rec_write_mask);

int cmdq_rec_wfe(struct cmdq_rec *rec, enum cmdq_event event)
{
	u32 arg_b;

	if (event >= CMDQ_MAX_HW_EVENT_COUNT || event < 0)
		return -EINVAL;

	/*
	 * bit 0-11: wait value
	 * bit 15: 1 - wait, 0 - no wait
	 * bit 16-27: update value
	 * bit 31: 1 - update, 0 - no update
	 */
	arg_b = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT | CMDQ_WFE_WAIT_VALUE;
	return cmdq_rec_append_command(rec, CMDQ_CODE_WFE, event, arg_b);
}
EXPORT_SYMBOL(cmdq_rec_wfe);

int cmdq_rec_clear_event(struct cmdq_rec *rec, enum cmdq_event event)
{
	if (event >= CMDQ_MAX_HW_EVENT_COUNT || event < 0)
		return -EINVAL;

	return cmdq_rec_append_command(rec, CMDQ_CODE_WFE, event,
				       CMDQ_WFE_UPDATE);
}
EXPORT_SYMBOL(cmdq_rec_clear_event);

static int cmdq_rec_fill_command(struct cmdq_rec *rec,
				 struct cmdq_command *command)
{
	if (!rec->finalized) {
		int err;

		/* insert EOC and generate IRQ for each command iteration */
		err = cmdq_rec_append_command(rec, CMDQ_CODE_EOC, 0,
					      CMDQ_EOC_IRQ_EN);
		if (err < 0)
			return err;

		/* JUMP to end */
		err = cmdq_rec_append_command(rec, CMDQ_CODE_JUMP, 0,
					      CMDQ_JUMP_PASS);
		if (err < 0)
			return err;

		rec->finalized = true;
	}

	command->cmdq = rec->cmdq;
	command->engine_flag = rec->engine_flag;
	command->base = rec->buf;
	command->size = rec->command_size;
	return 0;
}

int cmdq_rec_flush(struct cmdq_rec *rec)
{
	int err;
	struct cmdq_command command;

	err = cmdq_rec_fill_command(rec, &command);
	if (err < 0)
		return err;
	return cmdq_task_submit(&command);
}
EXPORT_SYMBOL(cmdq_rec_flush);

int cmdq_rec_flush_async(struct cmdq_rec *rec, cmdq_async_flush_cb cb,
			 void *data)
{
	int err;
	struct cmdq_command command;
	struct cmdq_task *task;
	struct cmdq_task_cb task_cb;

	err = cmdq_rec_fill_command(rec, &command);
	if (err < 0)
		return err;
	task_cb.cb = cb;
	task_cb.data = data;
	err = cmdq_task_submit_async(&command, &task, &task_cb);
	if (err < 0)
		return err;
	cmdq_task_auto_release(task);
	return 0;
}
EXPORT_SYMBOL(cmdq_rec_flush_async);

void cmdq_rec_destroy(struct cmdq_rec *rec)
{
	kfree(rec->buf);
	kfree(rec);
}
EXPORT_SYMBOL(cmdq_rec_destroy);

static int cmdq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *res;
	struct cmdq *cmdq;
	int err;

	cmdq = devm_kzalloc(dev, sizeof(*cmdq), GFP_KERNEL);
	if (!cmdq)
		return -ENOMEM;
	cmdq->dev = dev;

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

	dev_dbg(dev, "cmdq device: addr:0x%p, va:0x%p, irq:%d\n",
		dev, cmdq->base, cmdq->irq);

	/* init cmdq and save to device private data */
	err = cmdq_initialize(cmdq);
	if (err < 0) {
		dev_err(dev, "failed to init cmdq\n");
		return err;
	}
	platform_set_drvdata(pdev, cmdq);

	err = devm_request_irq(dev, cmdq->irq, cmdq_irq_handler, IRQF_SHARED,
			       CMDQ_DRIVER_DEVICE_NAME, cmdq);
	if (err < 0) {
		dev_err(dev, "failed to register ISR (%d)\n", err);
		goto fail;
	}

	cmdq->clock = devm_clk_get(dev, CMDQ_CLK_NAME);
	if (IS_ERR(cmdq->clock)) {
		dev_err(dev, "failed to get clk:%s\n", CMDQ_CLK_NAME);
		err = PTR_ERR(cmdq->clock);
		goto fail;
	}
	return 0;

fail:
	cmdq_remove(pdev);
	return err;
}

static const struct of_device_id cmdq_of_ids[] = {
	{.compatible = "mediatek,mt8173-gce",},
	{}
};

static struct platform_driver cmdq_drv = {
	.probe = cmdq_probe,
	.remove = cmdq_remove,
	.driver = {
		.name = CMDQ_DRIVER_DEVICE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = cmdq_of_ids,
	}
};

builtin_platform_driver(cmdq_drv);
