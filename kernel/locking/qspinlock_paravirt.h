#ifndef _GEN_PV_LOCK_SLOWPATH
#error "do not include this file"
#endif

#include <linux/hash.h>
#include <linux/bootmem.h>
#include <linux/debug_locks.h>

/*
 * Implement paravirt qspinlocks; the general idea is to halt the vcpus instead
 * of spinning them.
 *
 * This relies on the architecture to provide two paravirt hypercalls:
 *
 *   pv_wait(u8 *ptr, u8 val) -- suspends the vcpu if *ptr == val
 *   pv_kick(cpu)             -- wakes a suspended vcpu
 *
 * Using these we implement __pv_queued_spin_lock_slowpath() and
 * __pv_queued_spin_unlock() to replace native_queued_spin_lock_slowpath() and
 * native_queued_spin_unlock().
 */

#define _Q_SLOW_VAL	(3U << _Q_LOCKED_OFFSET)

/*
 * Queue node uses: vcpu_running & vcpu_halted.
 * Queue head uses: vcpu_running & vcpu_hashed.
 */
enum vcpu_state {
	vcpu_running = 0,
	vcpu_halted,		/* Used only in pv_wait_node */
	vcpu_hashed,		/* = pv_hash'ed + vcpu_halted */
};

struct pv_node {
	struct mcs_spinlock	mcs;
	struct mcs_spinlock	__res[3];

	int			cpu;
	u8			state;
};

/*
 * PV qspinlock statistics
 */
enum pv_qlock_stat {
	pvstat_wait_head,
	pvstat_wait_node,
	pvstat_wait_again,
	pvstat_kick_wait,
	pvstat_kick_unlock,
	pvstat_spurious,
	pvstat_hops,
	pvstat_num	/* Total number of statistics counts */
};

#ifdef CONFIG_QUEUED_LOCK_STAT
/*
 * Collect pvqspinlock statiatics
 */
#include <linux/debugfs.h>
#include <linux/sched.h>

static const char * const stat_fsnames[pvstat_num] = {
	[pvstat_wait_head]   = "wait_head_count",
	[pvstat_wait_node]   = "wait_node_count",
	[pvstat_wait_again]  = "wait_again_count",
	[pvstat_kick_wait]   = "kick_wait_count",
	[pvstat_kick_unlock] = "kick_unlock_count",
	[pvstat_spurious]    = "spurious_wakeup",
	[pvstat_hops]	     = "hash_hops_count",
};

static atomic_t pvstats[pvstat_num];

/*
 * pv_kick_latencies = sum of all pv_kick latencies in ns
 * pv_wake_latencies = sum of all wakeup latencies in ns
 *
 * Avg kick latency   = pv_kick_latencies/kick_unlock_count
 * Avg wake latency   = pv_wake_latencies/kick_wait_count
 * Avg # of hops/hash = hash_hops_count/kick_unlock_count
 */
static atomic64_t pv_kick_latencies, pv_wake_latencies;
static DEFINE_PER_CPU(u64, pv_kick_time);

/*
 * Reset all the statistics counts if set
 */
static bool reset_cnts __read_mostly;

/*
 * Initialize debugfs for the PV qspinlock statistics
 */
static int __init pv_qspinlock_debugfs(void)
{
	struct dentry *d_pvqlock = debugfs_create_dir("pv-qspinlock", NULL);
	int i;

	if (!d_pvqlock)
		pr_warn("Could not create 'pv-qspinlock' debugfs directory\n");

	for (i = 0; i < pvstat_num; i++)
		debugfs_create_u32(stat_fsnames[i], 0444, d_pvqlock,
				  (u32 *)&pvstats[i]);
	debugfs_create_u64("kick_latencies", 0444, d_pvqlock,
			   (u64 *)&pv_kick_latencies);
	debugfs_create_u64("wake_latencies", 0444, d_pvqlock,
			   (u64 *)&pv_wake_latencies);
	debugfs_create_bool("reset_cnts", 0644, d_pvqlock, (u32 *)&reset_cnts);
	return 0;
}
fs_initcall(pv_qspinlock_debugfs);

/*
 * Reset all the counts
 */
static noinline void pvstat_reset(void)
{
	int i;

	for (i = 0; i < pvstat_num; i++)
		atomic_set(&pvstats[i], 0);
	atomic64_set(&pv_kick_latencies, 0);
	atomic64_set(&pv_wake_latencies, 0);
	reset_cnts = 0;
}

/*
 * Increment the PV qspinlock statistics counts
 */
