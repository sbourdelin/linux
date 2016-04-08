#ifndef _LINUX_PERCPU_STATS_H
#define _LINUX_PERCPU_STATS_H
/*
 * Simple per-cpu statistics counts that have less overhead than the
 * per-cpu counters.
 */
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/u64_stats_sync.h>

/*
 * Supported flags for percpu_stats_init()
 */
#define PCPU_STAT_64BIT		1	/* Use 64-bit statistics count	  */
#define PCPU_STAT_INTSAFE	2	/* Make percpu_add interrupt safe */

struct percpu_stats {
	union {
		unsigned long __percpu	*stats;
		uint64_t __percpu	*stats64;
	};
	struct u64_stats_sync sync;
	int nstats;	/* Number of statistics counts in stats array */
	int flags;
};

extern void percpu_stats_destroy(struct percpu_stats *pcs);
extern int  percpu_stats_init(struct percpu_stats *pcs, int num, int flags);
extern uint64_t percpu_stats_sum(struct percpu_stats *pcs, int stat);
extern void __percpu_stats_add(struct percpu_stats *pcs, int stat, int cnt);

#ifdef CONFIG_64BIT
#define PERCPU_STATS_FLAGS(pcs)	false
#else
#define PERCPU_STATS_FLAGS(pcs)	((pcs)->flags)
#endif

/**
 * percpu_stats_add - Add the given value to a statistics count
 * @pcs:  Pointer to percpu_stats structure
 * @stat: The statistics count that needs to be updated
 * @cnt:  The value to be added to the statistics count
 */
static inline void
percpu_stats_add(struct percpu_stats *pcs, int stat, int cnt)
{
	BUG_ON((unsigned int)stat >= pcs->nstats);
	if (unlikely(PERCPU_STATS_FLAGS(pcs)))
		__percpu_stats_add(pcs, stat, cnt);
	else
		this_cpu_add(pcs->stats[stat], cnt);
}

static inline void percpu_stats_inc(struct percpu_stats *pcs, int stat)
{
	percpu_stats_add(pcs, stat, 1);
}

static inline void percpu_stats_dec(struct percpu_stats *pcs, int stat)
{
	percpu_stats_add(pcs, stat, -1);
}

#endif /* _LINUX_PERCPU_STATS_H */
