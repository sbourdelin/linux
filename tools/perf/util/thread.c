// SPDX-License-Identifier: GPL-2.0
#include "../perf.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/kernel.h>
#include "session.h"
#include "thread.h"
#include "thread-stack.h"
#include "util.h"
#include "debug.h"
#include "namespaces.h"
#include "comm.h"
#include "unwind.h"

#include <api/fs/fs.h>

struct map_groups *thread__get_map_groups(struct thread *thread, u64 timestamp)
{
	struct map_groups *mg;
	struct thread *leader = thread;

	BUG_ON(thread->mg == NULL);

	if (thread->tid != thread->pid_) {
		leader = machine__find_thread_by_time(thread->mg->machine,
						      thread->pid_, thread->pid_,
						      timestamp);
		if (leader == NULL)
			goto out;
	}

	list_for_each_entry(mg, &leader->mg_list, list)
		if (timestamp >= mg->timestamp)
			return mg;

out:
	return thread->mg;
}

int thread__set_map_groups(struct thread *thread, struct map_groups *mg,
			   u64 timestamp)
{
	struct list_head *pos;
	struct map_groups *old;

	if (mg == NULL)
		return -ENOMEM;

	/*
	 * Only a leader thread can have map groups list - others
	 * reference it through map_groups__get.  This means the
	 * leader thread will have one more refcnt than others.
	 */
	if (thread->tid != thread->pid_)
		return -EINVAL;

	if (thread->mg) {
		BUG_ON(refcount_read(&thread->mg->refcnt) <= 1);
		map_groups__put(thread->mg);
	}

	/* sort by time */
	list_for_each(pos, &thread->mg_list) {
		old = list_entry(pos, struct map_groups, list);
		if (timestamp > old->timestamp)
			break;
	}

	list_add_tail(&mg->list, pos);
	mg->timestamp = timestamp;

	/* set current ->mg to most recent one */
	thread->mg = list_first_entry(&thread->mg_list, struct map_groups, list);
	/* increase one more refcnt for current */
	map_groups__get(thread->mg);

	return 0;
}

int thread__init_map_groups(struct thread *thread, struct machine *machine)
{
	pid_t pid = thread->pid_;

	if (pid == thread->tid || pid == -1) {
		struct map_groups *mg = map_groups__new(machine);

		if (thread__set_map_groups(thread, mg, 0) < 0)
			map_groups__put(mg);
	} else {
		struct thread *leader = __machine__findnew_thread(machine, pid, pid);
		if (leader) {
			thread->mg = map_groups__get(leader->mg);
			thread__put(leader);
		}
	}

	return thread->mg ? 0 : -1;
}

struct thread *thread__new(pid_t pid, pid_t tid)
{
	char *comm_str;
	struct comm *comm;
	struct thread *thread = zalloc(sizeof(*thread));

	if (thread != NULL) {
		thread->pid_ = pid;
		thread->tid = tid;
		thread->ppid = -1;
		thread->cpu = -1;
		INIT_LIST_HEAD(&thread->namespaces_list);
		INIT_LIST_HEAD(&thread->comm_list);
		init_rwsem(&thread->namespaces_lock);
		init_rwsem(&thread->comm_lock);
		INIT_LIST_HEAD(&thread->mg_list);

		comm_str = malloc(32);
		if (!comm_str)
			goto err_thread;

		snprintf(comm_str, 32, ":%d", tid);
		comm = comm__new(comm_str, 0, false);
		free(comm_str);
		if (!comm)
			goto err_thread;

		list_add(&comm->list, &thread->comm_list);
		refcount_set(&thread->refcnt, 1);
		INIT_LIST_HEAD(&thread->tid_list);
		RB_CLEAR_NODE(&thread->rb_node);
		/* Thread holds first ref to nsdata. */
		thread->nsinfo = nsinfo__new(pid);
	}

	return thread;

err_thread:
	free(thread);
	return NULL;
}

void thread__delete(struct thread *thread)
{
	struct namespaces *namespaces, *tmp_namespaces;
	struct comm *comm, *tmp;
	struct map_groups *mg, *tmp_mg;

	BUG_ON(!RB_EMPTY_NODE(&thread->rb_node));
	BUG_ON(!list_empty(&thread->tid_list));

	thread_stack__free(thread);

	if (thread->mg) {
		map_groups__put(thread->mg);
		thread->mg = NULL;
	}
	down_write(&thread->namespaces_lock);

	list_for_each_entry_safe(namespaces, tmp_namespaces,
				 &thread->namespaces_list, list) {
		list_del(&namespaces->list);
		namespaces__free(namespaces);
	}
	up_write(&thread->namespaces_lock);

	down_write(&thread->comm_lock);

	/* only leader threads have mg list */
	list_for_each_entry_safe(mg, tmp_mg, &thread->mg_list, list)
		map_groups__put(mg);

	list_for_each_entry_safe(comm, tmp, &thread->comm_list, list) {
		list_del(&comm->list);
		comm__free(comm);
	}
	up_write(&thread->comm_lock);

	unwind__finish_access(thread);
	nsinfo__zput(thread->nsinfo);

	exit_rwsem(&thread->namespaces_lock);
	exit_rwsem(&thread->comm_lock);
	free(thread);
}

