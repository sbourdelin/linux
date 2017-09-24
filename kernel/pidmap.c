#include <linux/bitops.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/pidmap.h>

#define PIDMAP_PARAM	(~PIDMAP_IGNORE_KTHREADS)

static inline bool pidmap_perm(const struct pid_namespace *pid_ns)
{
	return pid_ns->hide_pid < HIDEPID_INVISIBLE || in_group_p(pid_ns->pid_gid);
}

static bool skip_task(struct task_struct *task, bool has_perms, int flags)
{
	int param = flags & PIDMAP_PARAM;

	if (!task)
		return true;
	if (!has_perms && !ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
		return true;
	if ((flags & PIDMAP_IGNORE_KTHREADS) && (task->flags & PF_KTHREAD))
		return true;
	if (param == PIDMAP_PROC && !thread_group_leader(task))
		return true;
	return false;
}

static long pidmap_tasks(int __user *pids, unsigned int count,
		  unsigned int start, int flags)
{
	struct pid_namespace *pid_ns = task_active_pid_ns(current);
	unsigned int start_page, start_elem;
	unsigned int last_pos = 0;
	unsigned int last_set_pid = 0;
	unsigned long mask;
	bool has_perms;
	unsigned int i;

	/*
	 * Pid 0 does not exist, however, corresponding bit is always set in
	 * ->pidmap[0].page, so we should skip it.
	 */
	if (start == 0)
		start = 1;

	if (start > pid_ns->last_pid)
		return 0;

	has_perms = pidmap_perm(pid_ns);

	start_page = start / BITS_PER_PAGE;
	start_elem = (start % BITS_PER_PAGE) / BITS_PER_LONG;
	mask = ~0UL << (start % BITS_PER_LONG);

	for (i = start_page; i < PIDMAP_ENTRIES; i++) {
		unsigned int j;

		/*
		 * ->pidmap[].page is set once to a valid pointer,
		 *  therefore do not take any locks.
		 */
		if (!pid_ns->pidmap[i].page)
			continue;

		for (j = start_elem; j < PAGE_SIZE/sizeof(unsigned long); j++) {
			unsigned long val;

			val = *((unsigned long *)pid_ns->pidmap[i].page + j);
			val &= mask;
			mask = ~0UL;
			while (val != 0) {
				struct task_struct *task;

				if (last_pos == count)
					return last_pos;

				last_set_pid = i * BITS_PER_PAGE +
					j * BITS_PER_LONG + __ffs(val);

				rcu_read_lock();
				task = find_task_by_pid_ns(last_set_pid, pid_ns);
				if (skip_task(task, has_perms, flags)) {
					rcu_read_unlock();
					goto next;
				}
				rcu_read_unlock();

				if (put_user(last_set_pid, pids + last_pos))
					return -EFAULT;
				last_pos++;
				if (last_set_pid == pid_ns->last_pid)
					return last_pos;
next:
				val &= (val - 1);
			}
		}
		start_elem = 0;
	}
	if (last_set_pid == 0)
		return 0;
	else
		return last_pos;
}

static struct task_struct *pidmap_get_task(pid_t pid, bool *has_perms)
{
	struct pid_namespace *pid_ns;
	struct task_struct *task;

	if (pid == 0) {
		*has_perms = true;
		return current;
	}

	pid_ns = task_active_pid_ns(current);
	task = find_task_by_pid_ns(pid, pid_ns);
	if (!task)
		return ERR_PTR(-ESRCH);
	*has_perms = pidmap_perm(pid_ns);
	if (!*has_perms && !ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
		return ERR_PTR(-EACCES);
	return task;
}

static long pidmap_children(pid_t pid, int __user *upid,
			    unsigned int count, unsigned int start)
{
	struct task_struct *task, *child;
	bool has_perms;
	int pids[64];
	unsigned int i;
	unsigned int ret;

	rcu_read_lock();
	task = pidmap_get_task(pid, &has_perms);
	if (IS_ERR(task)) {
		rcu_read_unlock();
		return PTR_ERR(task);
	}

	i = 0;
	ret = 0;
	list_for_each_entry(child, &task->children, sibling) {
		if (start) {
			start--;
			continue;
		}

		if (!has_perms &&
		    !ptrace_may_access(child, PTRACE_MODE_READ_FSCREDS))
			continue;

		pids[i++] = child->tgid;
		if (i >= ARRAY_SIZE(pids)) {
			get_task_struct(task);
			get_task_struct(child);
			rcu_read_unlock();

			if (copy_to_user(upid, pids, i * sizeof(int))) {
				put_task_struct(child);
				put_task_struct(task);
				return -EFAULT;
			}
			upid += i;
			ret += i;
			i = 0;

			rcu_read_lock();
			put_task_struct(child);
			put_task_struct(task);

			if (!pid_alive(task) || !pid_alive(child))
				break;
		}
		if (--count == 0)
			break;
	}
	rcu_read_unlock();
	if (i > 0) {
		if (copy_to_user(upid, pids, i * sizeof(int)))
			return -EFAULT;
		ret += i;
	}
	return ret;
}

static long pidmap_threads(pid_t pid, int __user *upid,
			   unsigned int count, unsigned int start)
{
	struct task_struct *task, *thread;
	bool has_perms;
	int pids[64];
	unsigned int i;
	unsigned int ret;

	rcu_read_lock();
	task = pidmap_get_task(pid, &has_perms);
	if (IS_ERR(task)) {
		rcu_read_unlock();
		return PTR_ERR(task);
	}

	i = 0;
	ret = 0;
	for_each_thread(task, thread) {
		if (start) {
			start--;
			continue;
		}

		pids[i++] = thread->pid;
		if (i >= ARRAY_SIZE(pids)) {
			get_task_struct(task);
			get_task_struct(thread);
			rcu_read_unlock();

			if (copy_to_user(upid, pids, i * sizeof(int))) {
				put_task_struct(thread);
				put_task_struct(task);
				return -EFAULT;
			}
			upid += i;
			ret += i;
			i = 0;

			rcu_read_lock();
			put_task_struct(thread);
			put_task_struct(task);

			if (!pid_alive(task) || !pid_alive(thread))
				break;
		}
		if (--count == 0)
			break;
	}
	rcu_read_unlock();
	if (i > 0) {
		if (copy_to_user(upid, pids, i * sizeof(int)))
			return -EFAULT;
		ret += i;
	}
	return ret;
}

/**
 * pidmap - get allocated PIDs
 * @pids: destination buffer.
 * @count: number of elements in the buffer.
 * @start: PID to start from or PIDs number already readed.
 * @flags: flags.
 *
 * Write allocated PIDs to a buffer. @start specifies PID to start from
 * with PIDMAP_TASKS or PIDMAP_PROC flags, or number of PIDs already
 * readed otherwise.
 *
 * PIDs are filled from pid namespace of the calling process POV:
 * unshare(CLONE_NEWPID)+fork+pidmap in child will always return 1/1.
 *
 * pidmap(2) hides PIDs inaccessible at /proc mounted with "hidepid" option.
 *
 * Note, pidmap(2) does not guarantee that any of returned PID exists
 * by the time system call exits.
 *
 * Return: number of PIDs written to the buffer or error code otherwise.
 */
SYSCALL_DEFINE5(pidmap, pid_t, pid, int __user *, pids,
		unsigned int, count, unsigned int, start, int, flags)
{
	int param = flags & PIDMAP_PARAM;

	switch (param) {
	case PIDMAP_TASKS:
	case PIDMAP_PROC:
		return pidmap_tasks(pids, count, start, flags);
	case PIDMAP_CHILDREN:
		return pidmap_children(pid, pids, count, start);
	case PIDMAP_THREADS:
		return pidmap_threads(pid, pids, count, start);
	}
	return -EINVAL;
}
