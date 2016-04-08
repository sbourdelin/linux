/*
 * Simple per-cpu statistics counts that have less overhead than the
 * per-cpu counters.
 */
#include <linux/percpu_stats.h>
#include <linux/bug.h>

#ifdef CONFIG_64BIT
/*
 * Ignore PCPU_STAT_64BIT & PCPU_STAT_INTSAFE flags for 64-bit architectures
 * as 64-bit count is the default.
 */
#define IS_STATS64(pcs)	false
#define GET_FLAGS(f)	((f) & ~(PCPU_STAT_64BIT | PCPU_STAT_INTSAFE))
#else
#define IS_STATS64(pcs)	((pcs)->flags & PCPU_STAT_64BIT)
#define GET_FLAGS(f)	(f)
#endif

/**
 * percpu_stats_init - allocate memory for the percpu statistics counts
 * @pcs  : Pointer to percpu_stats structure
 * @num  : Number of statistics counts to be used
 * @flags: Optional feature bits
 * Return: 0 if successful, -ENOMEM if memory allocation fails.
 */
int percpu_stats_init(struct percpu_stats *pcs, int num, int flags)
{
	int cpu, size;

	pcs->flags  = GET_FLAGS(flags);
	pcs->nstats = num;
	if (IS_STATS64(pcs)) {
		size = sizeof(uint64_t) * num;
		pcs->stats64 = __alloc_percpu(size, __alignof__(uint64_t));
		if (!pcs->stats64)
			return -ENOMEM;
		u64_stats_init(&pcs->sync);
	} else {
		size = sizeof(unsigned long) * num;
		pcs->stats = __alloc_percpu(size, __alignof__(unsigned long));
		if (!pcs->stats)
			return -ENOMEM;
	}

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(pcs->stats, cpu), 0, size);

	return 0;
}
EXPORT_SYMBOL(percpu_stats_init);

/**
 * percpu_stats_destroy - free the memory used by the statistics counts
 * @pcs: Pointer to percpu_stats structure
 */
void percpu_stats_destroy(struct percpu_stats *pcs)
{
	free_percpu(pcs->stats);
	pcs->stats  = NULL;
	pcs->nstats = 0;
}
EXPORT_SYMBOL(percpu_stats_destroy);

/**
 * percpu_stats_sum - compute the percpu sum of the given statistics count
 * @pcs  : Pointer to percpu_stats structure
 * @stat : The statistics count whose sum needs to be computed
 * Return: Sum of percpu count values
 */
uint64_t percpu_stats_sum(struct percpu_stats *pcs, int stat)
{
	int cpu;
	uint64_t sum = 0;

	BUG_ON((unsigned int)stat >= pcs->nstats);

	if (IS_STATS64(pcs)) {
		for_each_possible_cpu(cpu) {
			uint64_t val;
			unsigned int seq;

			do {
				seq = u64_stats_fetch_begin(&pcs->sync);
				val = per_cpu(pcs->stats64[stat], cpu);
			} while (u64_stats_fetch_retry(&pcs->sync, seq));
			sum += val;
		}
	} else {
		for_each_possible_cpu(cpu)
			sum += per_cpu(pcs->stats[stat], cpu);
	}
	return sum;
}
EXPORT_SYMBOL(percpu_stats_sum);

/**
 * __percpu_stats_add - add given count to percpu value
 * @pcs : Pointer to percpu_stats structure
 * @stat: The statistics count that needs to be updated
 * @cnt:  The value to be added to the statistics count
 */
void __percpu_stats_add(struct percpu_stats *pcs, int stat, int cnt)
{
	/*
	 * u64_stats_update_begin/u64_stats_update_end alone are not safe
	 * against recursive add on the same CPU caused by interrupt.
	 * So we need to set the PCPU_STAT_INTSAFE flag if this is required.
	 */
	if (IS_STATS64(pcs)) {
		uint64_t *pstats64;
		unsigned long flags;

		pstats64 = get_cpu_ptr(pcs->stats64);
		if (pcs->flags & PCPU_STAT_INTSAFE)
			local_irq_save(flags);

		u64_stats_update_begin(&pcs->sync);
		pstats64[stat] += cnt;
		u64_stats_update_end(&pcs->sync);

		if (pcs->flags & PCPU_STAT_INTSAFE)
			local_irq_restore(flags);

		put_cpu_ptr(pcs->stats64);
	}
}
EXPORT_SYMBOL(__percpu_stats_add);
