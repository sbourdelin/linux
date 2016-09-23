#undef TRACE_SYSTEM
#define TRACE_SYSTEM sched

#if !defined(_TRACE_SCHED_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHED_H

#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <linux/binfmts.h>

/*
 * Tracepoint for calling kthread_stop, performed to end a kthread:
 */
TRACE_EVENT(sched_kthread_stop,

	TP_PROTO(struct task_struct *t),

	TP_ARGS(t),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, t->comm, TASK_COMM_LEN);
		__entry->pid	= t->pid;
	),

	TP_printk("comm=%s pid=%d", __entry->comm, __entry->pid)
);

/*
 * Tracepoint for the return value of the kthread stopping:
 */
TRACE_EVENT(sched_kthread_stop_ret,

	TP_PROTO(int ret),

	TP_ARGS(ret),

	TP_STRUCT__entry(
		__field(	int,	ret	)
	),

	TP_fast_assign(
		__entry->ret	= ret;
	),

	TP_printk("ret=%d", __entry->ret)
);

/*
 * Tracepoint for waking up a task:
 */
DECLARE_EVENT_CLASS(sched_wakeup_template,

	TP_PROTO(struct task_struct *p),

	TP_ARGS(__perf_task(p)),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
		__field(	int,	prio			)
		__field(	int,	success			)
		__field(	int,	target_cpu		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->prio		= p->prio;
		__entry->success	= 1; /* rudiment, kill when possible */
		__entry->target_cpu	= task_cpu(p);
	),

	TP_printk("comm=%s pid=%d prio=%d target_cpu=%03d",
		  __entry->comm, __entry->pid, __entry->prio,
		  __entry->target_cpu)
);

/*
 * Tracepoint called when waking a task; this tracepoint is guaranteed to be
 * called from the waking context.
 */
DEFINE_EVENT(sched_wakeup_template, sched_waking,
	     TP_PROTO(struct task_struct *p),
	     TP_ARGS(p));

/*
 * Tracepoint called when the task is actually woken; p->state == TASK_RUNNNG.
 * It it not always called from the waking context.
 */
DEFINE_EVENT(sched_wakeup_template, sched_wakeup,
	     TP_PROTO(struct task_struct *p),
	     TP_ARGS(p));

/*
 * Tracepoint for waking up a new task:
 */
DEFINE_EVENT(sched_wakeup_template, sched_wakeup_new,
	     TP_PROTO(struct task_struct *p),
	     TP_ARGS(p));

TRACE_EVENT_MAP(sched_waking, sched_waking_prio,

	TP_PROTO(struct task_struct *p),

	TP_ARGS(p),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
		__field(	int,	target_cpu		)
		__field( 	unsigned int,	policy		)
		__field( 	int,	nice			)
		__field( 	unsigned int,	rt_priority	)
		__field( 	u64,	dl_runtime		)
		__field( 	u64,	dl_deadline		)
		__field( 	u64,	dl_period		)
		__array(	char,	top_waiter_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	top_waiter_pid		)
	),

	TP_fast_assign(
		struct task_struct *top_waiter = rt_mutex_get_top_task(p);

		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->target_cpu	= task_cpu(p);
		__entry->policy		= effective_policy(
						p->policy, p->prio);
		__entry->nice		= task_nice(p);
		__entry->rt_priority	= effective_rt_prio(
						p->prio);
		__entry->dl_runtime	= dl_prio(p->prio) ?
						p->dl.dl_runtime : 0;
		__entry->dl_deadline	= dl_prio(p->prio) ?
						p->dl.dl_deadline : 0;
		__entry->dl_period	= dl_prio(p->prio) ?
						p->dl.dl_period : 0;
		if (top_waiter) {
			memcpy(__entry->top_waiter_comm, top_waiter->comm,
					TASK_COMM_LEN);
			__entry->top_waiter_pid	= top_waiter->pid;
		} else {
			__entry->top_waiter_comm[0] = '\0';
			__entry->top_waiter_pid	= -1;
		}
	),

	TP_printk("comm=%s, pid=%d, target_cpu=%03d, policy=%s, "
			"nice=%d, rt_priority=%u, dl_runtime=%Lu, "
			"dl_deadline=%Lu, dl_period=%Lu, "
			"top_waiter_comm=%s, top_waiter_pid=%d",
		  __entry->comm, __entry->pid, __entry->target_cpu,
		  __print_symbolic(__entry->policy, SCHEDULING_POLICY),
		  __entry->nice, __entry->rt_priority, __entry->dl_runtime,
		  __entry->dl_deadline, __entry->dl_period,
		  __entry->top_waiter_comm, __entry->top_waiter_pid)
);

