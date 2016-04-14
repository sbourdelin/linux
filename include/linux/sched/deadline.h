#ifndef _SCHED_DEADLINE_H
#define _SCHED_DEADLINE_H

/*
 * Used by enqueue_task_dl() for PI cases to disguise sched_dl_entity,
 * thus must be the same order as the counterparts in sched_dl_entity.
 */
struct sched_dl_entity_fake {
	struct rb_node  rb_node;
	u64 dl_runtime;
	u64 dl_period;
};

/*
 * SCHED_DEADLINE tasks has negative priorities, reflecting
 * the fact that any of them has higher prio than RT and
 * NORMAL/BATCH tasks.
 */

#define MAX_DL_PRIO		0

static inline int dl_prio(int prio)
{
	if (unlikely(prio < MAX_DL_PRIO))
		return 1;
	return 0;
}

static inline int dl_task(struct task_struct *p)
{
	return dl_prio(p->prio);
}

static inline bool dl_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

extern void rt_mutex_update_copy(struct task_struct *p);

#ifdef CONFIG_RT_MUTEXES
extern struct rt_mutex_waiter *rt_mutex_get_top_waiter(struct task_struct *p);
#else
static inline
struct rt_mutex_waiter *rt_mutex_get_top_waiter(struct task_struct *p)
{
	return NULL;
}
#endif

#endif /* _SCHED_DEADLINE_H */
