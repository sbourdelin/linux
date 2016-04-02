#ifndef _LINUX_PERCPU_STATS_H
#define _LINUX_PERCPU_STATS_H
/*
 * Simple per-cpu statistics counts that have less overhead than the
 * per-cpu counters.
 */
#include <linux/percpu.h>
#include <linux/types.h>

struct percpu_stats {
	unsigned long __percpu *stats;
	int nstats;	/* Number of statistics counts in stats array */
};

/*
 * Reset the all statistics counts to 0 in the percpu_stats structure
 */
static inline void percpu_stats_reset(struct percpu_stats *pcs)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		unsigned long *pstats =  per_cpu_ptr(pcs->stats, cpu);
		int stat;

		for (stat = 0; stat < pcs->nstats; stat++, pstats++)
			*pstats = 0;
	}

	/*
	 * If a statistics count is in the middle of being updated, it
	 * is possible that the above clearing may not work. So we need
	 * to double check again to make sure that the counters are really
	 * cleared. Still there is a still a very small chance that the
	 * second clearing does not work.
	 */
	for_each_possible_cpu(cpu) {
		unsigned long *pstats =  per_cpu_ptr(pcs->stats, cpu);
		int stat;

		for (stat = 0; stat < pcs->nstats; stat++, pstats++)
			if (*pstats)
				*pstats = 0;
	}
}

static inline int percpu_stats_init(struct percpu_stats *pcs, int num)
{
	pcs->nstats = num;
	pcs->stats  = __alloc_percpu(sizeof(unsigned long) * num,
				     __alignof__(unsigned long));
	if (!pcs->stats)
		return -ENOMEM;

	percpu_stats_reset(pcs);
	return 0;
}

static inline void percpu_stats_destroy(struct percpu_stats *pcs)
{
	free_percpu(pcs->stats);
	pcs->stats  = NULL;
	pcs->nstats = 0;
}

static inline void
__percpu_stats_add(struct percpu_stats *pcs, int stat, int cnt)
{
	unsigned long *pstat;

	if ((unsigned int)stat >= pcs->nstats)
		return;
	preempt_disable();
	pstat = this_cpu_ptr(&pcs->stats[stat]);
	*pstat += cnt;
	preempt_enable();
}

static inline void percpu_stats_inc(struct percpu_stats *pcs, int stat)
{
	__percpu_stats_add(pcs, stat, 1);
}

static inline void percpu_stats_dec(struct percpu_stats *pcs, int stat)
{
	__percpu_stats_add(pcs, stat, -1);
}

static inline unsigned long
percpu_stats_sum(struct percpu_stats *pcs, int stat)
{
	int cpu;
	unsigned long sum = 0;

	if ((unsigned int)stat >= pcs->nstats)
		return sum;

	for_each_possible_cpu(cpu)
		sum += per_cpu(pcs->stats[stat], cpu);
	return sum;
}

#endif /* _LINUX_PERCPU_STATS_H */
