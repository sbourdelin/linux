/*
 * Queued read/write locks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * (C) Copyright 2013-2014 Hewlett-Packard Development Company, L.P.
 *
 * Authors: Waiman Long <waiman.long@hp.com>
 */
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <asm/qrwlock.h>

/*
 * More than one reader will be allowed to spin on the lock waiting for the
 * writer to exit. When more readers are allowed, it reduces the reader lock
 * acquisition latency, but increases the amount of cacheline contention and
 * probably power consumption.
 */
#define MAX_SPINNING_READERS	4

/*
 * This internal data structure is used for optimizing access to some of
 * the subfields within the atomic_t cnts.
 */
struct __qrwlock {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			u8 wmode;	/* Writer mode   */
			u8 rcnts[3];	/* Reader counts */
#else
			u8 rcnts[3];	/* Reader counts */
			u8 wmode;	/* Writer mode   */
#endif
		};
	};
	arch_spinlock_t	lock;
};

/**
 * queued_read_lock_slowpath - acquire read lock of a queue rwlock
 * @lock: Pointer to queue rwlock structure
 * @cnts: Current qrwlock lock value
 */
void queued_read_lock_slowpath(struct qrwlock *lock, u32 cnts)
{
	bool locked = true;

	/*
	 * Readers come here when they cannot get the lock without waiting
	 */
	if (unlikely(in_interrupt())) {
		/*
		 * Readers in interrupt context will get the lock immediately
		 * if the writer is just waiting (not holding the lock yet).
		 * The rspin_until_writer_unlock() function returns immediately
		 * in this case. Otherwise, they will spin (with ACQUIRE
		 * semantics) until the lock is available without waiting in
		 * the queue.
		 */
		while ((cnts & _QW_WMASK) == _QW_LOCKED) {
			cpu_relax_lowlatency();
			cnts = atomic_read_acquire(&lock->cnts);
		}
		return;
	}
	atomic_sub(_QR_BIAS, &lock->cnts);

	/*
	 * Put the reader into the wait queue
	 */
	arch_spin_lock(&lock->wait_lock);

	/*
	 * The ACQUIRE semantics of the following spinning code ensure
	 * that accesses can't leak upwards out of our subsequent critical
	 * section in the case that the lock is currently held for write.
	 *
	 * The reader increments the reader count & wait until the writer
	 * releases the lock.
	 */
	cnts = atomic_add_return_acquire(_QR_BIAS, &lock->cnts) - _QR_BIAS;
	while ((cnts & _QW_WMASK) == _QW_LOCKED) {
		if (locked && ((cnts >> _QR_SHIFT) < MAX_SPINNING_READERS)) {
			/*
			 * Unlock the wait queue so that more readers can
			 * come forward and waiting for the writer to exit
			 * as long as no more than MAX_SPINNING_READERS
			 * readers are present.
			 */
			arch_spin_unlock(&lock->wait_lock);
			locked = false;
		}
		cpu_relax_lowlatency();
		cnts = atomic_read_acquire(&lock->cnts);
	}

	/*
	 * Signal the next one in queue to become queue head
	 */
	if (locked)
		arch_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(queued_read_lock_slowpath);

/**
 * queued_write_lock_slowpath - acquire write lock of a queue rwlock
 * @lock : Pointer to queue rwlock structure
 */
void queued_write_lock_slowpath(struct qrwlock *lock)
{
	u32 cnts;

	/* Put the writer into the wait queue */
	arch_spin_lock(&lock->wait_lock);

	/* Try to acquire the lock directly if no reader is present */
	if (!atomic_read(&lock->cnts) &&
	    (atomic_cmpxchg_acquire(&lock->cnts, 0, _QW_LOCKED) == 0))
		goto unlock;

	/*
	 * Set the waiting flag to notify readers that a writer is pending,
	 * or wait for a previous writer to go away.
	 */
	for (;;) {
		struct __qrwlock *l = (struct __qrwlock *)lock;

		if (!READ_ONCE(l->wmode) &&
		   (cmpxchg_relaxed(&l->wmode, 0, _QW_WAITING) == 0))
			break;

		cpu_relax_lowlatency();
	}

	/* When no more readers, set the locked flag */
	for (;;) {
		cnts = atomic_read(&lock->cnts);
		if ((cnts == _QW_WAITING) &&
		    (atomic_cmpxchg_acquire(&lock->cnts, _QW_WAITING,
					    _QW_LOCKED) == _QW_WAITING))
			break;

		cpu_relax_lowlatency();
	}
unlock:
	arch_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(queued_write_lock_slowpath);