#ifdef CREATE_TRACE_POINTS
static inline long __trace_sched_switch_state(bool preempt, struct task_struct *p)
{
#ifdef CONFIG_SCHED_DEBUG
	BUG_ON(p != current);
#endif /* CONFIG_SCHED_DEBUG */

	/*
	 * Preemption ignores task state, therefore preempted tasks are always
	 * RUNNING (we will not have dequeued if state != RUNNING).
	 */
	return preempt ? TASK_RUNNING | TASK_STATE_MAX : p->state;
}
#endif /* CREATE_TRACE_POINTS */

/*
 * Tracepoint for task switches, performed by the scheduler:
 */
TRACE_EVENT(sched_switch,

	TP_PROTO(bool preempt,
		 struct task_struct *prev,
		 struct task_struct *next),

	TP_ARGS(preempt, prev, next),

	TP_STRUCT__entry(
		__array(	char,	prev_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	prev_pid			)
		__field(	int,	prev_prio			)
		__field(	long,	prev_state			)
		__array(	char,	next_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	next_pid			)
		__field(	int,	next_prio			)
	),

	TP_fast_assign(
		memcpy(__entry->next_comm, next->comm, TASK_COMM_LEN);
		__entry->prev_pid	= prev->pid;
		__entry->prev_prio	= prev->prio;
		__entry->prev_state	= __trace_sched_switch_state(preempt, prev);
		memcpy(__entry->prev_comm, prev->comm, TASK_COMM_LEN);
		__entry->next_pid	= next->pid;
		__entry->next_prio	= next->prio;
	),

	TP_printk("prev_comm=%s prev_pid=%d prev_prio=%d prev_state=%s%s ==> next_comm=%s next_pid=%d next_prio=%d",
		__entry->prev_comm, __entry->prev_pid, __entry->prev_prio,
		__entry->prev_state & (TASK_STATE_MAX-1) ?
		  __print_flags(__entry->prev_state & (TASK_STATE_MAX-1), "|",
				{ 1, "S"} , { 2, "D" }, { 4, "T" }, { 8, "t" },
				{ 16, "Z" }, { 32, "X" }, { 64, "x" },
				{ 128, "K" }, { 256, "W" }, { 512, "P" },
				{ 1024, "N" }) : "R",
		__entry->prev_state & TASK_STATE_MAX ? "+" : "",
		__entry->next_comm, __entry->next_pid, __entry->next_prio)
);

/*
 * Tracepoint for task switches, performed by the scheduler:
 */
