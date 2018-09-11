/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2017, 2018 Oracle and/or its affiliates. All rights reserved.
 *
 * Authors: Yosef Lev <levyossi@icloud.com>
 *          Daniel Jordan <daniel.m.jordan@oracle.com>
 */

#include <linux/list.h>
#include <linux/prefetch.h>

/*
 * smp_list_del is a variant of list_del that allows concurrent list removals
 * under certain assumptions.  The idea is to get away from overly coarse
 * synchronization, such as using a lock to guard an entire list, which
 * serializes all operations even though those operations might be happening on
 * disjoint parts.
 *
 * If you want to use other functions from the list API concurrently,
 * additional synchronization may be necessary.  For example, you could use a
 * rwlock as a two-mode lock, where readers use the lock in shared mode and are
 * allowed to call smp_list_del concurrently, and writers use the lock in
 * exclusive mode and are allowed to use all list operations.
 */

/**
 * smp_list_del - concurrent variant of list_del
 * @entry: entry to delete from the list
 *
 * Safely removes an entry from the list in the presence of other threads that
 * may try to remove adjacent entries.  Uses the entry's next field and the
 * predecessor entry's next field as locks to accomplish this.
 *
 * Assumes that no two threads may try to delete the same entry.  This
 * assumption holds, for example, if the objects on the list are
 * reference-counted so that an object is only removed when its refcount falls
 * to 0.
 *
 * @entry's next and prev fields are poisoned on return just as with list_del.
 */
void smp_list_del(struct list_head *entry)
{
	struct list_head *succ, *pred, *pred_reread;

	/*
	 * The predecessor entry's cacheline is read before it's written, so to
	 * avoid an unnecessary cacheline state transition, prefetch for
	 * writing.  In the common case, the predecessor won't change.
	 */
	prefetchw(entry->prev);

	/*
	 * Step 1: Lock @entry E by making its next field point to its
	 * predecessor D.  This prevents any thread from removing the
	 * predecessor because that thread will loop in its step 4 while
	 * E->next == D.  This also prevents any thread from removing the
	 * successor F because that thread will see that F->prev->next != F in
	 * the cmpxchg in its step 3.  Retry if the successor is being removed
	 * and has already set this field to NULL in step 3.
	 */
	succ = READ_ONCE(entry->next);
	pred = READ_ONCE(entry->prev);
	while (succ == NULL || cmpxchg(&entry->next, succ, pred) != succ) {
		/*
		 * Reread @entry's successor because it may change until
		 * @entry's next field is locked.  Reread the predecessor to
		 * have a better chance of publishing the right value and avoid
		 * entering the loop in step 2 while @entry is locked,
		 * but this isn't required for correctness because the
		 * predecessor is reread in step 2.
		 */
		cpu_relax();
		succ = READ_ONCE(entry->next);
		pred = READ_ONCE(entry->prev);
	}

	/*
	 * Step 2: A racing thread may remove @entry's predecessor.  Reread and
	 * republish @entry->prev until it does not change.  This guarantees
	 * that the racing thread has not passed the while loop in step 4 and
	 * has not freed the predecessor, so it is safe for this thread to
	 * access predecessor fields in step 3.
	 */
	pred_reread = READ_ONCE(entry->prev);
	while (pred != pred_reread) {
		WRITE_ONCE(entry->next, pred_reread);
		pred = pred_reread;
		/*
		 * Ensure the predecessor is published in @entry's next field
		 * before rereading the predecessor.  Pairs with the smp_mb in
		 * step 4.
		 */
		smp_mb();
		pred_reread = READ_ONCE(entry->prev);
	}

	/*
	 * Step 3: If the predecessor points to @entry, lock it and continue.
	 * Otherwise, the predecessor is being removed, so loop until that
	 * removal finishes and this thread's @entry->prev is updated, which
	 * indicates the old predecessor has reached the loop in step 4.  Write
	 * the new predecessor into @entry->next.  This both releases the old
	 * predecessor from its step 4 loop and sets this thread up to lock the
	 * new predecessor.
	 */
	while (pred->next != entry ||
	       cmpxchg(&pred->next, entry, NULL) != entry) {
		/*
		 * The predecessor is being removed so wait for a new,
		 * unlocked predecessor.
		 */
		cpu_relax();
		pred_reread = READ_ONCE(entry->prev);
		if (pred != pred_reread) {
			/*
			 * The predecessor changed, so republish it and update
			 * it as in step 2.
			 */
			WRITE_ONCE(entry->next, pred_reread);
			pred = pred_reread;
			/* Pairs with smp_mb in step 4. */
			smp_mb();
		}
	}

	/*
	 * Step 4: @entry and @entry's predecessor are both locked, so now
	 * actually remove @entry from the list.
	 *
	 * It is safe to write to the successor's prev pointer because step 1
	 * prevents the successor from being removed.
	 */

	WRITE_ONCE(succ->prev, pred);

	/*
	 * The full barrier guarantees that all changes are visible to other
	 * threads before the entry is unlocked by the final write, pairing
	 * with the implied full barrier before the cmpxchg in step 1.
	 *
	 * The barrier also guarantees that this thread writes succ->prev
	 * before reading succ->next, pairing with a thread in step 2 or 3 that
	 * writes entry->next before reading entry->prev, which ensures that
	 * the one that writes second sees the update from the other.
	 */
	smp_mb();

	while (READ_ONCE(succ->next) == entry) {
		/* The successor is being removed, so wait for it to finish. */
		cpu_relax();
	}

	/* Simultaneously completes the removal and unlocks the predecessor. */
	WRITE_ONCE(pred->next, succ);

	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}
