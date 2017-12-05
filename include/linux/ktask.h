/*
 * ktask.h
 *
 * Framework to parallelize CPU-intensive kernel work such as zeroing
 * huge pages or freeing many pages at once.  For more information, see
 * Documentation/core-api/ktask.rst.
 *
 * This is the interface to ktask; everything in this file is
 * accessible to ktask clients.
 *
 * If CONFIG_KTASK=n, calls to the ktask API are simply #define'd to run the
 * thread function that the client provides so that the task is completed
 * without concurrency in the current thread.
 */

#ifndef _LINUX_KTASK_H
#define _LINUX_KTASK_H

#include <linux/types.h>

#define	KTASK_RETURN_SUCCESS	0
#define	KTASK_RETURN_ERROR	(-1)

/**
 * struct ktask_node - Holds per-NUMA-node information about a task.
 *
 * @kn_start: An object that describes the start of the task on this NUMA node.
 * @kn_task_size: The size of the task on this NUMA node (units are
 *                task-specific).
 * @kn_nid: The NUMA node id (or NUMA_NO_NODE, in which case the work is done on
 *          the current node).
 */
struct ktask_node {
	void		*kn_start;
	size_t		kn_task_size;
	int		kn_nid;
};

/**
 * typedef ktask_thread_func
 *
 * Called on each chunk of work that a ktask thread does, where the chunk is
 * delimited by [start, end).  A thread may call this multiple times during one
 * task.
 *
 * @start: An object that describes the start of the chunk.
 * @end: An object that describes the end of the chunk.
 * @arg: The thread function argument (provided with struct ktask_ctl).
 *
 * RETURNS:
 * KTASK_RETURN_SUCCESS or KTASK_RETURN_ERROR.
 */
typedef int (*ktask_thread_func)(void *start, void *end, void *arg);

/**
 * typedef ktask_iter_func
 *
 * An iterator function that advances the position by a given number of steps.
 *
 * @position: An object that describes the current position in the task.
 * @nsteps: The number of steps to advance in the task (in task-specific
 *          units).
 *
 * RETURNS:
 * An object representing the new position.
 */
typedef void *(*ktask_iter_func)(void *position, size_t nsteps);

/**
 * ktask_iter_range
 *
 * An iterator function for a contiguous range such as an array or address
 * range.  This is the default iterator; clients may override with
 * ktask_ctl_set_iter_func.
 *
 * @position: An object that describes the current position in the task.
 *            Interpreted as an unsigned long.
 * @nsteps: The number of steps to advance in the task (in task-specific
 *          units).
 *
 * RETURNS:
 * (position + nsteps)
 */
void *ktask_iter_range(void *position, size_t nsteps);

/**
 * struct ktask_ctl - Client-provided per-task control information.
 *
 * @kc_thread_func: A thread function that completes one chunk of the task per
 *                  call.
 * @kc_thread_func_arg: An argument to be passed to the thread function.
 * @kc_iter_func: An iterator function to advance the iterator by some number
 *                   of task-specific units.
 * @kc_min_chunk_size: The minimum chunk size in task-specific units.  This
 *                     allows the client to communicate the minimum amount of
 *                     work that's appropriate for one worker thread to do at
 *                     once.
 * @kc_max_threads: The maximum number of threads to use for the task.
 *                  The actual number used may be less than this if the
 *                  framework determines that fewer threads would be better,
 *                  taking into account such things as total CPU count and
 *                  task size.  Pass 0 to use ktask's default maximum.
 */
struct ktask_ctl {
	/* Required arguments set with DEFINE_KTASK_CTL. */
	ktask_thread_func	kc_thread_func;
	void			*kc_thread_func_arg;
	size_t			kc_min_chunk_size;

	/*
	 * Optional arguments set with ktask_ctl_set_* functions.  Defaults
	 * listed to the side.
	 */
	ktask_iter_func		kc_iter_func;    /* ktask_iter_range */
	size_t			kc_max_threads;  /* 0 (uses internal limit) */
};

#define KTASK_CTL_INITIALIZER(thread_func, thread_func_arg, min_chunk_size)  \
	{								     \
		.kc_thread_func = (ktask_thread_func)(thread_func),	     \
		.kc_thread_func_arg = (thread_func_arg),		     \
		.kc_min_chunk_size = (min_chunk_size),			     \
		.kc_iter_func = (ktask_iter_range),			     \
		.kc_max_threads = (0),					     \
	}

