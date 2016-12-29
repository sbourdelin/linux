/*
 * Copyright (C) 2016 Waiman Long <longman@redhat.com>
 *
 * This microbenchmark simulates how the use of different futex types can
 * affect the actual performanace of userspace locking primitives like mutex.
 *
 * The raw throughput of the futex lock and unlock calls is not a good
 * indication of actual throughput of the mutex code as it may not really
 * need to call into the kernel. Therefore, 3 sets of simple mutex lock and
 * unlock functions are written to implenment a mutex lock using the
 * wait-wake, PI and TP futexes respectively. These functions serve as the
 * basis for measuring the locking throughput.
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

#define CACHELINE_SIZE		64
#define gettid()		syscall(SYS_gettid)
#define __cacheline_aligned	__attribute__((__aligned__(CACHELINE_SIZE)))

typedef u32 futex_t;
typedef void (*lock_fn_t)(futex_t *futex, int tid);
typedef void (*unlock_fn_t)(futex_t *futex, int tid);

/*
 * Statistical count list
 */
enum {
	STAT_OPS,	/* # of exclusive locking operations	*/
	STAT_LOCKS,	/* # of exclusive lock futex calls	*/
	STAT_UNLOCKS,	/* # of exclusive unlock futex calls	*/
	STAT_SLEEPS,	/* # of exclusive lock sleeps		*/
	STAT_EAGAINS,	/* # of EAGAIN errors			*/
	STAT_WAKEUPS,	/* # of wakeups (unlock return)		*/
	STAT_HANDOFFS,	/* # of lock handoff (TP only)		*/
	STAT_STEALS,	/* # of lock steals (TP only)		*/
	STAT_LOCKERRS,	/* # of exclusive lock errors		*/
	STAT_UNLKERRS,	/* # of exclusive unlock errors		*/
	STAT_NUM	/* Total # of statistical count		*/
};

/*
 * Syscall time list
 */
enum {
	TIME_LOCK,	/* Total exclusive lock syscall time	*/
	TIME_UNLK,	/* Total exclusive unlock syscall time	*/
	TIME_NUM,
};

struct worker {
	futex_t *futex;
	pthread_t thread;

	/*
	 * Per-thread operation statistics
	 */
	u32 stats[STAT_NUM];

	/*
	 * Lock/unlock times
	 */
	u64 times[TIME_NUM];
} __cacheline_aligned;

/*
 * Global cache-aligned futex
 */
static futex_t __cacheline_aligned global_futex;
static futex_t *pfutex = &global_futex;

static __thread futex_t thread_id;	/* Thread ID */
static __thread int counter;		/* Sleep counter */

static struct worker *worker, *worker_alloc;
static unsigned int nsecs = 10;
static bool verbose, done, fshared, exit_now, timestat;
static unsigned int ncpus, nthreads;
static int flags;
static const char *ftype;
static int loadlat = 1;
static int locklat = 1;
static int wratio;
struct timeval start, end, runtime;
static unsigned int worker_start;
static unsigned int threads_starting;
static unsigned int threads_stopping;
static struct stats throughput_stats;
static lock_fn_t mutex_lock_fn;
static unlock_fn_t mutex_unlock_fn;

/*
 * Lock/unlock syscall time macro
 */
static inline void systime_add(int tid, int item, struct timespec *begin,
			       struct timespec *_end)
{
	worker[tid].times[item] += (_end->tv_sec  - begin->tv_sec)*1000000000 +
				    _end->tv_nsec - begin->tv_nsec;
}

static inline double stat_percent(struct worker *w, int top, int bottom)
{
	return (double)w->stats[top] * 100 / w->stats[bottom];
}

/*
 * Inline functions to update the statistical counts
 *
 * Enable statistics collection may sometimes impact the locking rates
 * to be measured. So we can specify the DISABLE_STAT macro to disable
 * statistic counts collection for all except the core locking rate counts.
 *
 * #define DISABLE_STAT
 */
#ifndef DISABLE_STAT
static inline void stat_add(int tid, int item, int num)
{
	worker[tid].stats[item] += num;
}

static inline void stat_inc(int tid, int item)
{
	stat_add(tid, item, 1);
}
#else
static inline void stat_add(int tid __maybe_unused, int item __maybe_unused,
			    int num __maybe_unused)
{
}

static inline void stat_inc(int tid __maybe_unused, int item __maybe_unused)
{
}
#endif

/*
 * The latency value within a lock critical section (load) and between locking
 * operations is in term of the number of cpu_relax() calls that are being
 * issued.
 */
