/*
 * cyclic scheduler for rtc support
 *
 * Copyright (C) Bill Huey
 * Author: Bill Huey <bill.huey@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/sched.h>
#include "sched.h"
#include "cyclic.h"
#include "cyclic_rt.h"

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

DEFINE_RAW_SPINLOCK(rt_overrun_lock);
struct rb_root rt_overrun_tree = RB_ROOT;

#define MASK2 0xFFFFffffFFFFfff0

/* must revisit again when I get more time to fix the possbility of
 * overflow here and 32 bit portability */
static int cmp_ptr_unsigned_long(long *p, long *q)
{
	int result = ((unsigned long)p & MASK2) - ((unsigned long)q & MASK2);

	WARN_ON(sizeof(long *) != 8);

	if (!result)
		return 0;
	else if (result > 0)
		return 1;
	else
		return -1;
}

static int eq_ptr_unsigned_long(long *p, long *q)
{
	return (((long)p & MASK2) == ((long)q & MASK2));
}

#define CMP_PTR_LONG(p,q) cmp_ptr_unsigned_long((long *)p, (long *)q)

static
struct task_struct *_rt_overrun_entry_find(struct rb_root *root,
						struct task_struct *p)
{
	struct task_struct *ret = NULL;
	struct rb_node *node = root->rb_node;

	while (node) { // double_rq_lock(struct rq *, struct rq *) cpu_rq
 		struct task_struct *task = container_of(node,
					struct task_struct, rt.rt_overrun.node);

		int result = CMP_PTR_LONG(p, task);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else {
			ret = task;
			goto exit;
		}
	}
exit:
	return ret;
}

static int rt_overrun_task_runnable(struct task_struct *p)
{
	return task_on_rq_queued(p);
}

/* avoiding excessive debug printing, splitting the entry point */
static
struct task_struct *rt_overrun_entry_find(struct rb_root *root,
							struct task_struct *p)
{
printk("%s: \n", __func__);
	return _rt_overrun_entry_find(root, p);
}

static int _rt_overrun_entry_insert(struct rb_root *root, struct task_struct *p)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

printk("%s: \n", __func__);
	while (*new) {
 		struct task_struct *task = container_of(*new,
					struct task_struct, rt.rt_overrun.node);

		int result = CMP_PTR_LONG(p, task);

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return 0;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&p->rt.rt_overrun.node, parent, new);
	rb_insert_color(&p->rt.rt_overrun.node, root);

	return 1;
}

static void _rt_overrun_entry_delete(struct task_struct *p)
{
	struct task_struct *task;
	int i;

	task = rt_overrun_entry_find(&rt_overrun_tree, p);

	if (task) {
		printk("%s: p color %d - comm %s - slots 0x%016llx\n",
			__func__, task->rt.rt_overrun.color, task->comm,
			task->rt.rt_overrun.slots);

		rb_erase(&task->rt.rt_overrun.node, &rt_overrun_tree);
		list_del(&task->rt.rt_overrun.task_list);
		for (i = 0; i < SLOTS; ++i) {
			if (rt_admit_rq.curr[i] == p)
				rt_admit_rq.curr[i] = NULL;
		}

		if (rt_admit_curr == p)
			rt_admit_curr = NULL;
	}
}

void rt_overrun_entry_delete(struct task_struct *p)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	_rt_overrun_entry_delete(p);
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
}

/* forward */
int rt_overrun_task_active(struct task_struct *p);

#define PROCFS_MAX_SIZE  2048
static char chunk[PROCFS_MAX_SIZE]; // lock this

static
ssize_t rt_overrun_proc_write(struct file *file, const char *buffer, size_t len,
				loff_t * off)
{
	unsigned long end;

	if (len > PROCFS_MAX_SIZE)
		end = PROCFS_MAX_SIZE;
	else
		end = len;

	if (copy_from_user(chunk, buffer, end))
		return -EFAULT;

	printk(KERN_INFO "%s: write %lu bytes, s = %s \n", __func__, end,
							(char *) &chunk[0]);
	return end;
}

static int rt_overrun_proc_show(struct seq_file *m, void *v);