struct thread *thread__get(struct thread *thread)
{
	if (thread)
		refcount_inc(&thread->refcnt);
	return thread;
}

void thread__put(struct thread *thread)
{
	if (thread && refcount_dec_and_test(&thread->refcnt)) {
		/*
		 * Remove it from the dead_threads list, as last reference
		 * is gone.
		 */
		if (!RB_EMPTY_NODE(&thread->rb_node)) {
			struct machine *machine = thread->mg->machine;
			struct threads *threads = machine__threads(machine, thread->tid);

			rb_erase(&thread->rb_node, &threads->dead);
			RB_CLEAR_NODE(&thread->rb_node);
		}

		list_del_init(&thread->tid_list);
		thread__delete(thread);
	}
}

struct namespaces *thread__namespaces(const struct thread *thread)
{
	if (list_empty(&thread->namespaces_list))
		return NULL;

	return list_first_entry(&thread->namespaces_list, struct namespaces, list);
}

static int __thread__set_namespaces(struct thread *thread, u64 timestamp,
				    struct namespaces_event *event)
{
	struct namespaces *new, *curr = thread__namespaces(thread);

	new = namespaces__new(event);
	if (!new)
		return -ENOMEM;

	list_add(&new->list, &thread->namespaces_list);

	if (timestamp && curr) {
		/*
		 * setns syscall must have changed few or all the namespaces
		 * of this thread. Update end time for the namespaces
		 * previously used.
		 */
		curr = list_next_entry(new, list);
		curr->end_time = timestamp;
	}

	return 0;
}

int thread__set_namespaces(struct thread *thread, u64 timestamp,
			   struct namespaces_event *event)
{
	int ret;

	down_write(&thread->namespaces_lock);
	ret = __thread__set_namespaces(thread, timestamp, event);
	up_write(&thread->namespaces_lock);
	return ret;
}

struct comm *thread__comm(const struct thread *thread)
{
	if (list_empty(&thread->comm_list))
		return NULL;

	return list_first_entry(&thread->comm_list, struct comm, list);
}

struct comm *thread__exec_comm(const struct thread *thread)
{
	struct comm *comm, *last = NULL;

	list_for_each_entry(comm, &thread->comm_list, list) {
		if (comm->exec)
			return comm;
		last = comm;
	}

	return last;
}

struct comm *thread__comm_by_time(const struct thread *thread, u64 timestamp)
{
	struct comm *comm;

	list_for_each_entry(comm, &thread->comm_list, list) {
		if (timestamp >= comm->start)
			return comm;
	}

	if (list_empty(&thread->comm_list))
		return NULL;

	return list_last_entry(&thread->comm_list, struct comm, list);
}

static int thread__clone_map_groups(struct thread *thread,
				    struct thread *parent);

static int ____thread__set_comm(struct thread *thread, const char *str,
				u64 timestamp, bool exec)
{
	struct comm *new, *curr = thread__comm(thread);

	/* Override the default :tid entry */
	if (!thread->comm_set) {
		int err = comm__override(curr, str, timestamp, exec);

		if (!thread->start_time)
			thread->start_time = timestamp;

		if (err)
			return err;
	} else {
		new = comm__new(str, timestamp, exec);
		if (!new)
			return -ENOMEM;

		/* sort by time */
		list_for_each_entry(curr, &thread->comm_list, list) {
			if (timestamp >= curr->start)
				break;
		}
		list_add_tail(&new->list, &curr->list);

		if (exec)
			unwind__flush_access(thread);
	}

	if (exec) {
		struct machine *machine;

		BUG_ON(thread->mg == NULL || thread->mg->machine == NULL);

		machine = thread->mg->machine;

		if (thread->tid != thread->pid_) {
			struct map_groups *old = thread->mg;
			struct thread *leader;

			leader = machine__findnew_thread(machine, thread->pid_,
							 thread->pid_);

			/* now it'll be a new leader */
			thread->pid_ = thread->tid;

			thread->mg = map_groups__new(old->machine);
			if (thread->mg == NULL)
				return -ENOMEM;

			/* save current mg in the new leader */
			thread__clone_map_groups(thread, leader);

			/* current mg of leader thread needs one more refcnt */
			map_groups__get(thread->mg);

			thread__set_map_groups(thread, thread->mg, old->timestamp);
		}

		/* create a new mg for newly executed binary */
		thread__set_map_groups(thread, map_groups__new(machine), timestamp);
	}

	thread->comm_set = true;

	return 0;
}

int __thread__set_comm(struct thread *thread, const char *str, u64 timestamp,
		       bool exec)
{
	int ret;

	down_write(&thread->comm_lock);
	ret = ____thread__set_comm(thread, str, timestamp, exec);
	up_write(&thread->comm_lock);
	return ret;
}

