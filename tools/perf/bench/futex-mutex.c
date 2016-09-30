/*
 * Copyright (C) 2016 Waiman Long
 *
 * This microbenchmark simulates how the use of different futex types can
 * affect the actual performanace of userspace locking primitives like mutex.
 *
 * The raw throughput of the futex lock and unlock calls is not a good
 * indication of actual throughput of the mutex code as it may not really
 * need to call into the kernel. Therefore, 3 simple mutex lock and unlock
 * functions are written to implenment a mutex lock using the wait-wake,
 * PI and TP futexes respectively. These functions serve as the basis for
 * measuring the locking throughput.
 */

#include <pthread.h>

#include <signal.h>
#include <string.h>
#include "../util/stat.h"
#include "../perf-sys.h"
#include <subcmd/parse-options.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <errno.h>
#include "bench.h"
#include "futex.h"

#include <err.h>
#include <stdlib.h>
#include <sys/time.h>

#define	gettid()		syscall(SYS_gettid)
#define __cacheline_aligned	__attribute__((__aligned__(64)))

typedef u32 futex_t;
typedef void (*mutex_lock_fn_t)(futex_t *futex, int tid);
typedef void (*mutex_unlock_fn_t)(futex_t *futex, int tid);


struct worker {
	futex_t *futex;
	pthread_t thread;

	/*
	 * Per-thread operation statistics
	 */
	unsigned int ops;	/* # of locking operations	*/
	unsigned int locks;	/* # of lock futex calls	*/
	unsigned int unlocks;	/* # of unlock futex calls	*/
	unsigned int eagains;	/* # of EAGAIN errors		*/
	unsigned int lockerrs;	/* # of other lock errors	*/
	unsigned int unlockerrs;/* # of unlock errors		*/
	unsigned int wakeups;	/* # of wakeups (unlock return)	*/
	unsigned int handoffs;	/* # of lock handoff (TP only)	*/
	unsigned int steals;	/* # of lock steals (TP only)	*/
} __cacheline_aligned;

/*
 * Global cache-aligned futex
 */
static futex_t global_futex __cacheline_aligned;

static __thread futex_t thread_id;	/* Thread ID */
static __thread int counter;		/* Sleep counter */

static struct worker *worker;
static unsigned int nsecs = 10;
static bool verbose, done, fshared, exit_now;
static unsigned int ncpus, nthreads;
static int futex_flag;
static const char *ftype = "all";
static int csload = 1;
static int wratio;
struct timeval start, end, runtime;
static unsigned int worker_start;
static unsigned int threads_starting;
static struct stats throughput_stats;
static mutex_lock_fn_t mutex_lock_fn;
static mutex_unlock_fn_t mutex_unlock_fn;

/*
 * CS - Lock critical section
 */
static const struct option options[] = {
	OPT_STRING  ('f', "futex-type",	&ftype, "type", "Specify futex type: WW, PI, TP, all (default)"),
	OPT_INTEGER ('l', "load",	&csload,   "Specify # of cpu_relax's inside CS, default = 1"),
	OPT_UINTEGER('t', "threads",	&nthreads, "Specify number of threads, default = # of CPUs"),
	OPT_UINTEGER('r', "runtime",	&nsecs,    "Specify runtime (in seconds, default = 10s)"),
	OPT_BOOLEAN ('S', "shared",	&fshared,  "Use shared futexes instead of private ones"),
	OPT_BOOLEAN ('v', "verbose",	&verbose,  "Verbose mode: display thread-level details"),
	OPT_INTEGER ('w', "wait-ratio", &wratio,   "Specify <n>/1024 of CS is 1us sleep, default = 0"),
	OPT_END()
};

static const char * const bench_futex_mutex_usage[] = {
	"perf bench futex mutex <options>",
	NULL
};

/**
 * futex_cmpxchg() - atomic compare and exchange
 * @uaddr:	The address of the futex to be modified
 * @oldval:	The expected value of the futex
 * @newval:	The new value to try and assign the futex
 *
 * Implement cmpxchg using gcc atomic builtins.
 *
 * Return: the old futex value.
 */