static int rt_overrun_proc_open(struct inode *inode, struct  file *file)
{
	return single_open(file, rt_overrun_proc_show, NULL);
}

static const struct file_operations rt_overrun_proc_fops = {
	.owner = THIS_MODULE,
	.open = rt_overrun_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = rt_overrun_proc_write,
};

static int __init rt_overrun_proc_init(void) {
	proc_create("rt_overrun_proc", 0, NULL, &rt_overrun_proc_fops);
	return 0;
}

static void __exit rt_overrun_proc_exit(void) {
	remove_proc_entry("rt_overrun_proc", NULL);
}

struct rt_overrun_admit_rq rt_admit_rq;

/*static*/
void init_rt_overrun(void)
{
	rt_overrun_proc_init();
	reset_rt_overrun();
}

void reset_rt_overrun(void)
{
	int i;

	for (i = 0; i < SLOTS; i++)
		rt_admit_rq.curr[i] = NULL;

	rt_admit_rq.slot = 0;
	rt_admit_rq.end = SLOTS;
}

static int rt_overrun_proc_show(struct seq_file *m, void *v) {
	int i;
	unsigned long flags;
	u64 slots = 0;
	struct task_struct *task;

	seq_printf(m, "%s: \n", __func__);
	seq_printf(m, "\n\t");

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);

	if (rt_admit_curr)
		slots = rt_admit_curr->rt.rt_overrun.slots;

	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);

	for (i = 0; i < SLOTS; i++) {
		if ((i % 4) == 0 )
			seq_printf(m, "\n\t");

		task = rt_admit_rq.curr[i];
		if (task)
			seq_printf(m, " %d", task->rt.rt_overrun.color);
		else
			seq_printf(m, " 0");
	
		if (task)
			seq_printf(m, " (%d)",
				task->rt.rt_overrun.color);
		else
			seq_printf(m, " (0)");
	}
	seq_printf(m, "\ncurr\n");

	seq_printf(m, "\n\t");
	for (i = 0; i < SLOTS; ++i) {
		if (test_bit(i, (unsigned long *) &slots))
			seq_printf(m, "1");
		else
			seq_printf(m, "0");

		if (((i+1) % 16) == 0)
			seq_printf(m, "\n\t");
		else if (((i +1) % 4) == 0)
			seq_printf(m, " ");
	}
	seq_printf(m, "\n");

	return 0;
}


static void _rt_overrun_task_replenish(struct task_struct *p)
{
	WARN_ONCE(!rt_overrun_entry_find(&rt_overrun_tree, p), "\n");

	rt_admit_curr = p;
	rt_admit_rq.debug = p;
	WARN_ONCE(!(CMP_PTR_LONG(rt_admit_curr, p)),
			"not equal \b");
}

void rt_overrun_task_replenish(struct task_struct *p)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	_rt_overrun_task_replenish(p);
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
}

static void _rt_overrun_task_expire(struct task_struct *p)
{
printk("%s: \n", __func__);
	WARN_ONCE(!rt_overrun_entry_find(&rt_overrun_tree, p), "\n");

	rt_admit_curr = NULL;
}

static void rt_overrun_task_expire(struct task_struct *p)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	_rt_overrun_task_expire(p);
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
}

static int rt_overrun_slot_color_next(void)
{
	int color = rt_admit_rq.color;

	++rt_admit_rq.color;

	return color;
}

