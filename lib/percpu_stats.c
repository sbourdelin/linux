/*
 * Simple per-cpu statistics counts that have less overhead than the
 * per-cpu counters.
 */
#include <linux/percpu_stats.h>
#include <linux/bug.h>

/**
 * percpu_stats_init - allocate memory for the percpu statistics counts
 * @pcs: Pointer to percpu_stats structure
 * @num: Number of statistics counts to be used
 * Return: 0 if successful, -ENOMEM if memory allocation fails.
 */
int percpu_stats_init(struct percpu_stats *pcs, int num)
{
	int cpu;

	pcs->nstats = num;
	pcs->stats  = __alloc_percpu(sizeof(unsigned long) * num,
				     __alignof__(unsigned long));
	if (!pcs->stats)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		unsigned long *pstats =  per_cpu_ptr(pcs->stats, cpu);
		int stat;

		for (stat = 0; stat < pcs->nstats; stat++, pstats++)
			*pstats = 0;
	}
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

	for_each_possible_cpu(cpu)
		sum += per_cpu(pcs->stats[stat], cpu);
	return sum;
}
EXPORT_SYMBOL(percpu_stats_sum);