/*
 * Note that KTASK_CTL_INITIALIZER casts 'thread_func' to be of type
 * ktask_thread_func.  This is to help clients write cleaner thread functions
 * by relieving them of the need to cast the three void * arguments.  Clients
 * can just use the actual argument types instead.
 */
#define DEFINE_KTASK_CTL(ctl_name, thread_func, thread_func_arg,	  \
			 min_chunk_size)				  \
	struct ktask_ctl ctl_name =					  \
		KTASK_CTL_INITIALIZER(thread_func, thread_func_arg,	  \
				      min_chunk_size)

/**
 * ktask_ctl_set_iter_func - Set a task-specific iterator
 *
 * This overrides the default iterator, ktask_iter_range.
 *
 * @ctl:  A control structure containing information about the task.
 * @iter_func:  Client-provided iterator function that conforms to the
 *              declaration of ktask_iter_func.
 */
static inline void ktask_ctl_set_iter_func(struct ktask_ctl *ctl,
					   ktask_iter_func iter_func)
{
	ctl->kc_iter_func = iter_func;
}

/**
 * ktask_ctl_set_max_threads - Set a task-specific maximum number of threads
 *
 * This overrides the default maximum, which is KTASK_DEFAULT_MAX_THREADS
 * initially and may be changed via /proc/sys/debug/ktask_max_threads.
 *
 * @ctl:  A control structure containing information about the task.
 * @max_threads:  The maximum number of threads to be started for this task.
 *                The actual number of threads may be less than this.
 */
static inline void ktask_ctl_set_max_threads(struct ktask_ctl *ctl,
					     size_t max_threads)
{
	ctl->kc_max_threads = max_threads;
}

#ifdef CONFIG_KTASK

/**
 * ktask_run - Runs one task.
 *
 * Starts threads to complete one task with the given thread function.  Waits
 * for the task to finish before returning.
 *
 * On a NUMA system, threads run on the current node.  This is designed to
 * mirror other parts of the kernel that favor locality, such as the default
 * memory policy of allocating pages from the same node as the calling thread.
 * ktask_run_numa may be used to get more control over where threads run.
 *
 * @start: An object that describes the start of the task.  The client thread
 *         function interprets the object however it sees fit (e.g. an array
 *         index, a simple pointer, or a pointer to a more complicated
 *         representation of job position).
 * @task_size:  The size of the task (units are task-specific).
 * @ctl:  A control structure containing information about the task, including
 *        the client thread function.
 *
 * RETURNS:
 * KTASK_RETURN_SUCCESS or KTASK_RETURN_ERROR.
 */
int ktask_run(void *start, size_t task_size, struct ktask_ctl *ctl);

/**
 * ktask_run_numa - Runs one task while accounting for NUMA locality.
 *
 * Starts threads on the requested nodes to complete one task with the given
 * thread function.  The client is responsible for organizing the work along
 * NUMA boundaries in the 'nodes' array.  Waits for the task to finish before
 * returning.
 *
 * In the special case of NUMA_NO_NODE, threads are allowed to run on any node.
 * This is distinct from ktask_run, which runs threads on the current node.
 *
 * @nodes: An array of struct ktask_node's, each of which describes the task on
 *         a NUMA node (see struct ktask_node).
 * @nr_nodes:  The length of the 'nodes' array.
 * @ctl:  A control structure containing information about the task (see
 *        the definition of struct ktask_ctl).
 *
 * RETURNS:
 * KTASK_RETURN_SUCCESS or KTASK_RETURN_ERROR.
 */
int ktask_run_numa(struct ktask_node *nodes, size_t nr_nodes,
		   struct ktask_ctl *ctl);

void ktask_init(void);

#else  /* CONFIG_KTASK */

static inline int ktask_run(void *start, size_t task_size,
			    struct ktask_ctl *ctl)
{
	return ctl->kc_thread_func(start,
				   ctl->kc_iter_func(start, task_size),
				   ctl->kc_thread_func_arg);
}

static inline int ktask_run_numa(struct ktask_node *nodes, size_t nr_nodes,
				 struct ktask_ctl *ctl)
{
	size_t i;
	int err = KTASK_RETURN_SUCCESS;

	for (i = 0; i < nr_nodes; ++i) {
		err = ctl->kc_thread_func(
			    nodes[i].kn_start,
			    ctl->kc_iter_func(nodes[i].kn_start,
						 nodes[i].kn_task_size),
			    ctl->kc_thread_func_arg);

		if (err == KTASK_RETURN_ERROR)
			break;
	}

	return err;
}

static inline void ktask_init(void) { }

#endif /* CONFIG_KTASK */

#endif /* _LINUX_KTASK_H */