static const struct option mutex_options[] = {
	OPT_INTEGER ('d', "locklat",	&locklat,  "Specify inter-locking latency (default = 1)"),
	OPT_STRING  ('f', "ftype",	&ftype,    "type", "Specify futex type: WW, PI, TP, all (default)"),
	OPT_INTEGER ('L', "loadlat",	&loadlat,  "Specify load latency (default = 1)"),
	OPT_UINTEGER('r', "runtime",	&nsecs,    "Specify runtime (in seconds, default = 10s)"),
	OPT_BOOLEAN ('S', "shared",	&fshared,  "Use shared futexes instead of private ones"),
	OPT_BOOLEAN ('T', "timestat",	&timestat, "Track lock/unlock syscall times"),
	OPT_UINTEGER('t', "threads",	&nthreads, "Specify number of threads, default = # of CPUs"),
	OPT_BOOLEAN ('v', "verbose",	&verbose,  "Verbose mode: display thread-level details"),
	OPT_INTEGER ('w', "wait-ratio", &wratio,   "Specify <n>/1024 of load is 1us sleep, default = 0"),
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
 * @newval:	The new value to try and assign to the futex
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
 * futex_xchg() - atomic exchange
 * @uaddr:	The address of the futex to be modified
 * @newval:	The new value to assign to the futex
 *
 * Implement cmpxchg using gcc atomic builtins.
 *
 * Return: the old futex value.
 */
static inline futex_t futex_xchg(futex_t *uaddr, futex_t new)
{
	return __sync_lock_test_and_set(uaddr, new);
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

/**********************[ MUTEX lock/unlock functions ]*********************/

/*
 * Wait-wake futex lock/unlock functions (Glibc implementation)
 * futex value: 0 - unlocked
 *		1 - locked
 *		2 - locked with waiters (contended)
 */
static void ww_mutex_lock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val = *futex;
	int ret;

	if (!val) {
		val = futex_cmpxchg(futex, 0, 1);
		if (val == 0)
			return;
	}

	for (;;) {
		if (val != 2) {
			/*
			 * Force value to 2 to indicate waiter
			 */
			val = futex_xchg(futex, 2);
			if (val == 0)
				return;
		}
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_wait(futex, 2, NULL, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_LOCK, &stime, &etime);
		} else {
			ret = futex_wait(futex, 2, NULL, flags);
		}

		stat_inc(tid, STAT_LOCKS);
		if (ret < 0) {
			if (errno == EAGAIN)
				stat_inc(tid, STAT_EAGAINS);
			else
				stat_inc(tid, STAT_LOCKERRS);
		}

		val = *futex;
	}
}

static void ww_mutex_unlock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_xchg(futex, 0);

	if (val == 2) {
		stat_inc(tid, STAT_UNLOCKS);
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_wake(futex, 1, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_UNLK, &stime, &etime);
		} else {
			ret = futex_wake(futex, 1, flags);
		}
		if (ret < 0)
			stat_inc(tid, STAT_UNLKERRS);
		else
			stat_add(tid, STAT_WAKEUPS, ret);
	}
}

/*
 * Alternate wait-wake futex lock/unlock functions with thread_id lock word
 */
static void ww2_mutex_lock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val = *futex;
	int ret;

	if (!val) {
		val = futex_cmpxchg(futex, 0, thread_id);
		if (val == 0)
			return;
	}

	for (;;) {
		/*
		 * Set the FUTEX_WAITERS bit, if not set yet.
		 */
		while (!(val & FUTEX_WAITERS)) {
			futex_t old;

			if (!val) {
				val = futex_cmpxchg(futex, 0, thread_id);
				if (val == 0)
					return;
				continue;
			}
			old = futex_cmpxchg(futex, val, val | FUTEX_WAITERS);
			if (old == val) {
				val |= FUTEX_WAITERS;
				break;
			}
			val = old;
		}
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_wait(futex, val, NULL, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_LOCK, &stime, &etime);
		} else {
			ret = futex_wait(futex, val, NULL, flags);
		}
		stat_inc(tid, STAT_LOCKS);
		if (ret < 0) {
			if (errno == EAGAIN)
				stat_inc(tid, STAT_EAGAINS);
			else
				stat_inc(tid, STAT_LOCKERRS);
		}

		val = *futex;
	}
}