TRACE_EVENT_MAP(sched_switch, sched_switch_prio,
	TP_PROTO(bool preempt,
		 struct task_struct *prev,
		 struct task_struct *next),

	TP_ARGS(preempt, prev, next),

	TP_STRUCT__entry(
		__array(	char,	prev_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	prev_pid			)
		__field(	long,	prev_state			)
		__field( 	unsigned int,	prev_policy		)
		__field( 	int,	prev_nice			)
		__field( 	unsigned int,	prev_rt_priority	)
		__field( 	u64,	prev_dl_runtime			)
		__field( 	u64,	prev_dl_deadline		)
		__field( 	u64,	prev_dl_period			)
		__array(	char,	prev_top_waiter_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	prev_top_waiter_pid		)
		__array(	char,	next_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	next_pid			)
		__field( 	unsigned int,	next_policy		)
		__field( 	int,	next_nice			)
		__field( 	unsigned int,	next_rt_priority	)
		__field( 	u64,	next_dl_runtime			)
		__field( 	u64,	next_dl_deadline		)
		__field( 	u64,	next_dl_period			)
		__array(	char,	next_top_waiter_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	next_top_waiter_pid		)
	),

	TP_fast_assign(
		struct task_struct *prev_top = rt_mutex_get_top_task(prev);
		struct task_struct *next_top = rt_mutex_get_top_task(next);

		memcpy(__entry->prev_comm, prev->comm, TASK_COMM_LEN);
		__entry->prev_pid		= prev->pid;
		__entry->prev_state		= __trace_sched_switch_state(
							preempt, prev);
		__entry->prev_policy		= effective_policy(
							prev->policy, prev->prio);
		__entry->prev_nice		= task_nice(prev);
		__entry->prev_rt_priority	= effective_rt_prio(
							prev->prio);
		__entry->prev_dl_runtime	= dl_prio(prev->prio) ?
							prev->dl.dl_runtime : 0;
		__entry->prev_dl_deadline	= dl_prio(prev->prio) ?
							prev->dl.dl_deadline : 0;
		__entry->prev_dl_period		= dl_prio(prev->prio) ?
							prev->dl.dl_period : 0;
		if (prev_top) {
			memcpy(__entry->prev_top_waiter_comm, prev_top->comm,
					TASK_COMM_LEN);
			__entry->prev_top_waiter_pid	= prev_top->pid;
		} else {
			__entry->prev_top_waiter_comm[0] = '\0';
			__entry->prev_top_waiter_pid	= -1;
		}

		memcpy(__entry->next_comm, next->comm, TASK_COMM_LEN);
		__entry->next_pid		= next->pid;
		__entry->next_policy		= effective_policy(
							next->policy, prev->prio);
		__entry->next_nice		= task_nice(next);
		__entry->next_rt_priority	= effective_rt_prio(
							next->prio);
		__entry->next_dl_runtime	= dl_prio(next->prio) ?
							next->dl.dl_runtime : 0;
		__entry->next_dl_deadline	= dl_prio(next->prio) ?
							next->dl.dl_deadline : 0;
		__entry->next_dl_period		= dl_prio(next->prio) ?
							next->dl.dl_period : 0;
		if (next_top) {
			memcpy(__entry->next_top_waiter_comm, next_top->comm,
					TASK_COMM_LEN);
			__entry->next_top_waiter_pid	= next_top->pid;
		} else {
			__entry->next_top_waiter_comm[0] = '\0';
			__entry->next_top_waiter_pid	= -1;
		}
	),

	TP_printk("prev_comm=%s, prev_pid=%d, prev_policy=%s, prev_nice=%d, "
			"prev_rt_priority=%u, prev_dl_runtime=%Lu, "
			"prev_dl_deadline=%Lu, prev_dl_period=%Lu, "
			"prev_state=%s%s, prev_top_waiter_comm=%s, "
			"prev_top_waiter_pid=%d ==> next_comm=%s, next_pid=%d, "
			"next_policy=%s, next_nice=%d, next_rt_priority=%u, "
			"next_dl_runtime=%Lu, next_dl_deadline=%Lu, "
			"next_dl_period=%Lu, next_top_waiter_comm=%s, "
			"next_top_waiter_pid=%d",
		__entry->prev_comm, __entry->prev_pid,
		__print_symbolic(__entry->prev_policy, SCHEDULING_POLICY),
		__entry->prev_nice, __entry->prev_rt_priority,
		__entry->prev_dl_runtime, __entry->prev_dl_deadline,
		__entry->prev_dl_period,
		__entry->prev_state & (TASK_STATE_MAX-1) ?
		  __print_flags(__entry->prev_state & (TASK_STATE_MAX-1), "|",
				{ 1, "S"} , { 2, "D" }, { 4, "T" }, { 8, "t" },
				{ 16, "Z" }, { 32, "X" }, { 64, "x" },
				{ 128, "K" }, { 256, "W" }, { 512, "P" },
				{ 1024, "N" }) : "R",
		__entry->prev_state & TASK_STATE_MAX ? "+" : "",
		__entry->prev_top_waiter_comm, __entry->prev_top_waiter_pid,
		__entry->next_comm, __entry->next_pid,
		__print_symbolic(__entry->next_policy, SCHEDULING_POLICY),
		__entry->next_nice, __entry->next_rt_priority,
		__entry->next_dl_runtime, __entry->next_dl_deadline,
		__entry->next_dl_period, __entry->next_top_waiter_comm,
		__entry->next_top_waiter_pid)
);