static inline void pvstat_inc(enum pv_qlock_stat stat, bool cond)
{
	if (cond)
		atomic_inc(&pvstats[stat]);
	if (unlikely(reset_cnts))
		pvstat_reset();
}

/*
 * PV hash hop count
 */
static inline void pvstat_hop(int hopcnt)
{
	atomic_add(hopcnt, &pvstats[pvstat_hops]);
}

/*
 * Replacement function for pv_kick()
 */
static inline void __pv_kick(int cpu)
{
	u64 start = sched_clock();

	*per_cpu_ptr(&pv_kick_time, cpu) = start;
	pv_kick(cpu);
	atomic64_add(sched_clock() - start, &pv_kick_latencies);
}

/*
 * Replacement function for pv_wait()
 */
static inline void __pv_wait(u8 *ptr, u8 val)
{
	u64 *pkick_time = this_cpu_ptr(&pv_kick_time);

	*pkick_time = 0;
	pv_wait(ptr, val);
	if (*pkick_time) {
		atomic64_add(sched_clock() - *pkick_time, &pv_wake_latencies);
		pvstat_inc(pvstat_kick_wait, true);
	}
}

#define pv_kick(c)	__pv_kick(c)
#define pv_wait(p, v)	__pv_wait(p, v)

#else /* CONFIG_QUEUED_LOCK_STAT */

static inline void pvstat_inc(enum pv_qlock_stat stat, bool cond)	{ }
static inline void pvstat_hop(int hopcnt)				{ }

#endif /* CONFIG_QUEUED_LOCK_STAT */

/*
 * Lock and MCS node addresses hash table for fast lookup
 *
 * Hashing is done on a per-cacheline basis to minimize the need to access
 * more than one cacheline.
 *
 * Dynamically allocate a hash table big enough to hold at least 4X the
 * number of possible cpus in the system. Allocation is done on page
 * granularity. So the minimum number of hash buckets should be at least
 * 256 (64-bit) or 512 (32-bit) to fully utilize a 4k page.
 *
 * Since we should not be holding locks from NMI context (very rare indeed) the
 * max load factor is 0.75, which is around the point where open addressing
 * breaks down.
 *
 */
struct pv_hash_entry {
	struct qspinlock *lock;
	struct pv_node   *node;
};

#define PV_HE_PER_LINE	(SMP_CACHE_BYTES / sizeof(struct pv_hash_entry))
#define PV_HE_MIN	(PAGE_SIZE / sizeof(struct pv_hash_entry))

static struct pv_hash_entry *pv_lock_hash;
static unsigned int pv_lock_hash_bits __read_mostly;

/*
 * Allocate memory for the PV qspinlock hash buckets
 *
 * This function should be called from the paravirt spinlock initialization
 * routine.
 */
void __init __pv_init_lock_hash(void)
{
	int pv_hash_size = ALIGN(4 * num_possible_cpus(), PV_HE_PER_LINE);

	if (pv_hash_size < PV_HE_MIN)
		pv_hash_size = PV_HE_MIN;

	/*
	 * Allocate space from bootmem which should be page-size aligned
	 * and hence cacheline aligned.
	 */
	pv_lock_hash = alloc_large_system_hash("PV qspinlock",
					       sizeof(struct pv_hash_entry),
					       pv_hash_size, 0, HASH_EARLY,
					       &pv_lock_hash_bits, NULL,
					       pv_hash_size, pv_hash_size);
}

#define for_each_hash_entry(he, offset, hash)						\
	for (hash &= ~(PV_HE_PER_LINE - 1), he = &pv_lock_hash[hash], offset = 0;	\
	     offset < (1 << pv_lock_hash_bits);						\
	     offset++, he = &pv_lock_hash[(hash + offset) & ((1 << pv_lock_hash_bits) - 1)])

static struct qspinlock **pv_hash(struct qspinlock *lock, struct pv_node *node)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	int hopcnt = 0;

	for_each_hash_entry(he, offset, hash) {
		hopcnt++;
		if (!cmpxchg(&he->lock, NULL, lock)) {
			WRITE_ONCE(he->node, node);
			pvstat_hop(hopcnt);
			return &he->lock;
		}
	}
	/*
	 * Hard assume there is a free entry for us.
	 *
	 * This is guaranteed by ensuring every blocked lock only ever consumes
	 * a single entry, and since we only have 4 nesting levels per CPU
	 * and allocated 4*nr_possible_cpus(), this must be so.
	 *
	 * The single entry is guaranteed by having the lock owner unhash
	 * before it releases.
	 */
	BUG();
}

