/* cyclic.h rt_overrun */

//#ifndef _CYCLIC_H
//#define _CYCLIC_H
//#else

#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/rtc.h>

extern int rt_overrun_task_active(struct task_struct *p);
extern void rt_overrun_stat_tick_blocked(void);

extern void rt_overrun_stat_dequeue(void);
extern void rt_overrun_stat_enqueue(struct task_struct *p);

extern void rt_overrun_timer_handler(struct rtc_device *rtc);
extern int rt_overrun_rq_admitted(void);
extern void init_rt_overrun(void);
extern void rt_overrun_entry_delete(struct task_struct *p);

extern void rt_overrun_task_replenish(struct task_struct *p);
extern int rt_overrun_task_admit(struct task_struct *p, u64 slots);

extern int rt_overrun_task_yield(struct task_struct *p);
extern void rt_overrun_entries_delete_all(struct rtc_device *);
extern void reset_rt_overrun(void);

extern struct raw_spinlock rt_overrun_lock;
extern int single_default_wake_function(wait_queue_t *curr, unsigned mode,
					int wake_flags, void *key);
#define SLOTS 64

#define rt_admit_curr		(rt_admit_rq.curr[rt_admit_rq.slot])
#define rt_task_count(a)	(a->rt.rt_overrun.count)
#define rt_task_yield(a)	(a->rt.rt_overrun.yield)

/* slot admittance queue */
struct rt_overrun_admit_rq {
	int active;
	int slot, end;
	struct task_struct *curr[SLOTS];
	struct task_struct *debug;
	int color;
};

extern struct rt_overrun_admit_rq rt_admit_rq;

static inline int rt_overrun_policy(struct task_struct *p, int policy)
{
	int ret;
	unsigned long flags;

	raw_spin_lock_irqsave(&rt_overrun_lock, flags);
	ret = RB_EMPTY_NODE(&p->rt.rt_overrun.node);
	raw_spin_unlock_irqrestore(&rt_overrun_lock, flags);

	return ret;
}

static inline int _on_rt_overrun_admitted(struct task_struct *p)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rb_node *node = &rt_se->rt_overrun.node;

	if (node)
		return !RB_EMPTY_NODE(node);
	else
		return 0;
}

static inline int on_rt_overrun_admitted(struct task_struct *p)
{
	int ret;

	raw_spin_lock(&rt_overrun_lock);
	ret = _on_rt_overrun_admitted(p);
	raw_spin_unlock(&rt_overrun_lock);

	return ret;
}

extern int single_default_wake_function(wait_queue_t *curr, unsigned mode,
				int wake_flags, void *key);

//#endif