/*
 * Tracepoint for a task being migrated:
 */
TRACE_EVENT(sched_migrate_task,

	TP_PROTO(struct task_struct *p, int dest_cpu),

	TP_ARGS(p, dest_cpu),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
		__field(	int,	prio			)
		__field(	int,	orig_cpu		)
		__field(	int,	dest_cpu		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->prio		= p->prio;
		__entry->orig_cpu	= task_cpu(p);
		__entry->dest_cpu	= dest_cpu;
	),

	TP_printk("comm=%s pid=%d prio=%d orig_cpu=%d dest_cpu=%d",
		  __entry->comm, __entry->pid, __entry->prio,
		  __entry->orig_cpu, __entry->dest_cpu)
);

DECLARE_EVENT_CLASS(sched_process_template,

	TP_PROTO(struct task_struct *p),

	TP_ARGS(p),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
		__field(	int,	prio			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid		= p->pid;
		__entry->prio		= p->prio;
	),

	TP_printk("comm=%s pid=%d prio=%d",
		  __entry->comm, __entry->pid, __entry->prio)
);

/*
 * Tracepoint for freeing a task:
 */
DEFINE_EVENT(sched_process_template, sched_process_free,
	     TP_PROTO(struct task_struct *p),
	     TP_ARGS(p));
	     

/*
 * Tracepoint for a task exiting:
 */
DEFINE_EVENT(sched_process_template, sched_process_exit,
	     TP_PROTO(struct task_struct *p),
	     TP_ARGS(p));

/*
 * Tracepoint for waiting on task to unschedule:
 */
DEFINE_EVENT(sched_process_template, sched_wait_task,
	TP_PROTO(struct task_struct *p),
	TP_ARGS(p));

/*
 * Tracepoint for a waiting task:
 */
TRACE_EVENT(sched_process_wait,

	TP_PROTO(struct pid *pid),

	TP_ARGS(pid),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,	pid			)
		__field(	int,	prio			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->pid		= pid_nr(pid);
		__entry->prio		= current->prio;
	),

	TP_printk("comm=%s pid=%d prio=%d",
		  __entry->comm, __entry->pid, __entry->prio)
);

/*
 * Tracepoint for do_fork:
 */
TRACE_EVENT(sched_process_fork,

	TP_PROTO(struct task_struct *parent, struct task_struct *child),

	TP_ARGS(parent, child),

	TP_STRUCT__entry(
		__array(	char,	parent_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	parent_pid			)
		__array(	char,	child_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	child_pid			)
	),

	TP_fast_assign(
		memcpy(__entry->parent_comm, parent->comm, TASK_COMM_LEN);
		__entry->parent_pid	= parent->pid;
		memcpy(__entry->child_comm, child->comm, TASK_COMM_LEN);
		__entry->child_pid	= child->pid;
	),

	TP_printk("comm=%s pid=%d child_comm=%s child_pid=%d",
		__entry->parent_comm, __entry->parent_pid,
		__entry->child_comm, __entry->child_pid)
);