/* potential security problems */
int rt_overrun_task_admit(struct task_struct *p, u64 slots)
{
	int i, ret = 0;
	unsigned long flags;

	printk("%s: slot = 0x%016llx\n", __func__, slots);

	get_task_struct(p);
	if (!rt_policy(p->policy)) {
		printk("%s: policy, admittance failed \n", __func__);
		put_task_struct(p);
		return 1;
	}

	if (p->sched_class != &rt_sched_class) {
		printk("%s: sched_class, admittance failed \n", __func__);
		put_task_struct(p);
		return 1;
	}

	/* grabs the rq lock here, CPU 0 only */
	set_cpus_allowed_ptr(p, get_cpu_mask(0));

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	if (!p->rt.rt_overrun.color) {
		p->rt.rt_overrun.color = rt_overrun_slot_color_next();
		printk("%s: color = %d \n", __func__, p->rt.rt_overrun.color);
	}


	p->rt.rt_overrun.slots = slots;

	WARN_ONCE(rt_admit_rq.active < 0, "\n");
	WARN_ONCE(_rt_overrun_entry_find(&rt_overrun_tree, p), "\n");

	p->rt.rt_overrun.count = 0;

	_rt_overrun_entry_insert(&rt_overrun_tree, p);
	_rt_overrun_task_replenish(p);
	++rt_admit_rq.active;

	if ((cpumask_weight(&p->cpus_allowed) != 1) ||
	    !cpumask_test_cpu(0, &p->cpus_allowed)) {
		printk("%s: failed \n", __func__);
		ret = 1;
	} else 
		printk("%s: success \n", __func__);

	for (i = 0; i < SLOTS; ++i) {
		/* slots is a u64, ignore the pointer type */
		if (test_bit(i, (unsigned long *) &slots))
			rt_admit_rq.curr[i] = p;
	}

	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
	put_task_struct(p);

	return ret;
}

static void rt_overrun_task_discharge(struct task_struct *p)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
printk("%s: \n", __func__);
	WARN_ONCE(rt_admit_rq.active <= 0, "\n");
	WARN_ONCE(!_rt_overrun_entry_find(&rt_overrun_tree, p), "\n");
	--rt_admit_rq.active;

	/* assert */
	_rt_overrun_task_expire(p);
	_rt_overrun_entry_delete(p);

	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
}

void rt_overrun_entries_delete_all(struct rtc_device *rtc)
{
	unsigned long flags;
	struct task_struct *task;

	struct list_head *pos;

printk("%s: \n", __func__);
	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	list_for_each (pos, &rtc->rt_overrun_tasks) {
		task = list_entry(pos, struct task_struct,
							rt.rt_overrun.task_list);
printk("%s: rt_overrun_tasks p 0x%016llx - comm %s\n", __func__, (u64) task->rt.rt_overrun.color,
			task->comm);
		_rt_overrun_entry_delete(task);
	}

	rt_admit_rq.active = 0;
	rt_admit_rq.color = 0;
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);
}

/* must think about locking here, nothing for now BUG */
int rt_overrun_task_admitted1(struct rq *rq, struct task_struct *p)
{
	int ret = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	if (rt_admit_rq.active) { // --billh
		if ((rt_admit_curr == p)
		 || _on_rt_overrun_admitted(p)
		 || _rt_overrun_entry_find(&rt_overrun_tree, p))
			ret = 1;
	}
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);

	return ret;
}

void rt_overrun_check(struct rq *rq, struct task_struct *p)
{
	get_task_struct(p);
	WARN_ONCE(rt_overrun_task_admitted1(rq, p) &&
		cpumask_equal(get_cpu_mask(0), &p->cpus_allowed),
		"not bounded to CPU 0\n");
	put_task_struct(p);
}


/* must think about locking here, nothing for now BUG */
int rt_overrun_rq_admitted(void)
{
	return rt_admit_rq.active;
}

/* must think about locking here, nothing for now BUG */
int rt_overrun_task_active(struct task_struct *p)
{
	if (CMP_PTR_LONG(rt_admit_curr, p))
		return 1;
	else
		return 0;
}

static struct task_struct *rt_overrun_get_next_task(void)
{
	/* return the next slot, advance the cursor */
	WARN_ONCE(!rt_admit_rq.active, "\n");

	if (rt_admit_rq.slot < (SLOTS -1)) {
		++rt_admit_rq.slot;
	} else {
//		printk("%s: slot wrap = 0 \n", __func__);
		rt_admit_rq.slot = 0;
	}

	return rt_admit_curr;
}

#define PRT_RUNNABLE()				\
	if (tail == 1)				\
		printk("on rq \n");		\
	else if (tail == 0)			\
		printk("not on rq \n");	\
	else					\
		printk("\n");