static inline futex_t futex_cmpxchg(futex_t *uaddr, futex_t old, futex_t new)
{
	return __sync_val_compare_and_swap(uaddr, old, new);
}

/**
 * atomic_dec_return - atomically decrement & return the new value
 * @uaddr:	The address of the futex to be decremented
 * Return:	The new value
 */
static inline int atomic_dec_return(futex_t *uaddr)
{
	return __sync_sub_and_fetch(uaddr, 1);
}

/**
 * atomic_inc_return - atomically increment & return the new value
 * @uaddr:	The address of the futex to be incremented
 * Return:	The new value
 */
static inline int atomic_inc_return(futex_t *uaddr)
{
	return __sync_add_and_fetch(uaddr, 1);
}

/*
 * Wait-wake futex lock/unlock functions
 * futex value: 0 - unlocked
 *		1 - locked
 *		2 - locked with waiters (contended)
 */
static void ww_mutex_lock(futex_t *futex, int tid)
{
	futex_t old, val = *futex;
	int ret;

	for (;;) {
		if (!val) {
			val = futex_cmpxchg(futex, 0, 1);
			if (val == 0)
				return;
		}
		if (val != 2) {
			/*
			 * Force value to 2 to indicate waiter
			 */
			old = val;
			val = futex_cmpxchg(futex, old, 2);
			if (val == old)
				val = 2;
			else
				continue;
		}
		break;
	}

	for (;;) {
		ret = futex_wait(futex, 2, NULL, futex_flag);
		worker[tid].locks++;
		if (ret < 0) {
			if (errno == EAGAIN)
				worker[tid].eagains++;
			else
				worker[tid].lockerrs++;
		}

		val = *futex;
		if (val == 2)
			continue;
		for (;;) {
			old = val;
			val = futex_cmpxchg(futex, old, 2);
			if (old == val)
				break;
		}
		if (val == 0)
			break;	/* We got the lock */
	}
}

static void ww_mutex_unlock(futex_t *futex, int tid)
{
	futex_t old, val;
	int ret;

	val = *futex;
	do {
		old = val;
		val = futex_cmpxchg(futex, old, 0);
	} while (val != old);

	switch (val) {
	default:
	case 1: /* No waiter */
		break;

	case 2:
		worker[tid].unlocks++;
		ret = futex_wake(futex, 1, futex_flag);
		if (ret < 0)
			worker[tid].unlockerrs++;
		else
			worker[tid].wakeups += ret;
		break;
	}
}

/*
 * PI futex lock/unlock functions
 */
static void pi_mutex_lock(futex_t *futex, int tid)
{
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, 0, thread_id);
	if (val == 0)
		return;

	/*
	 * Retry if an error happens
	 */
	for (;;) {
		ret = futex_lock_pi(futex, NULL, futex_flag);
		worker[tid].locks++;
		if (ret >= 0)
			break;
		worker[tid].lockerrs++;
	}
}

static void pi_mutex_unlock(futex_t *futex, int tid)
{
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, thread_id, 0);
	if (val == thread_id)
		return;

	ret = futex_unlock_pi(futex, futex_flag);
	if (ret < 0)
		worker[tid].unlockerrs++;
	else
		worker[tid].wakeups += ret;
	worker[tid].unlocks++;
}

/*
 * TP futex lock/unlock functions
 */
static void tp_mutex_lock(futex_t *futex, int tid)
{
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, 0, thread_id);
	if (val == 0)
		return;

	/*
	 * Retry if an error happens
	 */
	for (;;) {
		ret = futex_lock(futex, NULL, futex_flag);
		worker[tid].locks++;
		if (ret >= 0)
			break;
		worker[tid].lockerrs++;
	}
	/*
	 * Check locking method
	 */
	if (!ret)
		worker[tid].steals++;
	else if (ret == 2)
		worker[tid].handoffs++;
}