static struct pv_node *pv_unhash(struct qspinlock *lock)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	struct pv_node *node;

	for_each_hash_entry(he, offset, hash) {
		if (READ_ONCE(he->lock) == lock) {
			node = READ_ONCE(he->node);
			WRITE_ONCE(he->lock, NULL);
			return node;
		}
	}
	/*
	 * Hard assume we'll find an entry.
	 *
	 * This guarantees a limited lookup time and is itself guaranteed by
	 * having the lock owner do the unhash -- IFF the unlock sees the
	 * SLOW flag, there MUST be a hash entry.
	 */
	BUG();
}

/*
 * Initialize the PV part of the mcs_spinlock node.
 */
static void pv_init_node(struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;

	BUILD_BUG_ON(sizeof(struct pv_node) > 5*sizeof(struct mcs_spinlock));

	pn->cpu = smp_processor_id();
	pn->state = vcpu_running;
}

/*
 * Wait for node->locked to become true, halt the vcpu after a short spin.
 * pv_kick_node() is used to set _Q_SLOW_VAL and fill in hash table on its
 * behalf.
 */
static void pv_wait_node(struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	int waitcnt = 0;
	int loop;

	/* waitcnt processing will be compiled out if !QUEUED_LOCK_STAT */
	for (;; waitcnt++) {
		for (loop = SPIN_THRESHOLD; loop; loop--) {
			if (READ_ONCE(node->locked))
				return;
			cpu_relax();
		}

		/*
		 * Order pn->state vs pn->locked thusly:
		 *
		 * [S] pn->state = vcpu_halted	  [S] next->locked = 1
		 *     MB			      MB
		 * [L] pn->locked		[RmW] pn->state = vcpu_hashed
		 *
		 * Matches the cmpxchg() from pv_kick_node().
		 */
		smp_store_mb(pn->state, vcpu_halted);

		if (!READ_ONCE(node->locked)) {
			pvstat_inc(pvstat_wait_node, true);
			pvstat_inc(pvstat_wait_again, waitcnt);
			pv_wait(&pn->state, vcpu_halted);
		}

		/*
		 * If pv_kick_node() changed us to vcpu_hashed, retain that
		 * value so that pv_wait_head() knows to not also try to hash
		 * this lock.
		 */
		cmpxchg(&pn->state, vcpu_halted, vcpu_running);

		/*
		 * If the locked flag is still not set after wakeup, it is a
		 * spurious wakeup and the vCPU should wait again. However,
		 * there is a pretty high overhead for CPU halting and kicking.
		 * So it is better to spin for a while in the hope that the
		 * MCS lock will be released soon.
		 */
		pvstat_inc(pvstat_spurious, !READ_ONCE(node->locked));
	}

	/*
	 * By now our node->locked should be 1 and our caller will not actually
	 * spin-wait for it. We do however rely on our caller to do a
	 * load-acquire for us.
	 */
}

/*
 * Called after setting next->locked = 1 when we're the lock owner.
 *
 * Instead of waking the waiters stuck in pv_wait_node() advance their state such
 * that they're waiting in pv_wait_head(), this avoids a wake/sleep cycle.
 */
static void pv_kick_node(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct __qspinlock *l = (void *)lock;

	/*
	 * If the vCPU is indeed halted, advance its state to match that of
	 * pv_wait_node(). If OTOH this fails, the vCPU was running and will
	 * observe its next->locked value and advance itself.
	 *
	 * Matches with smp_store_mb() and cmpxchg() in pv_wait_node()
	 */
	if (cmpxchg(&pn->state, vcpu_halted, vcpu_hashed) != vcpu_halted)
		return;

	/*
	 * Put the lock into the hash table and set the _Q_SLOW_VAL.
	 *
	 * As this is the same vCPU that will check the _Q_SLOW_VAL value and
	 * the hash table later on at unlock time, no atomic instruction is
	 * needed.
	 */
	WRITE_ONCE(l->locked, _Q_SLOW_VAL);
	(void)pv_hash(lock, pn);
}

/*
 * Wait for l->locked to become clear; halt the vcpu after a short spin.
 * __pv_queued_spin_unlock() will wake us.
 */
