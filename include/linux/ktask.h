/*
 * ktask.h
 *
 * Framework to parallelize cpu-intensive kernel work such as zeroing
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

struct ktask_ctl;
struct ktask_node;

#define	KTASK_RETURN_SUCCESS	0
#define	KTASK_RETURN_ERROR	(-1)

#ifdef CONFIG_KTASK

/**
 * ktask_run() runs one task.  It doesn't account for NUMA locality.
 *
 * @start: An object that describes the start of the task.  The client thread
 *         function interprets the object however it sees fit (e.g. an array
 *         index, a simple pointer, or a pointer to a more complicated
 *         representation of job position.
 * @task_size:  The size of the task (units are task-specific).
 * @ctl:  A control structure containing information about the task, including
 *        the client thread function (see the definition of struct ktask_ctl).
 *
 * RETURNS:
 * KTASK_RETURN_SUCCESS or KTASK_RETURN_ERROR.
 *
 * XXX include ks_error in ktask_ctl instead so client can provide own error
 * information in void *?
 */
int ktask_run(void *start, size_t task_size, struct ktask_ctl *ctl);

/**
 * ktask_run_numa() runs one task while accounting for NUMA locality.
 *
 * The ktask framework ensures worker threads are scheduled on a CPU local to
 * each chunk of a task.  The client is responsible for organizing the work
 * along NUMA boundaries in the 'nodes' array.
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

/*
 * XXX Two possible future enhancements related to error handling, should the
 * need arise, are:
 *
 * - Add client specific error reporting.  It's possible for tasks to fail for
 *   different reasons, so let the client pass a pointer for its own error
 *   information.
 *
 * - Allow clients to pass an "undo" callback to ktask that is responsible for
 *   undoing those parts of the task that fail if an error occurs.
 */

#else  /* CONFIG_KTASK */

#define ktask_run(start, task_size, ctl)				      \
	((ctl)->kc_thread_func((start),				              \
			       (ctl)->kc_iter_advance((start), (task_size)),  \
			       (ctl)->kc_thread_func_arg))

#define ktask_run_numa(nodes, nr_nodes, ctl)				      \
({									      \
	size_t __i;							      \
	int __ret = KTASK_RETURN_SUCCESS;				      \
									      \
	for (__i = 0; __i < (nr_nodes); ++__i) {			      \
		__ret = (ctl)->kc_thread_func(				      \
			    (nodes)->kn_start,				      \
			    (ctl)->kc_iter_advance((nodes)->kn_start,	      \
						   (nodes)->kn_task_size),    \
			    (ctl)->kc_thread_func_arg);			      \
									      \
		if (__ret == KTASK_RETURN_ERROR)			      \
			break;						      \
	}								      \
									      \
	__ret;								      \
})

#endif /* CONFIG_KTASK */

/**
 * Holds per-NUMA-node information about a task.
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
 * @KTASK_ASYNC: ktask_run* won't wait for the task to finish before returning.
 */
enum ktask_flags {
	/* XXX last ktask thread has to kfree the struct ktask_node's */
	KTASK_ASYNC = 0x1,
};

/**
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
 * An iterator function that advances the given position 'nsteps'.
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
 * An iterator function for a contiguous range.  Clients should use this to
 * avoid reinventing the wheel for this common case.
 *
 * The arguments and return value are the same as the ktask_iter_func function
 * pointer type.
 */
void *ktask_iter_range(void *position, size_t nsteps);

/**
 * Client-provided per-task control information.
 *
 * @kc_thread_func: A thread function that completes one chunk of the task per
 *                  call.
 * @kc_thread_func_arg: An argument to be passed to the thread function.
 * @kc_iter_advance: An iterator function to advance the iterator by some number
 *                   of task-specific units.
 * @kc_min_chunk_size: The minimum chunk size in task-specific units.  This
 *                     allows the client to communicate the minimum amount of
 *                     work that's appropriate for one worker thread to do at
 *                     once.
 * @kc_gfp_flags: gfp flags for allocating ktask metadata during the task.
 * @kc_flags: ktask flags that control the behavior of the job:
 *    - KTASK_ASYNC: ktask API calls may return before the task is finished.
 *                   By default, ktask API calls do not return until the
 *                   task is finished.
 */
struct ktask_ctl {
	ktask_thread_func	kc_thread_func;
	void			*kc_thread_func_arg;
	ktask_iter_func		kc_iter_advance;
	size_t			kc_min_chunk_size;
	gfp_t			kc_gfp_flags;
	int			kc_flags;
};

#define KTASK_CTL_INITIALIZER(thread_func, thread_func_arg, iter_advance, \
			      min_chunk_size, gfp_flags, flags)		  \
	{								  \
		.kc_thread_func = (ktask_thread_func)(thread_func),	  \
		.kc_thread_func_arg = (thread_func_arg),		  \
		.kc_iter_advance = (iter_advance),			  \
		.kc_min_chunk_size = (min_chunk_size),			  \
		.kc_gfp_flags = (gfp_flags),				  \
		.kc_flags = (flags),					  \
	}

/**
 * Convenience macro to pack a struct ktask_ctl.
 *
 * Note that KTASK_CTL_INITIALIZER casts 'thread_func' to be of type
 * ktask_thread_func.  This is to help clients write cleaner thread functions
 * by relieving them of the need to cast the three void * arguments.  Clients
 * can just use the actual argument types instead.
 */
#define DEFINE_KTASK_CTL(ctl_name, thread_func, thread_func_arg,	  \
			 iter_advance, min_chunk_size, gfp_flags, flags)  \
	struct ktask_ctl ctl_name =					  \
		KTASK_CTL_INITIALIZER(thread_func, thread_func_arg,	  \
				      iter_advance, min_chunk_size,	  \
				      gfp_flags, flags)
/**
 * Similar to DEFINE_KTASK_CTL, but omits the iterator argument in favor of
 * using ktask_iter_range.
 */
#define DEFINE_KTASK_CTL_RANGE(ctl_name, thread_func, thread_func_arg,	  \
			 min_chunk_size, gfp_flags, flags)		  \
	struct ktask_ctl ctl_name =					  \
		KTASK_CTL_INITIALIZER(thread_func, thread_func_arg,	  \
				      ktask_iter_range, min_chunk_size,	  \
				      gfp_flags, flags)

#endif /* _LINUX_KTASK_H */