TRACE_EVENT_MAP(sched_process_fork, sched_process_fork_prio,

	TP_PROTO(struct task_struct *parent, struct task_struct *child),

	TP_ARGS(parent, child),

	TP_STRUCT__entry(
		__array(	char,	parent_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	parent_pid			)
		__array(	char,	child_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	child_pid			)
		__field( 	unsigned int,	child_policy		)
		__field( 	int,	child_nice			)
		__field( 	unsigned int,	child_rt_priority	)
		__field( 	u64,	child_dl_runtime		)
		__field( 	u64,	child_dl_deadline		)
		__field( 	u64,	child_dl_period			)
	),

	TP_fast_assign(
		memcpy(__entry->parent_comm, parent->comm, TASK_COMM_LEN);
		__entry->parent_pid	= parent->pid;
		memcpy(__entry->child_comm, child->comm, TASK_COMM_LEN);
		__entry->child_pid	= child->pid;
		__entry->child_policy		= effective_policy(
							child->policy, child->prio);
		__entry->child_nice		= task_nice(child);
		__entry->child_rt_priority	= effective_rt_prio(
							child->prio);
		__entry->child_dl_runtime	= dl_prio(child->prio) ?
							child->dl.dl_runtime : 0;
		__entry->child_dl_deadline	= dl_prio(child->prio) ?
							child->dl.dl_deadline : 0;
		__entry->child_dl_period	= dl_prio(child->prio) ?
							child->dl.dl_period : 0;
	),

	TP_printk("comm=%s, pid=%d, child_comm=%s, child_pid=%d, "
			"child_policy=%s, child_nice=%d, "
			"child_rt_priority=%u, child_dl_runtime=%Lu, "
			"child_dl_deadline=%Lu, child_dl_period=%Lu",
		__entry->parent_comm, __entry->parent_pid,
		__entry->child_comm, __entry->child_pid,
		__print_symbolic(__entry->child_policy, SCHEDULING_POLICY),
		__entry->child_nice, __entry->child_rt_priority,
		__entry->child_dl_runtime, __entry->child_dl_deadline,
		__entry->child_dl_period)
);

/*
 * Tracepoint for exec:
 */
TRACE_EVENT(sched_process_exec,

	TP_PROTO(struct task_struct *p, pid_t old_pid,
		 struct linux_binprm *bprm),

	TP_ARGS(p, old_pid, bprm),

	TP_STRUCT__entry(
		__string(	filename,	bprm->filename	)
		__field(	pid_t,		pid		)
		__field(	pid_t,		old_pid		)
	),

	TP_fast_assign(
		__assign_str(filename, bprm->filename);
		__entry->pid		= p->pid;
		__entry->old_pid	= old_pid;
	),

	TP_printk("filename=%s pid=%d old_pid=%d", __get_str(filename),
		  __entry->pid, __entry->old_pid)
);

/*
 * XXX the below sched_stat tracepoints only apply to SCHED_OTHER/BATCH/IDLE
 *     adding sched_stat support to SCHED_FIFO/RR would be welcome.
 */