static void ww2_mutex_unlock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_xchg(futex, 0);

	if ((val & FUTEX_TID_MASK) != thread_id)
		stat_inc(tid, STAT_UNLKERRS);

	if (val & FUTEX_WAITERS) {
		stat_inc(tid, STAT_UNLOCKS);
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_wake(futex, 1, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_UNLK, &stime, &etime);
		} else {
			ret = futex_wake(futex, 1, flags);
		}
		if (ret < 0)
			stat_inc(tid, STAT_UNLKERRS);
		else
			stat_add(tid, STAT_WAKEUPS, ret);
	}
}

/*
 * PI futex lock/unlock functions
 */
static void pi_mutex_lock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, 0, thread_id);
	if (val == 0)
		return;

	/*
	 * Retry if an error happens
	 */
	for (;;) {
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_lock_pi(futex, NULL, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_LOCK, &stime, &etime);
		} else {
			ret = futex_lock_pi(futex, NULL, flags);
		}
		stat_inc(tid, STAT_LOCKS);
		if (ret >= 0)
			break;
		stat_inc(tid, STAT_LOCKERRS);
	}
}

static void pi_mutex_unlock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, thread_id, 0);
	if (val == thread_id)
		return;

	if (timestat) {
		clock_gettime(CLOCK_REALTIME, &stime);
		ret = futex_unlock_pi(futex, flags);
		clock_gettime(CLOCK_REALTIME, &etime);
		systime_add(tid, TIME_UNLK, &stime, &etime);
	} else {
		ret = futex_unlock_pi(futex, flags);
	}
	if (ret < 0)
		stat_inc(tid, STAT_UNLKERRS);
	else
		stat_add(tid, STAT_WAKEUPS, ret);
	stat_inc(tid, STAT_UNLOCKS);
}

/*
 * TP futex lock/unlock functions
 */
static void tp_mutex_lock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, 0, thread_id);
	if (val == 0)
		return;

	/*
	 * Retry if an error happens
	 */
	for (;;) {
		if (timestat) {
			clock_gettime(CLOCK_REALTIME, &stime);
			ret = futex_lock(futex, NULL, flags);
			clock_gettime(CLOCK_REALTIME, &etime);
			systime_add(tid, TIME_LOCK, &stime, &etime);
		} else {
			ret = futex_lock(futex, NULL, flags);
		}
		stat_inc(tid, STAT_LOCKS);
		if (ret >= 0)
			break;
		stat_inc(tid, STAT_LOCKERRS);
	}
	/*
	 * Get # of sleeps & locking method
	 */
	stat_add(tid, STAT_SLEEPS, ret >> 16);
	ret &= 0xff;
	if (!ret)
		stat_inc(tid, STAT_STEALS);
	else if (ret == 2)
		stat_inc(tid, STAT_HANDOFFS);
}

static void tp_mutex_unlock(futex_t *futex, int tid)
{
	struct timespec stime, etime;
	futex_t val;
	int ret;

	val = futex_cmpxchg(futex, thread_id, 0);
	if (val == thread_id)
		return;

	if (timestat) {
		clock_gettime(CLOCK_REALTIME, &stime);
		ret = futex_unlock(futex, flags);
		clock_gettime(CLOCK_REALTIME, &etime);
		systime_add(tid, TIME_UNLK, &stime, &etime);
	} else {
		ret = futex_unlock(futex, flags);
	}
	stat_inc(tid, STAT_UNLOCKS);
	if (ret < 0)
		stat_inc(tid, STAT_UNLKERRS);
	else
		stat_add(tid, STAT_WAKEUPS, ret);
}

/**************************************************************************/

/*
 * Load function
 */
static inline void load(int tid)
{
	int n = loadlat;

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

static inline void csdelay(void)
{
	int n = locklat;

	while (n-- > 0)
		cpu_relax();
}

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

static void *mutex_workerfn(void *arg)
{
	long tid = (long)arg;
	struct worker *w = &worker[tid];
	lock_fn_t lock_fn = mutex_lock_fn;
	unlock_fn_t unlock_fn = mutex_unlock_fn;

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
		w->stats[STAT_OPS]++;	/* One more locking operation */
		csdelay();
	}  while (!done);

	if (verbose)
		printf("[thread %3ld (%d)] exited.\n", tid, thread_id);
	atomic_inc_return(&threads_stopping);
	return NULL;
}

