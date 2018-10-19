#ifdef CONFIG_SMP

int __update_load_avg_blocked_se(u64 now, int cpu, struct sched_entity *se);
int __update_load_avg_se(u64 now, int cpu, struct cfs_rq *cfs_rq, struct sched_entity *se);
int __update_load_avg_cfs_rq(u64 now, int cpu, struct cfs_rq *cfs_rq);
int update_rt_rq_load_avg(u64 now, struct rq *rq, int running);
int update_dl_rq_load_avg(u64 now, struct rq *rq, int running);

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
int update_irq_load_avg(struct rq *rq, u64 running);
#else
static inline int
update_irq_load_avg(struct rq *rq, u64 running)
{
	return 0;
}
#endif

/*
 * When a task is dequeued, its estimated utilization should not be update if
 * its util_avg has not been updated at least once.
 * This flag is used to synchronize util_avg updates with util_est updates.
 * We map this information into the LSB bit of the utilization saved at
 * dequeue time (i.e. util_est.dequeued).
 */
#define UTIL_AVG_UNCHANGED 0x1

static inline void cfs_se_util_change(struct sched_avg *avg)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Avoid store if the flag has been already set */
	enqueued = avg->util_est.enqueued;
	if (!(enqueued & UTIL_AVG_UNCHANGED))
		return;

	/* Reset flag to report util_avg has been updated */
	enqueued &= ~UTIL_AVG_UNCHANGED;
	WRITE_ONCE(avg->util_est.enqueued, enqueued);
}

void update_rq_clock_pelt(struct rq *rq, s64 delta);

static inline u64 rq_clock_pelt(struct rq *rq)
{
	return rq->clock_pelt - rq->lost_idle_time;
}

#ifdef CONFIG_CFS_BANDWIDTH
/* rq->task_clock normalized against any time this cfs_rq has spent throttled */
static inline u64 cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
	if (unlikely(cfs_rq->throttle_count))
		return cfs_rq->throttled_clock_task - cfs_rq->throttled_clock_task_time;

	return rq_clock_pelt(rq_of(cfs_rq)) - cfs_rq->throttled_clock_task_time;
}
#else
static inline u64 cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
	return rq_clock_pelt(rq_of(cfs_rq));
}
#endif

#else

static inline int
update_cfs_rq_load_avg(u64 now, struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int
update_rt_rq_load_avg(u64 now, struct rq *rq, int running)
{
	return 0;
}

static inline int
update_dl_rq_load_avg(u64 now, struct rq *rq, int running)
{
	return 0;
}

static inline int
update_irq_load_avg(struct rq *rq, u64 running)
{
	return 0;
}

static inline void
update_rq_clock_pelt(struct rq *rq, s64 delta) {}

#endif