/* rq lock already grabbed, interrupts off */
void rt_overrun_timer_handler(struct rtc_device *rtc)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	unsigned long irq_data;

	struct task_struct *curr_slot, *next_slot;
	int tail = 2;
	int wake_next = 0, curr_runnable = 0;
	int same;

	WARN_ON(!irqs_disabled());

	printk("%s: ---\n", __func__);

	/* this is incorrect, but is working for now */
	WARN_ON((rq->cpu != 0));
	raw_spin_lock(&rq->lock);
	raw_spin_lock(&rt_overrun_lock);

	curr_slot = rt_admit_curr;
	irq_data = rtc->irq_data;

	/* suppress rtc_read_dev wake if curr_slot ==  NULL */
	if (curr_slot) {
		if (rt_overrun_task_runnable(curr_slot))
			curr_runnable = tail = 1;
		else
			curr_runnable = tail = 0;
	}

	if (curr_slot)
		printk("%s: curr_slot %d ", __func__, curr_slot->rt.rt_overrun.color);
	PRT_RUNNABLE();

	next_slot = rt_overrun_get_next_task();
	tail = 2;

	/* */
	if (curr_slot == next_slot) {
		same = 1;
	} else {
		same = 0;
		/* deactivate edge, runnable case */
		if (curr_slot && curr_runnable) {
			requeue_task_rt2(rq, curr_slot, 0); // tail
			resched_curr(rq);
		}
	}

	/* transition edge, record per task overrun */
	if (curr_slot && !same) {
		++curr_slot->rt.rt_overrun.count;
		printk("%s: overrun inc %ld\n", __func__,
						curr_slot->rt.rt_overrun.count);
	}

	/* activate edge, wake/top next_slot */
	if (next_slot) {
		if (!same) {
			printk("%s: ~same\n", __func__);
			if (rt_overrun_task_runnable(next_slot)) {
				printk("%s: next runnable requeue top\n", __func__);
				requeue_task_rt2(rq, next_slot, 1); // head
				resched_curr(rq);
				tail = 1;
			} else {
				printk("%s: ~next runnable\n", __func__);
				tail = 0;
				wake_next = 1;
			}
		} /* same, then chain the activations */
	}

	if (next_slot)
		printk("%s: next_slot %d ", __func__, next_slot->rt.rt_overrun.color);
	PRT_RUNNABLE();

	rt_admit_curr = next_slot;

	raw_spin_unlock(&rt_overrun_lock);
	raw_spin_unlock(&rq->lock);

	/* set to reschedule at interrupt return, wake attempt should
	 * do this for us already */
	if (wake_next) {
		wake_up_interruptible_sync_poll(&rtc->irq_queue, next_slot);
		if (same) {
			printk("%s: same\n", __func__);
		}
	}
	else
		printk("%s: pass\n", __func__);
}

int rt_overrun_task_yield(struct task_struct *p)
{
	return rt_task_yield(p);
}

// default_wake_function wake_up_state list_head rtc_device
int single_default_wake_function(wait_queue_t *curr, unsigned mode,
				int wake_flags, void *key)
{
	unsigned long flags;
	struct task_struct *p = key, *task = curr->private;
	int on = 0;

	/* If not admitted to rt_overrun, then wake it normally with at the
	 * normal timer interrupt handler */

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	if (p) on = _on_rt_overrun_admitted(p);
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);

	/* wake only one thread for this case */
	if (key == NULL) {
		printk("%s: wake 0 p 0x%08llx, task 0x%08llx, admit %d,"
			" flags %ld\n",
				__func__, (u64) p, (u64) task, on, flags);
		return wake_up_state(task, mode);
	} else if (key == task) {
		if (on) {
			printk("%s: wake 1 p 0x%08llx, task 0x%08llx, "
				"admit %d, flags %ld\n",
				__func__, (u64) p, (u64) task, on, flags);
			return wake_up_state(task, mode);
		} else {
			printk("%s: ignore 0 p 0x%08llx, task 0x%08llx, "
				"flags %ld\n",
				__func__, (u64) p, (u64) task, flags);
			return 0;
		}
	} else {
		printk("%s: ignore 1 p 0x%08llx, task 0x%08llx, flags %ld\n",
			__func__, (u64) p, (u64) task, flags);
		return 0;
	}
}