static void create_threads(struct worker *w, pthread_attr_t *thread_attr,
			   void *(*workerfn)(void *arg), long tid)
{
	cpu_set_t cpu;

	/*
	 * Bind each thread to a CPU
	 */
	CPU_ZERO(&cpu);
	CPU_SET(tid % ncpus, &cpu);
	w->futex = pfutex;

	if (pthread_attr_setaffinity_np(thread_attr, sizeof(cpu_set_t), &cpu))
		err(EXIT_FAILURE, "pthread_attr_setaffinity_np");

	if (pthread_create(&w->thread, thread_attr, workerfn, (void *)tid))
		err(EXIT_FAILURE, "pthread_create");
}

static int futex_mutex_type(const char **ptype)
{
	const char *type = *ptype;

	if (!strcasecmp(type, "WW")) {
		*ptype = "WW";
		mutex_lock_fn = ww_mutex_lock;
		mutex_unlock_fn = ww_mutex_unlock;
	} else if (!strcasecmp(type, "WW2")) {
		*ptype = "WW2";
		mutex_lock_fn = ww2_mutex_lock;
		mutex_unlock_fn = ww2_mutex_unlock;
	} else if (!strcasecmp(type, "PI")) {
		*ptype = "PI";
		mutex_lock_fn = pi_mutex_lock;
		mutex_unlock_fn = pi_mutex_unlock;
	} else if (!strcasecmp(type, "TP")) {
		*ptype = "TP";
		mutex_lock_fn = tp_mutex_lock;
		mutex_unlock_fn = tp_mutex_unlock;

		/*
		 * Check if TP futex is supported.
		 */
		futex_unlock(&global_futex, 0);
		if (errno == ENOSYS) {
			fprintf(stderr,
			    "\nTP futexes are not supported by the kernel!\n");
			return -1;
		}
	} else {
		return -1;
	}
	return 0;
}

