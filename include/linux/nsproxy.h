#ifndef _LINUX_NSPROXY_H
#define _LINUX_NSPROXY_H

#include <linux/spinlock.h>
#include <linux/sched.h>

struct mnt_namespace;
struct uts_namespace;
struct ipc_namespace;
struct pid_namespace;
struct fs_struct;

/*
 * A structure to contain pointers to all per-process
 * namespaces - fs (mount), uts, network, sysvipc, etc.
 *
 * The pid namespace is an exception -- it's accessed using
 * task_active_pid_ns.  The pid namespace here is the
 * namespace that children will use.
 *
 * 'count' is the number of tasks holding a reference.
 * The count for each namespace, then, will be the number
 * of nsproxies pointing to it, not the number of tasks.
 *
 * The nsproxy is shared by tasks which share all namespaces.
 * As soon as a single namespace is cloned or unshared, the
 * nsproxy is copied.
 */
struct nsproxy {
	atomic_t count;
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children;
	struct net 	     *net_ns;
};
extern struct nsproxy init_nsproxy;

/*
 * the namespaces access rules are:
 *
 *  1. only current task is allowed to change current->nsproxy pointer or
 *     any pointer on the nsproxy itself.
 *
 *  2. the access to other task namespaces (reader) are very rare and short
 *     lived, enough to refcount whatever resource we are dealing with. This
 *     remote reader access is performed like this:
 *
 *     set_reader_nsproxy(task);
 *     nsproxy = task_nsproxy(task);
 *     if (nsproxy != NULL) {
 *             / *
 *               * work with the namespaces here
 *               * i.e. get the reference on one of them
 *               * /
 *     } / *
 *         * NULL task->nsproxy means that this task is
 *         * almost dead (zombie)
 *         * /
 *     clear_reader_nsproxy(task);
 *
 *  3. above guarantees 1 & 2 enable writer pointer fastpath optimizations
 *     and proxy on the task's alloc_lock as a slowpath. Otherwise the common
 *     case will be that nobody is peeking into our ns and, synchronized via
 *     [Rmw], we can skip any locks altogether when setting a new namespace,
 *     i.e. switch_task_namespaces().
 */

#define NSPROXY_READER	1UL

static inline void set_reader_nsproxy(struct task_struct *tsk)
{
	/*
	 * In case there is contention on the alloc_lock, toggle
	 * readers before we try to acquire it. Any incoming writer
	 * must sync-up at this point.
	 */
	(void)xchg(&tsk->nsproxy, (struct nsproxy *)
		   ((unsigned long)tsk->nsproxy | NSPROXY_READER));
	task_lock(tsk);
}

static inline void clear_reader_nsproxy(struct task_struct *tsk)
{
	task_unlock(tsk);
	(void)xchg(&tsk->nsproxy, (struct nsproxy *)
		   ((unsigned long)tsk->nsproxy & ~NSPROXY_READER));
}

static inline struct nsproxy *task_nsproxy(struct task_struct *tsk)
{
	return (struct nsproxy *)
		((unsigned long)READ_ONCE(tsk->nsproxy) & ~NSPROXY_READER);
}

int copy_namespaces(unsigned long flags, struct task_struct *tsk);
void exit_task_namespaces(struct task_struct *tsk);
void switch_task_namespaces(struct task_struct *tsk, struct nsproxy *new);
void free_nsproxy(struct nsproxy *ns);
int unshare_nsproxy_namespaces(unsigned long, struct nsproxy **,
	struct cred *, struct fs_struct *);
int __init nsproxy_cache_init(void);

static inline void put_nsproxy(struct nsproxy *ns)
{
	if (atomic_dec_and_test(&ns->count)) {
		free_nsproxy(ns);
	}
}

static inline void get_nsproxy(struct nsproxy *ns)
{
	atomic_inc(&ns->count);
}

#endif
