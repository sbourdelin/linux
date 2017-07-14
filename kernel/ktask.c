/*
 * ktask.c
 *
 * Framework to parallelize cpu-intensive kernel work such as zeroing
 * huge pages or freeing many pages at once.  For more information, see
 * Documentation/core-api/ktask.rst.
 *
 * This is the ktask implementation; everything in this file is private to
 * ktask.
 */

#include <linux/ktask.h>

#ifdef CONFIG_KTASK

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * Shrink the size of each job by this shift amount to load balance between the
 * worker threads.
 */
#define	KTASK_LOAD_BAL_SHIFT		2

#define	KTASK_DEFAULT_MAX_THREADS	4

/* Maximum number of threads for a single task. */
int ktask_max_threads = KTASK_DEFAULT_MAX_THREADS;

static struct workqueue_struct *ktask_wq;

/* Used to pass ktask state to the workqueue API. */
struct ktask_work {
	struct work_struct kw_work;
	void               *kw_state;
};

/* Internal per-task state hidden from clients. */
struct ktask_state {
	struct ktask_ctl	ks_ctl;
	size_t			ks_total_size;
	size_t			ks_chunk_size;
	/* mutex protects nodes, nr_nodes_left, nthreads_fini, error */
	struct mutex		ks_mutex;
	struct ktask_node	*ks_nodes;
	size_t			ks_nr_nodes;
	size_t			ks_nr_nodes_left;
	size_t			ks_nthreads;
	size_t			ks_nthreads_fini;
	int			ks_error; /* tracks error(s) from thread_func */
	struct completion	ks_ktask_done;
};

static inline size_t ktask_get_start_node(struct ktask_node *nodes,
					  size_t nr_nodes)
{
	int cur_nid = numa_node_id();
	size_t fallback_i = 0;
	size_t i;

	for (i = 0; i < nr_nodes; ++i) {
		if (nodes[i].kn_nid == cur_nid)
			break;
		else if (nodes[i].kn_nid == NUMA_NO_NODE)
			fallback_i = i;
	}

	if (i >= nr_nodes)
		i = fallback_i;

	return i;
}

static void ktask_node_migrate(cpumask_var_t *saved_cpumask,
			       struct ktask_node *kn,
			       gfp_t gfp_flags, bool *migratedp)
{
	struct task_struct *p = current;
	const struct cpumask *node_cpumask;
	int ret;

	/* Don't migrate a user thread; migrating to NUMA_NO_NODE is nonsense */
	if (!(p->flags & PF_KTHREAD) || kn->kn_nid == NUMA_NO_NODE)
		return;

	node_cpumask = cpumask_of_node(kn->kn_nid);
	/* No cpu to migrate to. */
	if (cpumask_empty(node_cpumask))
		return;

	if (!*migratedp) {
		/*
		 * Save the workqueue thread's original mask so we can restore
		 * it after the task is done.
		 */
		if (!alloc_cpumask_var(saved_cpumask, gfp_flags))
			return;

		cpumask_copy(*saved_cpumask, &p->cpus_allowed);
	}

	ret = set_cpus_allowed_ptr(current, node_cpumask);
	if (ret == 0)
		*migratedp = true;
	else if (!*migratedp)
		free_cpumask_var(*saved_cpumask);
}