static void tp_mutex_unlock(futex_t *futex, int tid)
{
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, thread_id, 0);
	if (val == thread_id)
		return;

	ret = futex_unlock(futex, futex_flag);
	if (ret < 0)
		worker[tid].unlockerrs++;
	else
		worker[tid].wakeups += ret;
	worker[tid].unlocks++;
}

/*
 * Load function
 */
static inline void load(int tid)
{
	int n = csload;

	/*
	 * Optionally does a 1us sleep instead if wratio is defined and
	 * is within bound.
	 */
	if (wratio && (((counter++ + tid) & 0x3ff) < wratio)) {
		usleep(1);
		return;
	}

	while (n-- > 0)
		cpu_relax();
}

/****************************************************************************/

static void toggle_done(int sig __maybe_unused,
			siginfo_t *info __maybe_unused,
			void *uc __maybe_unused)
{
	/* inform all threads that we're done for the day */
	done = true;
	gettimeofday(&end, NULL);
	timersub(&end, &start, &runtime);
	if (sig)
		exit_now = true;
}

static void *workerfn(void *arg)
{
	long tid = (long)arg;
	struct worker *w = &worker[tid];
	mutex_lock_fn_t lock_fn = mutex_lock_fn;
	mutex_unlock_fn_t unlock_fn = mutex_unlock_fn;

	thread_id = gettid();
	counter = 0;

	atomic_dec_return(&threads_starting);

	/*
	 * Busy wait until asked to start
	 */
	while (!worker_start)
		cpu_relax();

	do {
		lock_fn(w->futex, tid);
		load(tid);
		unlock_fn(w->futex, tid);
		w->ops++;	/* One more locking operation */
		cpu_relax();
	}  while (!done);

	return NULL;
}

static void create_threads(struct worker *w, pthread_attr_t *thread_attr,
			   long tid)
{
	cpu_set_t cpu;

	/*
	 * Bind each thread to a CPU
	 */
	CPU_ZERO(&cpu);
	CPU_SET(tid % ncpus, &cpu);
	w->futex = &global_futex;

	if (pthread_attr_setaffinity_np(thread_attr, sizeof(cpu_set_t), &cpu))
		err(EXIT_FAILURE, "pthread_attr_setaffinity_np");

	if (pthread_create(&w->thread, thread_attr, workerfn, (void *)tid))
		err(EXIT_FAILURE, "pthread_create");
}

