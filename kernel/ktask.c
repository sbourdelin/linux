/*
 * ktask.c
 *
 * Framework to parallelize CPU-intensive kernel work such as zeroing
 * huge pages or freeing many pages at once.  For more information, see
 * Documentation/core-api/ktask.rst.
 *
 * This is the ktask implementation; everything in this file is private to
 * ktask.
 */

#define pr_fmt(fmt)	"ktask: " fmt

#include <linux/ktask.h>

#ifdef CONFIG_KTASK

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktask_internal.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

/* Resource limits on the amount of workqueue items queued through ktask. */
spinlock_t ktask_rlim_lock;
/* Work items queued on all nodes (includes NUMA_NO_NODE) */
size_t ktask_rlim_cur;
size_t ktask_rlim_max;
/* Work items queued per node */
size_t *ktask_rlim_node_cur;
size_t *ktask_rlim_node_max;

/* Allow only 80% of the cpus to be running additional ktask threads. */
#define	KTASK_CPUFRAC_NUMER	4
#define	KTASK_CPUFRAC_DENOM	5

/* Used to pass ktask data to the workqueue API. */
struct ktask_work {
	struct work_struct	kw_work;
	struct ktask_task	*kw_task;
	int			kw_ktask_node_i;
	int			kw_queue_nid;
	struct list_head	kw_list;	/* ktask_free_works linkage */
};

static LIST_HEAD(ktask_free_works);
struct ktask_work *ktask_works;

/* Represents one task.  This is for internal use only. */
struct ktask_task {
	struct ktask_ctl	kt_ctl;
	size_t			kt_total_size;
	size_t			kt_chunk_size;
	/* mutex protects nodes, nr_nodes_left, nthreads_fini, error */
	struct mutex		kt_mutex;
	struct ktask_node	*kt_nodes;
	size_t			kt_nr_nodes;
	size_t			kt_nr_nodes_left;
	size_t			kt_nthreads;
	size_t			kt_nthreads_fini;
	int			kt_error; /* tracks error(s) from thread_func */
	struct completion	kt_ktask_done;
};

/*
 * Shrink the size of each job by this shift amount to load balance between the
 * worker threads.
 */
#define	KTASK_LOAD_BAL_SHIFT		2

#define	KTASK_DEFAULT_MAX_THREADS	4

/* Maximum number of threads for a single task. */
int ktask_max_threads = KTASK_DEFAULT_MAX_THREADS;

static struct workqueue_struct *ktask_wq;
static struct workqueue_struct *ktask_nonuma_wq;

static void ktask_thread(struct work_struct *work);

static inline void ktask_init_work(struct ktask_work *kw, struct ktask_task *kt,
			    size_t ktask_node_i, size_t queue_nid)
{
	INIT_WORK(&kw->kw_work, ktask_thread);
	kw->kw_task = kt;
	kw->kw_ktask_node_i = ktask_node_i;
	kw->kw_queue_nid = queue_nid;
}

static void ktask_queue_work(struct ktask_work *kw)
{
	struct workqueue_struct *wq;
	int cpu;

	if (kw->kw_queue_nid == NUMA_NO_NODE) {
		/*
		 * If no node is specified, use ktask_nonuma_wq to
		 * allow the thread to run on any node, but fall back
		 * to ktask_wq if we couldn't allocate ktask_nonuma_wq.
		 */
		cpu = WORK_CPU_UNBOUND;
		wq = (ktask_nonuma_wq) ?: ktask_wq;
	} else {
		/*
		 * WQ_UNBOUND workqueues, such as the one ktask uses,
		 * execute work on some CPU from the node of the CPU we
		 * pass to queue_work_on, so just pick any CPU to stand
		 * for the node on NUMA systems.
		 *
		 * On non-NUMA systems, cpumask_of_node becomes
		 * cpu_online_mask.
		 */
		cpu = cpumask_any(cpumask_of_node(kw->kw_queue_nid));
		wq = ktask_wq;
	}

	WARN_ON(!queue_work_on(cpu, wq, &kw->kw_work));
}

#ifdef CONFIG_NUMA

/* Returns true if we're migrating this part of the task to another node. */
static bool ktask_node_migrate(struct ktask_node *old_kn, struct ktask_node *kn,
			       size_t ktask_node_i, struct ktask_work *kw,
			       struct ktask_task *kt)
{
	int new_queue_nid;

	/*
	 * Don't migrate a user thread, otherwise migrate only if we're going
	 * to a different node.
	 */
	if (!(current->flags & PF_KTHREAD) || kn->kn_nid == old_kn->kn_nid ||
	    num_online_nodes() == 1)
		return false;