static void ktask_task(struct work_struct *work)
{
	struct ktask_work  *kw;
	struct ktask_state *ks;
	struct ktask_ctl   *kc;
	struct ktask_node  *kn;
	size_t             nidx;
	bool               done;
	bool               migrated = false;
	cpumask_var_t      saved_cpumask;

	kw = container_of(work, struct ktask_work, kw_work);
	ks = kw->kw_state;
	kc = &ks->ks_ctl;

	if (ks->ks_nr_nodes > 1)
		nidx = ktask_get_start_node(ks->ks_nodes, ks->ks_nr_nodes);
	else
		nidx = 0;

	WARN_ON(nidx >= ks->ks_nr_nodes);
	kn = &ks->ks_nodes[nidx];

	mutex_lock(&ks->ks_mutex);

	while (ks->ks_total_size > 0 && ks->ks_error == KTASK_RETURN_SUCCESS) {
		void *start, *end;
		size_t nsteps;
		int ret;

		if (kn->kn_task_size == 0) {
			/* The current node is out of work; pick a new one. */
			size_t remaining_nodes_seen = 0;
			size_t new_idx = prandom_u32_max(ks->ks_nr_nodes_left);

			WARN_ON(ks->ks_nr_nodes_left == 0);
			WARN_ON(new_idx >= ks->ks_nr_nodes_left);
			for (nidx = 0; nidx < ks->ks_nr_nodes; ++nidx) {
				if (ks->ks_nodes[nidx].kn_task_size == 0)
					continue;

				if (remaining_nodes_seen >= new_idx)
					break;

				++remaining_nodes_seen;
			}
			/* We should have found work on another node. */
			WARN_ON(nidx >= ks->ks_nr_nodes);

			kn = &ks->ks_nodes[nidx];

			/* Temporarily migrate to the node we just chose. */
			ktask_node_migrate(&saved_cpumask, kn, kc->kc_gfp_flags,
					   &migrated);
		}

		start = kn->kn_start;
		nsteps = min(ks->ks_chunk_size, kn->kn_task_size);
		end = kc->kc_iter_advance(start, nsteps);
		kn->kn_start = end;
		WARN_ON(kn->kn_task_size < nsteps);
		kn->kn_task_size -= nsteps;
		WARN_ON(ks->ks_total_size < nsteps);
		ks->ks_total_size -= nsteps;
		if (kn->kn_task_size == 0) {
			WARN_ON(ks->ks_nr_nodes_left == 0);
			ks->ks_nr_nodes_left--;
		}

		mutex_unlock(&ks->ks_mutex);

		ret = kc->kc_thread_func(start, end, kc->kc_thread_func_arg);

		mutex_lock(&ks->ks_mutex);

		if (ret == KTASK_RETURN_ERROR)
			ks->ks_error = KTASK_RETURN_ERROR;
	}

	WARN_ON(ks->ks_nr_nodes_left > 0 &&
		ks->ks_error == KTASK_RETURN_SUCCESS);

	++ks->ks_nthreads_fini;
	WARN_ON(ks->ks_nthreads_fini > ks->ks_nthreads);
	done = (ks->ks_nthreads_fini == ks->ks_nthreads);
	mutex_unlock(&ks->ks_mutex);

	if (migrated) {
		set_cpus_allowed_ptr(current, saved_cpumask);
		free_cpumask_var(saved_cpumask);
	}

	if (done)
		complete(&ks->ks_ktask_done);
}

/* Returns the number of threads to use for this task. */
static inline size_t ktask_nthreads(size_t task_size, size_t min_chunk_size)
{
	size_t nthreads;

	/* Ensure at least one thread when task_size < min_chunk_size. */
	nthreads = DIV_ROUND_UP(task_size, min_chunk_size);

	nthreads = min_t(size_t, nthreads, num_online_cpus());

	nthreads = min_t(size_t, nthreads, ktask_max_threads);

	return nthreads;
}

/*
 * Returns the number of chunks to break this task into.
 *
 * The number of chunks will be at least the number of threads, but in the
 * common case of a large task, the number of chunks will be greater to load
 * balance the work between threads in case some threads finish their work more
 * quickly than others.
 */
static inline size_t ktask_chunk_size(size_t task_size, size_t min_chunk_size,
				    size_t nthreads)
{
	size_t chunk_size;

	if (nthreads == 1)
		return task_size;

	chunk_size = (task_size / nthreads) >> KTASK_LOAD_BAL_SHIFT;

	/*
	 * chunk_size should be a multiple of min_chunk_size for tasks that
	 * need to operate in fixed-size batches.
	 */
	if (chunk_size > min_chunk_size)
		chunk_size -= (chunk_size % min_chunk_size);

	return max(chunk_size, min_chunk_size);
}