static void futex_mutex_test(const char *futex_type)
{
	u64 us;
	unsigned int i;
	struct worker total;
	double avg, stddev;
	pthread_attr_t thread_attr;

	if (exit_now)
		return;

	if (!strcasecmp(futex_type, "WW")) {
		futex_type = "WW";
		mutex_lock_fn = ww_mutex_lock;
		mutex_unlock_fn = ww_mutex_unlock;
	} else if (!strcasecmp(futex_type, "PI")) {
		futex_type = "PI";
		mutex_lock_fn = pi_mutex_lock;
		mutex_unlock_fn = pi_mutex_unlock;
	} else if (!strcasecmp(futex_type, "TP")) {
		futex_type = "TP";
		mutex_lock_fn = tp_mutex_lock;
		mutex_unlock_fn = tp_mutex_unlock;

		/*
		 * Check if TP futex is supported.
		 */
		futex_unlock(&global_futex, 0);
		if (errno == ENOSYS) {
			fprintf(stderr, "\nTP futexes are not supported by the kernel!\n");
			return;
		}
	} else {
		fprintf(stderr, "Unknown futex type '%s'!\n", futex_type);
		exit(1);
	}

	printf("\n=====================================\n");
	printf("Run summary [PID %d]: %d threads doing %s futex lockings for %d secs.\n\n",
	       getpid(), nthreads, futex_type, nsecs);

	init_stats(&throughput_stats);

	global_futex = 0;
	done = false;
	threads_starting = nthreads;
	pthread_attr_init(&thread_attr);

	for (i = 0; i < nthreads; i++)
		create_threads(&worker[i], &thread_attr, i);
	pthread_attr_destroy(&thread_attr);

	while (threads_starting)
		usleep(1);

	gettimeofday(&start, NULL);

	/*
	 * Start the test
	 *
	 * Unlike the other futex benchmarks, this one uses busy waiting
	 * instead of pthread APIs to make sure that all the threads (except
	 * the one that shares CPU with the parent) will start more or less
	 * simultaineously.
	 */
	atomic_inc_return(&worker_start);
	sleep(nsecs);
	toggle_done(0, NULL, NULL);

	for (i = 0; i < nthreads; i++) {
		int ret = pthread_join(worker[i].thread, NULL);

		if (ret)
			err(EXIT_FAILURE, "pthread_join");
	}

	us = runtime.tv_sec * 1000000 + runtime.tv_usec;
	memset(&total, 0, sizeof(total));
	for (i = 0; i < nthreads; i++) {
		/*
		 * Get a rounded estimate of the # of locking ops/sec.
		 */
		u64 tp = ((u64)worker[i].ops) * 1000000 / us;

		total.ops        += worker[i].ops;
		total.locks      += worker[i].locks;
		total.unlocks    += worker[i].unlocks;
		total.wakeups    += worker[i].wakeups;
		total.eagains    += worker[i].eagains;
		total.lockerrs   += worker[i].lockerrs;
		total.unlockerrs += worker[i].unlockerrs;
		total.handoffs   += worker[i].handoffs;
		total.steals     += worker[i].steals;

		update_stats(&throughput_stats, tp);
		if (verbose)
			printf("[thread %3d] futex: %p [ %ld ops/sec ]\n",
			       i, worker[i].futex, (long)tp);
	}

	avg    = avg_stats(&throughput_stats);
	stddev = stddev_stats(&throughput_stats);

	printf("Locking statistics:\n");
	printf("Test run time      = %'.2f s\n", (double)us/1000000);
	printf("Total locking ops  = %'d\n", total.ops);
	printf("Lock futex calls   = %'d (%.1f%%)\n", total.locks,
		(double)total.locks*100/total.ops);
	printf("Unlock futex calls = %'d (%.1f%%)\n", total.unlocks,
		(double)total.unlocks*100/total.ops);
	if (total.wakeups)
		printf("Process wakeups    = %'d\n", total.wakeups);
	if (total.eagains)
		printf("EAGAIN lock errors = %'d\n", total.eagains);
	if (total.lockerrs)
		printf("Other lock errors  = %'d\n", total.lockerrs);
	if (total.unlockerrs)
		printf("Unlock errors      = %'d\n", total.unlockerrs);
	if (total.handoffs)
		printf("Lock handoffs      = %'d\n", total.handoffs);
	if (total.steals)
		printf("Lock stealings     = %'d\n", total.steals);

	printf("\nPer-thread Locking Rates:\n");
	printf("Avg = %'d ops/sec (+- %.2f%%)\n", (int)(avg + 0.5),
		rel_stddev_stats(stddev, avg));
	printf("Min = %'d ops/sec\n", (int)throughput_stats.min);
	printf("Max = %'d ops/sec\n", (int)throughput_stats.max);

	/* Clear the workers area */
	memset(worker, 0, sizeof(*worker) * nthreads);
}

int bench_futex_mutex(int argc, const char **argv,
		      const char *prefix __maybe_unused)
{
	struct sigaction act;

	argc = parse_options(argc, argv, options, bench_futex_mutex_usage, 0);
	if (argc)
		goto err;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	sigfillset(&act.sa_mask);
	act.sa_sigaction = toggle_done;
	sigaction(SIGINT, &act, NULL);

	if (!nthreads)
		nthreads = ncpus;

	worker = calloc(nthreads, sizeof(*worker));
	if (!worker)
		err(EXIT_FAILURE, "calloc");

	if (!fshared)
		futex_flag = FUTEX_PRIVATE_FLAG;

	if (!strcmp(ftype, "all")) {
		futex_mutex_test("WW");
		futex_mutex_test("PI");
		futex_mutex_test("TP");
	} else {
		futex_mutex_test(ftype);
	}
	free(worker);
	return 0;
err:
	usage_with_options(bench_futex_mutex_usage, options);
	exit(EXIT_FAILURE);
}