int thread__set_comm_from_proc(struct thread *thread)
{
	char path[64];
	char *comm = NULL;
	size_t sz;
	int err = -1;

	if (!(snprintf(path, sizeof(path), "%d/task/%d/comm",
		       thread->pid_, thread->tid) >= (int)sizeof(path)) &&
	    procfs__read_str(path, &comm, &sz) == 0) {
		comm[sz - 1] = '\0';
		err = thread__set_comm(thread, comm, 0);
	}

	return err;
}

static const char *__thread__comm_str(const struct thread *thread)
{
	const struct comm *comm = thread__comm(thread);

	if (!comm)
		return NULL;

	return comm__str(comm);
}

const char *thread__comm_str(const struct thread *thread)
{
	const char *str;

	down_read((struct rw_semaphore *)&thread->comm_lock);
	str = __thread__comm_str(thread);
	up_read((struct rw_semaphore *)&thread->comm_lock);

	return str;
}

const char *thread__comm_str_by_time(const struct thread *thread, u64 timestamp)
{
	const struct comm *comm = thread__comm_by_time(thread, timestamp);

	if (!comm)
		return NULL;

	return comm__str(comm);
}

/* CHECKME: it should probably better return the max comm len from its comm list */
int thread__comm_len(struct thread *thread)
{
	if (!thread->comm_len) {
		const char *comm = thread__comm_str(thread);
		if (!comm)
			return 0;
		thread->comm_len = strlen(comm);
	}

	return thread->comm_len;
}

size_t thread__fprintf(struct thread *thread, FILE *fp)
{
	return fprintf(fp, "Thread %d %s\n", thread->tid, thread__comm_str(thread)) +
	       map_groups__fprintf(thread->mg, fp);
}

int thread__insert_map(struct thread *thread, struct map *map)
{
	int ret;

	ret = unwind__prepare_access(thread, map, NULL);
	if (ret)
		return ret;

	if (perf_has_index) {
		map_groups__insert_by_time(thread->mg, map);
	} else {
		map_groups__fixup_overlappings(thread->mg, map, stderr);
		map_groups__insert(thread->mg, map);
	}

	return 0;
}

static int __thread__prepare_access(struct thread *thread)
{
	bool initialized = false;
	int err = 0;
	struct maps *maps = &thread->mg->maps;
	struct map *map;

	down_read(&maps->lock);

	for (map = maps__first(maps); map; map = map__next(map)) {
		err = unwind__prepare_access(thread, map, &initialized);
		if (err || initialized)
			break;
	}

	up_read(&maps->lock);

	return err;
}

static int thread__prepare_access(struct thread *thread)
{
	int err = 0;

	if (symbol_conf.use_callchain)
		err = __thread__prepare_access(thread);

	return err;
}

static int thread__clone_map_groups(struct thread *thread,
				    struct thread *parent)
{
	/* This is new thread, we share map groups for process. */
	if (thread->pid_ == parent->pid_)
		return thread__prepare_access(thread);

	if (thread->mg == parent->mg) {
		pr_debug("broken map groups on thread %d/%d parent %d/%d\n",
			 thread->pid_, thread->tid, parent->pid_, parent->tid);
		return 0;
	}

	/* But this one is new process, copy maps. */
	if (map_groups__clone(thread, parent->mg) < 0)
		return -ENOMEM;

	return 0;
}

int thread__fork(struct thread *thread, struct thread *parent, u64 timestamp)
{
	if (parent->comm_set) {
		const char *comm = thread__comm_str(parent);
		int err;
		if (!comm)
			return -ENOMEM;
		err = thread__set_comm(thread, comm, timestamp);
		if (err)
			return err;
	}

	thread->ppid = parent->tid;
	thread->start_time = timestamp;
	return thread__clone_map_groups(thread, parent);
}

void thread__find_cpumode_addr_location(struct thread *thread, u64 addr,
					struct addr_location *al)
{
	size_t i;
	const u8 cpumodes[] = {
		PERF_RECORD_MISC_USER,
		PERF_RECORD_MISC_KERNEL,
		PERF_RECORD_MISC_GUEST_USER,
		PERF_RECORD_MISC_GUEST_KERNEL
	};

	for (i = 0; i < ARRAY_SIZE(cpumodes); i++) {
		thread__find_symbol(thread, cpumodes[i], addr, al);
		if (al->map)
			break;
	}
}

struct thread *thread__main_thread(struct machine *machine, struct thread *thread)
{
	if (thread->pid_ == thread->tid)
		return thread__get(thread);

	if (thread->pid_ == -1)
		return NULL;

	return machine__find_thread(machine, thread->pid_, thread->pid_);
}

void thread__find_cpumode_addr_location_by_time(struct thread *thread,
						u64 addr, struct addr_location *al,
						u64 timestamp)
{
	size_t i;
	const u8 cpumodes[] = {
		PERF_RECORD_MISC_USER,
		PERF_RECORD_MISC_KERNEL,
		PERF_RECORD_MISC_GUEST_USER,
		PERF_RECORD_MISC_GUEST_KERNEL
	};

	if (!perf_has_index) {
		thread__find_cpumode_addr_location(thread, addr, al);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(cpumodes); i++) {
		thread__find_symbol_by_time(thread, cpumodes[i],
					    addr, al, timestamp);
		if (al->map)
			break;
	}
}