DECLARE_EVENT_CLASS(sched_stat_template,

	TP_PROTO(struct task_struct *tsk, u64 delay),

	TP_ARGS(__perf_task(tsk), __perf_count(delay)),

	TP_STRUCT__entry(
		__array( char,	comm,	TASK_COMM_LEN	)
		__field( pid_t,	pid			)
		__field( u64,	delay			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid	= tsk->pid;
		__entry->delay	= delay;
	),

	TP_printk("comm=%s pid=%d delay=%Lu [ns]",
			__entry->comm, __entry->pid,
			(unsigned long long)__entry->delay)
);


/*
 * Tracepoint for accounting wait time (time the task is runnable
 * but not actually running due to scheduler contention).
 */
DEFINE_EVENT(sched_stat_template, sched_stat_wait,
	     TP_PROTO(struct task_struct *tsk, u64 delay),
	     TP_ARGS(tsk, delay));

/*
 * Tracepoint for accounting sleep time (time the task is not runnable,
 * including iowait, see below).
 */
DEFINE_EVENT(sched_stat_template, sched_stat_sleep,
	     TP_PROTO(struct task_struct *tsk, u64 delay),
	     TP_ARGS(tsk, delay));

/*
 * Tracepoint for accounting iowait time (time the task is not runnable
 * due to waiting on IO to complete).
 */
DEFINE_EVENT(sched_stat_template, sched_stat_iowait,
	     TP_PROTO(struct task_struct *tsk, u64 delay),
	     TP_ARGS(tsk, delay));

/*
 * Tracepoint for accounting blocked time (time the task is in uninterruptible).
 */
DEFINE_EVENT(sched_stat_template, sched_stat_blocked,
	     TP_PROTO(struct task_struct *tsk, u64 delay),
	     TP_ARGS(tsk, delay));

/*
 * Tracepoint for accounting runtime (time the task is executing
 * on a CPU).
 */
DECLARE_EVENT_CLASS(sched_stat_runtime,

	TP_PROTO(struct task_struct *tsk, u64 runtime, u64 vruntime),

	TP_ARGS(tsk, __perf_count(runtime), vruntime),

	TP_STRUCT__entry(
		__array( char,	comm,	TASK_COMM_LEN	)
		__field( pid_t,	pid			)
		__field( u64,	runtime			)
		__field( u64,	vruntime			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid		= tsk->pid;
		__entry->runtime	= runtime;
		__entry->vruntime	= vruntime;
	),

	TP_printk("comm=%s pid=%d runtime=%Lu [ns] vruntime=%Lu [ns]",
			__entry->comm, __entry->pid,
			(unsigned long long)__entry->runtime,
			(unsigned long long)__entry->vruntime)
);

DEFINE_EVENT(sched_stat_runtime, sched_stat_runtime,
	     TP_PROTO(struct task_struct *tsk, u64 runtime, u64 vruntime),
	     TP_ARGS(tsk, runtime, vruntime));

/*
 * Tracepoint for showing priority inheritance modifying a tasks
 * priority.
 */
TRACE_EVENT(sched_pi_setprio,

	TP_PROTO(struct task_struct *tsk, int newprio),

	TP_ARGS(tsk, newprio),

	TP_STRUCT__entry(
		__array( char,	comm,	TASK_COMM_LEN	)
		__field( pid_t,	pid			)
		__field( int,	oldprio			)
		__field( int,	newprio			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid		= tsk->pid;
		__entry->oldprio	= tsk->prio;
		__entry->newprio	= newprio;
	),

	TP_printk("comm=%s pid=%d oldprio=%d newprio=%d",
			__entry->comm, __entry->pid,
			__entry->oldprio, __entry->newprio)
);

/*
 * Extract the complete scheduling information from the before
 * and after the change of priority.
 */
TRACE_EVENT_MAP(sched_pi_setprio, sched_pi_update_prio,

	TP_PROTO(struct task_struct *tsk, int newprio),

	TP_ARGS(tsk, newprio),

	TP_STRUCT__entry(
		__array( char,	comm,	TASK_COMM_LEN	)
		__field( pid_t,	pid			)
		__field( unsigned int,	old_policy	)
		__field( int,	old_nice		)
		__field( unsigned int,	old_rt_priority	)
		__field( u64,	old_dl_runtime		)
		__field( u64,	old_dl_deadline		)
		__field( u64,	old_dl_period		)
		__array( char,	top_waiter_comm,	TASK_COMM_LEN	)
		__field( pid_t,	top_waiter_pid		)
		__field( unsigned int,	new_policy	)
		__field( int,	new_nice		)
		__field( unsigned int,	new_rt_priority	)
		__field( u64,	new_dl_runtime		)
		__field( u64,	new_dl_deadline		)
		__field( u64,	new_dl_period		)
	),

	TP_fast_assign(
		struct task_struct *top_waiter = rt_mutex_get_top_task(tsk);

		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid			= tsk->pid;
		__entry->old_policy		= effective_policy(
							tsk->policy, tsk->prio);
		__entry->old_nice		= task_nice(tsk);
		__entry->old_rt_priority	= effective_rt_prio(
							tsk->prio);
		__entry->old_dl_runtime	= dl_prio(tsk->prio) ?
							tsk->dl.dl_runtime : 0;
		__entry->old_dl_deadline	= dl_prio(tsk->prio) ?
							tsk->dl.dl_deadline : 0;
		__entry->old_dl_period		= dl_prio(tsk->prio) ?
							tsk->dl.dl_period : 0;
		if (top_waiter) {
			memcpy(__entry->top_waiter_comm, top_waiter->comm, TASK_COMM_LEN);
			__entry->top_waiter_pid		= top_waiter->pid;
			/*
			 * The effective policy depends on the current policy of
			 * the target task.
			 */
			__entry->new_policy		= effective_policy(
								tsk->policy, top_waiter->prio);
			__entry->new_nice		= task_nice(top_waiter);
			__entry->new_rt_priority	= effective_rt_prio(
								top_waiter->prio);
			__entry->new_dl_runtime	= dl_prio(top_waiter->prio) ?
								top_waiter->dl.dl_runtime : 0;
			__entry->new_dl_deadline	= dl_prio(top_waiter->prio) ?
								top_waiter->dl.dl_deadline : 0;
			__entry->new_dl_period	= dl_prio(top_waiter->prio) ?
								top_waiter->dl.dl_period : 0;
		} else {
			__entry->top_waiter_comm[0]	= '\0';
			__entry->top_waiter_pid		= -1;
			__entry->new_policy		= 0;
			__entry->new_nice		= 0;
			__entry->new_rt_priority	= 0;
			__entry->new_dl_runtime	= 0;
			__entry->new_dl_deadline	= 0;
			__entry->new_dl_period	= 0;
		}
	),

	TP_printk("comm=%s, pid=%d, old_policy=%s, old_nice=%d, "
			"old_rt_priority=%u, old_dl_runtime=%Lu, "
			"old_dl_deadline=%Lu, old_dl_period=%Lu, "
			"top_waiter_comm=%s, top_waiter_pid=%d, new_policy=%s, "
			"new_nice=%d, new_rt_priority=%u, "
			"new_dl_runtime=%Lu, new_dl_deadline=%Lu, "
			"new_dl_period=%Lu",
		__entry->comm, __entry->pid,
		__print_symbolic(__entry->old_policy, SCHEDULING_POLICY),
		__entry->old_nice, __entry->old_rt_priority,
		__entry->old_dl_runtime, __entry->old_dl_deadline,
		__entry->old_dl_period,
		__entry->top_waiter_comm, __entry->top_waiter_pid,
		__entry->new_policy >= 0 ?
			__print_symbolic(__entry->new_policy,
				SCHEDULING_POLICY) : "",
		__entry->new_nice, __entry->new_rt_priority,
		__entry->new_dl_runtime, __entry->new_dl_deadline,
		__entry->new_dl_period)
);

#ifdef CONFIG_DETECT_HUNG_TASK
TRACE_EVENT(sched_process_hang,
	TP_PROTO(struct task_struct *tsk),
	TP_ARGS(tsk),

	TP_STRUCT__entry(
		__array( char,	comm,	TASK_COMM_LEN	)
		__field( pid_t,	pid			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid = tsk->pid;
	),

	TP_printk("comm=%s pid=%d", __entry->comm, __entry->pid)
);
#endif /* CONFIG_DETECT_HUNG_TASK */

DECLARE_EVENT_CLASS(sched_move_task_template,

	TP_PROTO(struct task_struct *tsk, int src_cpu, int dst_cpu),

	TP_ARGS(tsk, src_cpu, dst_cpu),

	TP_STRUCT__entry(
		__field( pid_t,	pid			)
		__field( pid_t,	tgid			)
		__field( pid_t,	ngid			)
		__field( int,	src_cpu			)
		__field( int,	src_nid			)
		__field( int,	dst_cpu			)
		__field( int,	dst_nid			)
	),

	TP_fast_assign(
		__entry->pid		= task_pid_nr(tsk);
		__entry->tgid		= task_tgid_nr(tsk);
		__entry->ngid		= task_numa_group_id(tsk);
		__entry->src_cpu	= src_cpu;
		__entry->src_nid	= cpu_to_node(src_cpu);
		__entry->dst_cpu	= dst_cpu;
		__entry->dst_nid	= cpu_to_node(dst_cpu);
	),

	TP_printk("pid=%d tgid=%d ngid=%d src_cpu=%d src_nid=%d dst_cpu=%d dst_nid=%d",
			__entry->pid, __entry->tgid, __entry->ngid,
			__entry->src_cpu, __entry->src_nid,
			__entry->dst_cpu, __entry->dst_nid)
);

/*
 * Tracks migration of tasks from one runqueue to another. Can be used to
 * detect if automatic NUMA balancing is bouncing between nodes
 */
DEFINE_EVENT(sched_move_task_template, sched_move_numa,
	TP_PROTO(struct task_struct *tsk, int src_cpu, int dst_cpu),

	TP_ARGS(tsk, src_cpu, dst_cpu)
);

DEFINE_EVENT(sched_move_task_template, sched_stick_numa,
	TP_PROTO(struct task_struct *tsk, int src_cpu, int dst_cpu),

	TP_ARGS(tsk, src_cpu, dst_cpu)
);

TRACE_EVENT(sched_swap_numa,

	TP_PROTO(struct task_struct *src_tsk, int src_cpu,
		 struct task_struct *dst_tsk, int dst_cpu),

	TP_ARGS(src_tsk, src_cpu, dst_tsk, dst_cpu),

	TP_STRUCT__entry(
		__field( pid_t,	src_pid			)
		__field( pid_t,	src_tgid		)
		__field( pid_t,	src_ngid		)
		__field( int,	src_cpu			)
		__field( int,	src_nid			)
		__field( pid_t,	dst_pid			)
		__field( pid_t,	dst_tgid		)
		__field( pid_t,	dst_ngid		)
		__field( int,	dst_cpu			)
		__field( int,	dst_nid			)
	),

	TP_fast_assign(
		__entry->src_pid	= task_pid_nr(src_tsk);
		__entry->src_tgid	= task_tgid_nr(src_tsk);
		__entry->src_ngid	= task_numa_group_id(src_tsk);
		__entry->src_cpu	= src_cpu;
		__entry->src_nid	= cpu_to_node(src_cpu);
		__entry->dst_pid	= task_pid_nr(dst_tsk);
		__entry->dst_tgid	= task_tgid_nr(dst_tsk);
		__entry->dst_ngid	= task_numa_group_id(dst_tsk);
		__entry->dst_cpu	= dst_cpu;
		__entry->dst_nid	= cpu_to_node(dst_cpu);
	),

	TP_printk("src_pid=%d src_tgid=%d src_ngid=%d src_cpu=%d src_nid=%d dst_pid=%d dst_tgid=%d dst_ngid=%d dst_cpu=%d dst_nid=%d",
			__entry->src_pid, __entry->src_tgid, __entry->src_ngid,
			__entry->src_cpu, __entry->src_nid,
			__entry->dst_pid, __entry->dst_tgid, __entry->dst_ngid,
			__entry->dst_cpu, __entry->dst_nid)
);

/*
 * Tracepoint for waking a polling cpu without an IPI.
 */
TRACE_EVENT(sched_wake_idle_without_ipi,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(	int,	cpu	)
	),

	TP_fast_assign(
		__entry->cpu	= cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);
#endif /* _TRACE_SCHED_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