	/* Adjust resource limits. */
	spin_lock(&ktask_rlim_lock);
	if (kw->kw_queue_nid != NUMA_NO_NODE)
		--ktask_rlim_node_cur[kw->kw_queue_nid];

	if (kn->kn_nid != NUMA_NO_NODE &&
	    ktask_rlim_node_cur[kw->kw_queue_nid] <
	    ktask_rlim_node_max[kw->kw_queue_nid]) {
		new_queue_nid = kn->kn_nid;
		++ktask_rlim_node_cur[new_queue_nid];
	} else {
		new_queue_nid = NUMA_NO_NODE;
	}
	spin_unlock(&ktask_rlim_lock);

	ktask_init_work(kw, kt, ktask_node_i, new_queue_nid);
	ktask_queue_work(kw);

	return true;
}

#else /* CONFIG_NUMA */

static bool ktask_node_migrate(struct ktask_node *old_kn, struct ktask_node *kn,
			       size_t ktask_node_i, struct ktask_work *kw,
			       struct ktask_task *kt)
{
	return false;
}

#endif /* CONFIG_NUMA */

static void ktask_thread(struct work_struct *work)
{
	struct ktask_work  *kw;
	struct ktask_task  *kt;
	struct ktask_ctl   *kc;
	struct ktask_node  *kn;
	bool               done;

	kw = container_of(work, struct ktask_work, kw_work);
	kt = kw->kw_task;
	kc = &kt->kt_ctl;
	kn = &kt->kt_nodes[kw->kw_ktask_node_i];

	mutex_lock(&kt->kt_mutex);

	while (kt->kt_total_size > 0 && kt->kt_error == KTASK_RETURN_SUCCESS) {
		void *start, *end;
		size_t nsteps;
		int ret;

		if (kn->kn_task_size == 0) {
			/* The current node is out of work; pick a new one. */
			size_t remaining_nodes_seen = 0;
			size_t new_idx = prandom_u32_max(kt->kt_nr_nodes_left);
			struct ktask_node *old_kn;
			size_t i;

			WARN_ON(kt->kt_nr_nodes_left == 0);
			WARN_ON(new_idx >= kt->kt_nr_nodes_left);
			for (i = 0; i < kt->kt_nr_nodes; ++i) {
				if (kt->kt_nodes[i].kn_task_size == 0)
					continue;

				if (remaining_nodes_seen >= new_idx)
					break;

				++remaining_nodes_seen;
			}
			/* We should have found work on another node. */
			WARN_ON(i >= kt->kt_nr_nodes);

			old_kn = kn;
			kn = &kt->kt_nodes[i];

			/* Start another worker on the node we've chosen. */
			if (ktask_node_migrate(old_kn, kn, i, kw, kt)) {
				mutex_unlock(&kt->kt_mutex);
				return;
			}
		}

		start = kn->kn_start;
		nsteps = min(kt->kt_chunk_size, kn->kn_task_size);
		end = kc->kc_iter_func(start, nsteps);
		kn->kn_start = end;
		WARN_ON(kn->kn_task_size < nsteps);
		kn->kn_task_size -= nsteps;
		WARN_ON(kt->kt_total_size < nsteps);
		kt->kt_total_size -= nsteps;
		if (kn->kn_task_size == 0) {
			WARN_ON(kt->kt_nr_nodes_left == 0);
			kt->kt_nr_nodes_left--;
		}

		mutex_unlock(&kt->kt_mutex);

		ret = kc->kc_thread_func(start, end, kc->kc_thread_func_arg);

		mutex_lock(&kt->kt_mutex);

		if (ret == KTASK_RETURN_ERROR)
			kt->kt_error = KTASK_RETURN_ERROR;
	}

	WARN_ON(kt->kt_nr_nodes_left > 0 &&
		kt->kt_error == KTASK_RETURN_SUCCESS);

	++kt->kt_nthreads_fini;
	WARN_ON(kt->kt_nthreads_fini > kt->kt_nthreads);
	done = (kt->kt_nthreads_fini == kt->kt_nthreads);
	mutex_unlock(&kt->kt_mutex);

	if (done)
		complete(&kt->kt_ktask_done);
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
		chunk_size = rounddown(chunk_size, min_chunk_size);

	return max(chunk_size, min_chunk_size);
}

/*
 * Prepares to run the task by computing the number of threads, checking
 * the ktask resource limits, finding the chunk size, and initializing the
 * work items.
 */