int ktask_run_numa(struct ktask_node *nodes, size_t nr_nodes,
		   struct ktask_ctl *ctl)
{
	size_t i;
	struct ktask_work *kw;
	struct ktask_state ks = {
		.ks_ctl             = *ctl,
		.ks_total_size        = 0,
		.ks_nodes           = nodes,
		.ks_nr_nodes        = nr_nodes,
		.ks_nr_nodes_left   = nr_nodes,
		.ks_nthreads_fini   = 0,
		.ks_error           = KTASK_RETURN_SUCCESS,
	};

	for (i = 0; i < nr_nodes; ++i) {
		ks.ks_total_size += nodes[i].kn_task_size;
		if (nodes[i].kn_task_size == 0)
			ks.ks_nr_nodes_left--;

		WARN_ON(nodes[i].kn_nid >= MAX_NUMNODES);
	}

	if (ks.ks_total_size == 0)
		return KTASK_RETURN_SUCCESS;

	mutex_init(&ks.ks_mutex);

	ks.ks_nthreads = ktask_nthreads(ks.ks_total_size,
					ctl->kc_min_chunk_size);

	ks.ks_chunk_size = ktask_chunk_size(ks.ks_total_size,
					ctl->kc_min_chunk_size, ks.ks_nthreads);

	init_completion(&ks.ks_ktask_done);

	kw = kmalloc_array(ks.ks_nthreads, sizeof(struct ktask_work),
			    ctl->kc_gfp_flags);
	if (unlikely(!kw)) {
		/* Low on memory; fall back to a single thread. */
		struct ktask_work kw = {
			.kw_work = __WORK_INITIALIZER(kw.kw_work, ktask_task),
			.kw_state = &ks
		};

		ks.ks_nthreads = 1;

		ktask_task(&kw.kw_work);
		mutex_destroy(&ks.ks_mutex);

		return ks.ks_error;
	}

	for (i = 1; i < ks.ks_nthreads; ++i) {
		int cpu;
		struct ktask_node *kn;

		INIT_WORK(&kw[i].kw_work, ktask_task);
		kw[i].kw_state = &ks;

		/*
		 * Spread workers evenly across nodes with work to do,
		 * starting each worker on a cpu local to the nid of their
		 * part of the task.
		 */
		kn = &nodes[i % nr_nodes];

		if (kn->kn_nid == NUMA_NO_NODE) {
			cpu = smp_processor_id();
		} else {
			/*
			 * WQ_UNBOUND workqueues execute work on a cpu from
			 * the node of the cpu we pass to queue_work_on, so
			 * just pick any cpu to stand for the node.
			 */
			cpu = cpumask_any(cpumask_of_node(kn->kn_nid));
		}

		queue_work_on(cpu, ktask_wq, &kw[i].kw_work);
	}

	/*
	 * Make ourselves one of the threads, which saves launching a workqueue
	 * worker.
	 */
	INIT_WORK(&kw[0].kw_work, ktask_task);
	kw[0].kw_state = &ks;
	ktask_task(&kw[0].kw_work);

	/* Wait for all the jobs to finish. */
	wait_for_completion(&ks.ks_ktask_done);

	kfree(kw);
	mutex_destroy(&ks.ks_mutex);

	return ks.ks_error;
}
EXPORT_SYMBOL_GPL(ktask_run_numa);

int ktask_run(void *start, size_t task_size, struct ktask_ctl *ctl)
{
	struct ktask_node node;

	node.kn_start = start;
	node.kn_task_size = task_size;
	node.kn_nid = NUMA_NO_NODE;

	return ktask_run_numa(&node, 1, ctl);
}
EXPORT_SYMBOL_GPL(ktask_run);

static int __init ktask_init(void)
{
	ktask_wq = alloc_workqueue("ktask_wq", WQ_UNBOUND, 0);
	if (!ktask_wq) {
		pr_err("%s: alloc_workqueue failed", __func__);
		return -1;
	}

	return 0;
}
core_initcall(ktask_init);

#endif /* CONFIG_KTASK */

/*
 * This function is defined outside CONFIG_KTASK so it can be called in the
 * ktask_run and ktask_run_numa macros defined in ktask.h for CONFIG_KTASK=n
 * kernels.
 */
void *ktask_iter_range(void *position, size_t nsteps)
{
	return (char *)position + nsteps;
}