static void futex_test_driver(const char *futex_type,
			      int (*proc_type)(const char **ptype),
			      void *(*workerfn)(void *arg))
{
	u64 us;
	int i, j;
	struct worker total;
	double avg, stddev;
	pthread_attr_t thread_attr;

	/*
	 * There is an extra blank line before the error counts to highlight
	 * them.
	 */
	const char *desc[STAT_NUM] = {
		[STAT_OPS]	 = "Total exclusive locking ops",
		[STAT_LOCKS]	 = "Exclusive lock futex calls",
		[STAT_UNLOCKS]	 = "Exclusive unlock futex calls",
		[STAT_SLEEPS]	 = "Exclusive lock sleeps",
		[STAT_WAKEUPS]	 = "Process wakeups",
		[STAT_EAGAINS]	 = "EAGAIN lock errors",
		[STAT_HANDOFFS]  = "Lock handoffs",
		[STAT_STEALS]	 = "Lock stealings",
		[STAT_LOCKERRS]  = "\nExclusive lock errors",
		[STAT_UNLKERRS]  = "\nExclusive unlock errors",
	};

	if (exit_now)
		return;

	if (proc_type(&futex_type) < 0) {
		fprintf(stderr, "Unknown futex type '%s'!\n", futex_type);
		exit(1);
	}

	printf("\n=====================================\n");
	printf("[PID %d]: %d threads doing %s futex lockings (load=%d) for %d secs.\n\n",
	       getpid(), nthreads, futex_type, loadlat, nsecs);

	init_stats(&throughput_stats);

	*pfutex = 0;
	done = false;
	threads_starting = nthreads;
	pthread_attr_init(&thread_attr);

	for (i = 0; i < (int)nthreads; i++)
		create_threads(&worker[i], &thread_attr, workerfn, i);

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

	/*
	 * In verbose mode, we check if all the threads have been stopped
	 * after 1ms and report the status if some are still running.
	 */
	if (verbose) {
		usleep(1000);
		if (threads_stopping != nthreads) {
			printf("%d threads still running 1ms after timeout"
				" - futex = 0x%x\n",
				nthreads - threads_stopping, *pfutex);
			/*
			 * If the threads are still running after 10s,
			 * go directly to statistics printing and exit.
			 */
			for (i = 10; i > 0; i--) {
				sleep(1);
				if (threads_stopping == nthreads)
					break;
			}
			if (!i) {
				printf("*** Threads waiting ABORTED!! ***\n\n");
				goto print_stat;
			}
		}
	}

	for (i = 0; i < (int)nthreads; i++) {
		int ret = pthread_join(worker[i].thread, NULL);

		if (ret)
			err(EXIT_FAILURE, "pthread_join");
	}

print_stat:
	pthread_attr_destroy(&thread_attr);

	us = runtime.tv_sec * 1000000 + runtime.tv_usec;
	memset(&total, 0, sizeof(total));
	for (i = 0; i < (int)nthreads; i++) {
		/*
		 * Get a rounded estimate of the # of locking ops/sec.
		 */
		u64 tp = (u64)worker[i].stats[STAT_OPS] * 1000000 / us;

		for (j = 0; j < STAT_NUM; j++)
			total.stats[j] += worker[i].stats[j];

		update_stats(&throughput_stats, tp);
		if (verbose)
			printf("[thread %3d] futex: %p [ %'ld ops/sec ]\n",
			       i, worker[i].futex, (long)tp);
	}

	avg    = avg_stats(&throughput_stats);
	stddev = stddev_stats(&throughput_stats);

	printf("Locking statistics:\n");
	printf("%-28s = %'.2fs\n", "Test run time", (double)us/1000000);
	for (i = 0; i < STAT_NUM; i++)
		if (total.stats[i])
			printf("%-28s = %'d\n", desc[i], total.stats[i]);

	if (timestat && total.times[TIME_LOCK]) {
		printf("\nSyscall times:\n");
		if (total.stats[STAT_LOCKS])
			printf("Avg exclusive lock syscall   = %'ldns\n",
			    total.times[TIME_LOCK]/total.stats[STAT_LOCKS]);
		if (total.stats[STAT_UNLOCKS])
			printf("Avg exclusive unlock syscall = %'ldns\n",
			    total.times[TIME_UNLK]/total.stats[STAT_UNLOCKS]);
	}

	printf("\nPercentages:\n");
	if (total.stats[STAT_LOCKS])
		printf("Exclusive lock futex calls   = %.1f%%\n",
			stat_percent(&total, STAT_LOCKS, STAT_OPS));
	if (total.stats[STAT_UNLOCKS])
		printf("Exclusive unlock futex calls = %.1f%%\n",
			stat_percent(&total, STAT_UNLOCKS, STAT_OPS));
	if (total.stats[STAT_EAGAINS])
		printf("EAGAIN lock errors           = %.1f%%\n",
			stat_percent(&total, STAT_EAGAINS, STAT_LOCKS));
	if (total.stats[STAT_WAKEUPS])
		printf("Process wakeups              = %.1f%%\n",
			stat_percent(&total, STAT_WAKEUPS, STAT_UNLOCKS));

	printf("\nPer-thread Locking Rates:\n");
	printf("Avg = %'d ops/sec (+- %.2f%%)\n", (int)(avg + 0.5),
		rel_stddev_stats(stddev, avg));
	printf("Min = %'d ops/sec\n", (int)throughput_stats.min);
	printf("Max = %'d ops/sec\n", (int)throughput_stats.max);

	if (*pfutex != 0)
		printf("\nResidual futex value = 0x%x\n", *pfutex);

	/* Clear the workers area */
	memset(worker, 0, sizeof(*worker) * nthreads);
}

int bench_futex_mutex(int argc, const char **argv,
		      const char *prefix __maybe_unused)
{
	struct sigaction act;

	argc = parse_options(argc, argv, mutex_options,
			     bench_futex_mutex_usage, 0);
	if (argc)
		goto err;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	sigfillset(&act.sa_mask);
	act.sa_sigaction = toggle_done;
	sigaction(SIGINT, &act, NULL);

	if (!nthreads)
		nthreads = ncpus;

	/*
	 * Since the allocated memory buffer may not be properly cacheline
	 * aligned, we need to allocate one more than needed and manually
	 * adjust array boundary to be cacheline aligned.
	 */
	worker_alloc = calloc(nthreads + 1, sizeof(*worker));
	if (!worker_alloc)
		err(EXIT_FAILURE, "calloc");
	worker = (void *)((unsigned long)&worker_alloc[1] &
					~(CACHELINE_SIZE - 1));

	if (!fshared)
		flags = FUTEX_PRIVATE_FLAG;

	if (!ftype || !strcmp(ftype, "all")) {
		futex_test_driver("WW", futex_mutex_type, mutex_workerfn);
		futex_test_driver("PI", futex_mutex_type, mutex_workerfn);
		futex_test_driver("TP", futex_mutex_type, mutex_workerfn);
	} else {
		futex_test_driver(ftype, futex_mutex_type, mutex_workerfn);
	}
	free(worker_alloc);
	return 0;
err:
	usage_with_options(bench_futex_mutex_usage, mutex_options);
	exit(EXIT_FAILURE);
}