static size_t ktask_prepare_threads(struct ktask_node *nodes, size_t nr_nodes,
				    struct ktask_task *kt,
				    struct list_head *to_queue)
{
	size_t i, nthreads, nthreads_check;
	size_t min_chunk_size = kt->kt_ctl.kc_min_chunk_size;
	size_t max_threads    = kt->kt_ctl.kc_max_threads;

	if (!ktask_wq)
		return 1;

	if (max_threads == 0)
		max_threads = ktask_max_threads;

	/* Ensure at least one thread when task_size < min_chunk_size. */
	nthreads_check = DIV_ROUND_UP(kt->kt_total_size, min_chunk_size);
	nthreads_check = min_t(size_t, nthreads_check, num_online_cpus());
	nthreads_check = min_t(size_t, nthreads_check, max_threads);

	/*
	 * Use at least the current thread for this task; check whether
	 * ktask_rlim allows additional work items to be queued.
	 */
	nthreads = 1;
	spin_lock(&ktask_rlim_lock);
	for (i = nthreads; i < nthreads_check; ++i) {
		/* Spread threads across nodes evenly. */
		size_t ktask_node_i = i % nr_nodes;
		struct ktask_node *kn = &nodes[ktask_node_i];
		struct ktask_work *kw;
		int nid = kn->kn_nid;
		int queue_nid;

		WARN_ON(ktask_rlim_cur > ktask_rlim_max);
		if (ktask_rlim_cur == ktask_rlim_max)
			break;	/* No more work items allowed to be queued. */

		/* Allowed to queue on requested node? */
		if (nid != NUMA_NO_NODE &&
		    ktask_rlim_node_cur[nid] < ktask_rlim_node_max[nid]) {
			WARN_ON(ktask_rlim_node_cur[nid] > ktask_rlim_cur);
			++ktask_rlim_node_cur[nid];
			queue_nid = nid;
		} else {
			queue_nid = NUMA_NO_NODE;
		}

		BUG_ON(list_empty(&ktask_free_works));
		kw = list_first_entry(&ktask_free_works, struct ktask_work,
				      kw_list);
		list_move_tail(&kw->kw_list, to_queue);
		ktask_init_work(kw, kt, ktask_node_i, queue_nid);

		++ktask_rlim_cur;
		++nthreads;
	}
	spin_unlock(&ktask_rlim_lock);

	return nthreads;
}

int ktask_run_numa(struct ktask_node *nodes, size_t nr_nodes,
		   struct ktask_ctl *ctl)
{
	size_t i;
	struct ktask_work kw;
	struct ktask_work *kw_cur, *kw_next;
	LIST_HEAD(to_queue);
	struct ktask_task kt = {
		.kt_ctl             = *ctl,
		.kt_total_size      = 0,
		.kt_nodes           = nodes,
		.kt_nr_nodes        = nr_nodes,
		.kt_nr_nodes_left   = nr_nodes,
		.kt_nthreads_fini   = 0,
		.kt_error           = KTASK_RETURN_SUCCESS,
	};

	for (i = 0; i < nr_nodes; ++i) {
		kt.kt_total_size += nodes[i].kn_task_size;
		if (nodes[i].kn_task_size == 0)
			kt.kt_nr_nodes_left--;

		WARN_ON(nodes[i].kn_nid >= MAX_NUMNODES);
	}

	if (kt.kt_total_size == 0)
		return KTASK_RETURN_SUCCESS;

	mutex_init(&kt.kt_mutex);

	kt.kt_nthreads = ktask_nthreads(kt.kt_total_size,
					ctl->kc_min_chunk_size,
					ctl->kc_max_threads);

	kt.kt_chunk_size = ktask_chunk_size(kt.kt_total_size,
					ctl->kc_min_chunk_size, kt.kt_nthreads);

	init_completion(&kt.kt_ktask_done);

	kt.kt_nthreads = ktask_prepare_threads(nodes, nr_nodes, &kt, &to_queue);
	kt.kt_chunk_size = ktask_chunk_size(kt.kt_total_size,
					    ctl->kc_min_chunk_size,
					    kt.kt_nthreads);

	list_for_each_entry_safe(kw_cur, kw_next, &to_queue, kw_list)
		ktask_queue_work(kw_cur);

	/*
	 * Make ourselves one of the threads, which saves launching a workqueue
	 * worker.
	 */
	INIT_WORK(&kw.kw_work, ktask_thread);
	kw.kw_task = &kt;
	kw.kw_ktask_node_i = 0;
	ktask_thread(&kw.kw_work);

	/* Wait for all the jobs to finish. */
	wait_for_completion(&kt.kt_ktask_done);

	spin_lock(&ktask_rlim_lock);