static void pv_wait_head(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct __qspinlock *l = (void *)lock;
	struct qspinlock **lp = NULL;
	int waitcnt = 0;
	int loop;

	/*
	 * If pv_kick_node() already advanced our state, we don't need to
	 * insert ourselves into the hash table anymore.
	 */
	if (READ_ONCE(pn->state) == vcpu_hashed)
		lp = (struct qspinlock **)1;

	for (;; waitcnt++) {
		for (loop = SPIN_THRESHOLD; loop; loop--) {
			if (!READ_ONCE(l->locked))
				return;
			cpu_relax();
		}

		if (!lp) { /* ONCE */
			lp = pv_hash(lock, pn);

			/*
			 * We must hash before setting _Q_SLOW_VAL, such that
			 * when we observe _Q_SLOW_VAL in __pv_queued_spin_unlock()
			 * we'll be sure to be able to observe our hash entry.
			 *
			 *   [S] <hash>                 [Rmw] l->locked == _Q_SLOW_VAL
			 *       MB                           RMB
			 * [RmW] l->locked = _Q_SLOW_VAL  [L] <unhash>
			 *
			 * Matches the smp_rmb() in __pv_queued_spin_unlock().
			 */
			if (!cmpxchg(&l->locked, _Q_LOCKED_VAL, _Q_SLOW_VAL)) {
				/*
				 * The lock is free and _Q_SLOW_VAL has never
				 * been set. Therefore we need to unhash before
				 * getting the lock.
				 */
				WRITE_ONCE(*lp, NULL);
				return;
			}
		}
		pvstat_inc(pvstat_wait_head, true);
		pvstat_inc(pvstat_wait_again, waitcnt);
		pv_wait(&l->locked, _Q_SLOW_VAL);

		if (!READ_ONCE(l->locked))
			return;
		/*
		 * The unlocker should have freed the lock before kicking the
		 * CPU. So if the lock is still not free, it is a spurious
		 * wakeup and so the vCPU should wait again after spinning for
		 * a while.
		 */
		pvstat_inc(pvstat_spurious, true);
	}

	/*
	 * Lock is unlocked now; the caller will acquire it without waiting.
	 * As with pv_wait_node() we rely on the caller to do a load-acquire
	 * for us.
	 */
}

/*
 * PV versions of the unlock fastpath and slowpath functions to be used
 * instead of queued_spin_unlock().
 */
__visible void
__pv_queued_spin_unlock_slowpath(struct qspinlock *lock, u8 locked)
{
	struct __qspinlock *l = (void *)lock;
	struct pv_node *node;

	if (unlikely(locked != _Q_SLOW_VAL)) {
		WARN(!debug_locks_silent,
		     "pvqspinlock: lock 0x%lx has corrupted value 0x%x!\n",
		     (unsigned long)lock, atomic_read(&lock->val));
		return;
	}

	/*
	 * A failed cmpxchg doesn't provide any memory-ordering guarantees,
	 * so we need a barrier to order the read of the node data in
	 * pv_unhash *after* we've read the lock being _Q_SLOW_VAL.
	 *
	 * Matches the cmpxchg() in pv_wait_head() setting _Q_SLOW_VAL.
	 */
	smp_rmb();

	/*
	 * Since the above failed to release, this must be the SLOW path.
	 * Therefore start by looking up the blocked node and unhashing it.
	 */
	node = pv_unhash(lock);

	/*
	 * Now that we have a reference to the (likely) blocked pv_node,
	 * release the lock.
	 */
	smp_store_release(&l->locked, 0);

	/*
	 * At this point the memory pointed at by lock can be freed/reused,
	 * however we can still use the pv_node to kick the CPU.
	 * The other vCPU may not really be halted, but kicking an active
	 * vCPU is harmless other than the additional latency in completing
	 * the unlock.
	 */
	pvstat_inc(pvstat_kick_unlock, true);
	pv_kick(node->cpu);
}

/*
 * Include the architecture specific callee-save thunk of the
 * __pv_queued_spin_unlock(). This thunk is put together with
 * __pv_queued_spin_unlock() to make the callee-save thunk and the real unlock
 * function close to each other sharing consecutive instruction cachelines.
 * Alternatively, architecture specific version of __pv_queued_spin_unlock()
 * can be defined.
 */
#include <asm/qspinlock_paravirt.h>

#ifndef __pv_queued_spin_unlock
__visible void __pv_queued_spin_unlock(struct qspinlock *lock)
{
	struct __qspinlock *l = (void *)lock;
	u8 locked;

	/*
	 * We must not unlock if SLOW, because in that case we must first
	 * unhash. Otherwise it would be possible to have multiple @lock
	 * entries, which would be BAD.
	 */
	locked = cmpxchg(&l->locked, _Q_LOCKED_VAL, 0);
	if (likely(locked == _Q_LOCKED_VAL))
		return;

	__pv_queued_spin_unlock_slowpath(lock, locked);
}
#endif /* __pv_queued_spin_unlock */
