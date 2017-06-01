/*
 * The lower 2 bits of the owner field in the rw_semaphore structure are
 * used for the following special purposes on a reader-owned lock:
 * 1) Bit 0 - Mark the semaphore as being owned by readers.
 * 2) Bit 1 - The optimistic spinning disable bit set by a writer to disable
 *	      spinning on a reader-owned lock after failing to acquire the
 *	      lock for a certain period of time. It will be reset only when a
 *	      new writer acquires the lock.
 *
 * A writer will clear the owner field when it unlocks. A reader, on the other
 * hand, will not touch the owner field when it unlocks.
 *
 * In essence, the owner field now has the following 3 states:
 *  1) 0
 *     - lock is free or the owner hasn't set the field yet
 *  2) RWSEM_READER_OWNED [| RWSEM_SPIN_DISABLE_BIT]
 *     - lock is currently or previously owned by readers (lock is free
 *       or not set by owner yet)
 *  3) Other non-zero value
 *     - a writer owns the lock
 */
#define RWSEM_READER_OWNED_BIT	1UL
#define RWSEM_SPIN_DISABLE_BIT	2UL
#define RWSEM_READER_OWNED	((struct task_struct *)RWSEM_READER_OWNED_BIT)

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
/*
 * All writes to owner are protected by WRITE_ONCE() to make sure that
 * store tearing can't happen as optimistic spinners may read and use
 * the owner value concurrently without lock. Read from owner, however,
 * may not need READ_ONCE() as long as the pointer value is only used
 * for comparison and isn't being dereferenced.
 */
static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, current);
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, NULL);
}

static inline bool rwsem_owner_is_reader(struct task_struct *owner)
{
	return (unsigned long)owner & RWSEM_READER_OWNED_BIT;
}

static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
	/*
	 * We check the owner value first to make sure that we will only
	 * do a write to the rwsem cacheline when it is really necessary
	 * to minimize cacheline contention.
	 */
	if (!rwsem_owner_is_reader(READ_ONCE(sem->owner)))
		WRITE_ONCE(sem->owner, RWSEM_READER_OWNED);
}

static inline bool rwsem_owner_is_writer(struct task_struct *owner)
{
	return ((unsigned long)owner & ~RWSEM_SPIN_DISABLE_BIT) &&
		!rwsem_owner_is_reader(owner);
}

static inline bool rwsem_owner_is_spin_disabled(struct task_struct *owner)
{
	return (unsigned long)owner & RWSEM_SPIN_DISABLE_BIT;
}

/*
 * Try to set an optimistic spinning disable bit while it is reader-owned.
 */
static inline void rwsem_set_spin_disable(struct rw_semaphore *sem)
{
	struct task_struct *new;

	if (READ_ONCE(sem->owner) != RWSEM_READER_OWNED)
		return;
	new = (struct task_struct *)(RWSEM_READER_OWNED_BIT|
				     RWSEM_SPIN_DISABLE_BIT);

	/*
	 * Failure in cmpxchg() will be ignored, and the caller is expected
	 * to retry later.
	 */
	(void)cmpxchg(&sem->owner, RWSEM_READER_OWNED, new);
}

/*
 * Is reader-owned rwsem optimistic spinning disabled?
 */
static inline bool rwsem_is_spin_disabled(struct rw_semaphore *sem)
{
	return rwsem_owner_is_spin_disabled(READ_ONCE(sem->owner));
}

#else
static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
}

static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
}

static inline void rwsem_set_spin_disable(struct rw_semaphore *sem)
{
}
#endif