	/* Put the works back on the free list, adjusting rlimits. */
	list_for_each_entry_safe(kw_cur, kw_next, &to_queue, kw_list) {
		if (kw_cur->kw_queue_nid != NUMA_NO_NODE) {
			WARN_ON(ktask_rlim_node_cur[kw_cur->kw_queue_nid] == 0);
			--ktask_rlim_node_cur[kw_cur->kw_queue_nid];
		}
		WARN_ON(ktask_rlim_cur == 0);
		--ktask_rlim_cur;
	}
	list_splice(&to_queue, &ktask_free_works);
	spin_unlock(&ktask_rlim_lock);

	mutex_destroy(&kt.kt_mutex);

	return kt.kt_error;
}
EXPORT_SYMBOL_GPL(ktask_run_numa);

int ktask_run(void *start, size_t task_size, struct ktask_ctl *ctl)
{
	struct ktask_node node;

	node.kn_start = start;
	node.kn_task_size = task_size;
	node.kn_nid = numa_node_id();

	return ktask_run_numa(&node, 1, ctl);
}
EXPORT_SYMBOL_GPL(ktask_run);

/*
 * Initialize internal limits on work items queued.  Work items submitted to
 * cmwq capped at 80% of online cpus both system-wide and per-node to maintain
 * an efficient level of parallelization at these respective levels.
 */
bool ktask_rlim_init(void)
{
	int node;
	unsigned nr_node_cpus;

	spin_lock_init(&ktask_rlim_lock);

	ktask_rlim_node_cur = kcalloc(num_possible_nodes(),
					       sizeof(size_t),
					       GFP_KERNEL);
	if (!ktask_rlim_node_cur) {
		pr_warn("can't alloc rlim counts (ktask disabled)");
		return false;
	}

	ktask_rlim_node_max = kmalloc_array(num_possible_nodes(),
						     sizeof(size_t),
						     GFP_KERNEL);
	if (!ktask_rlim_node_max) {
		kfree(ktask_rlim_node_cur);
		pr_warn("can't alloc rlim maximums (ktask disabled)");
		return false;
	}

	ktask_rlim_max = mult_frac(num_online_cpus(), KTASK_CPUFRAC_NUMER,
						      KTASK_CPUFRAC_DENOM);
	for_each_node(node) {
		nr_node_cpus = cpumask_weight(cpumask_of_node(node));
		ktask_rlim_node_max[node] = mult_frac(nr_node_cpus,
						      KTASK_CPUFRAC_NUMER,
						      KTASK_CPUFRAC_DENOM);
	}

	return true;
}

void __init ktask_init(void)
{
	struct workqueue_attrs *attrs;
	int i, ret;

	if (!ktask_rlim_init())
		goto out;

	ktask_works = kmalloc_array(ktask_rlim_max, sizeof(struct ktask_work),
				    GFP_KERNEL);
	if (!ktask_works) {
		pr_warn("failed to alloc ktask_works (ktask disabled)");
		goto out;
	}
	for (i = 0; i < ktask_rlim_max; ++i)
		list_add_tail(&ktask_works[i].kw_list, &ktask_free_works);

	ktask_wq = alloc_workqueue("ktask_wq", WQ_UNBOUND, 0);
	if (!ktask_wq) {
		pr_warn("failed to alloc ktask_wq (ktask disabled)");
		goto out;
	}

	/*
	 * Threads executing work from this workqueue can run on any node on
	 * the system.  If we get any failures below, use ktask_wq in its
	 * place.  It's better than nothing.
	 */
	ktask_nonuma_wq = alloc_workqueue("ktask_nonuma_wq", WQ_UNBOUND, 0);
	if (!ktask_nonuma_wq) {
		pr_warn("failed to alloc ktask_nonuma_wq");
		goto out;
	}

	attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if (!attrs) {
		pr_warn("alloc_workqueue_attrs failed");
		goto alloc_fail;
	}

	attrs->no_numa = true;

	ret = apply_workqueue_attrs(ktask_nonuma_wq, attrs);
	if (ret != 0) {
		pr_warn("apply_workqueue_attrs failed");
		goto apply_fail;
	}

	free_workqueue_attrs(attrs);
out:
	return;

apply_fail:
	free_workqueue_attrs(attrs);
alloc_fail:
	destroy_workqueue(ktask_nonuma_wq);
	ktask_nonuma_wq = NULL;
}

#endif /* CONFIG_KTASK */

/*
 * This function is defined outside CONFIG_KTASK so it can be called in the
 * !CONFIG_KTASK versions of ktask_run and ktask_run_numa.
 */
void *ktask_iter_range(void *position, size_t nsteps)
{
	return (char *)position + nsteps;
}
