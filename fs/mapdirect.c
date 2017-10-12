/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/mapdirect.h>
#include <linux/workqueue.h>
#include <linux/signal.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm.h>

#define MAPDIRECT_BREAK 0
#define MAPDIRECT_VALID 1

struct map_direct_state {
	atomic_t mds_ref;
	atomic_t mds_vmaref;
	unsigned long mds_state;
	struct inode *mds_inode;
	struct delayed_work mds_work;
	struct fasync_struct *mds_fa;
	struct vm_area_struct *mds_vma;
};

bool test_map_direct_valid(struct map_direct_state *mds)
{
	return test_bit(MAPDIRECT_VALID, &mds->mds_state);
}
EXPORT_SYMBOL_GPL(test_map_direct_valid);

static void put_map_direct(struct map_direct_state *mds)
{
	if (!atomic_dec_and_test(&mds->mds_ref))
		return;
	kfree(mds);
}

static void put_map_direct_vma(struct map_direct_state *mds)
{
	struct vm_area_struct *vma = mds->mds_vma;
	struct file *file = vma->vm_file;
	struct inode *inode = file_inode(file);
	void *owner = mds;

	if (!atomic_dec_and_test(&mds->mds_vmaref))
		return;

	/*
	 * Flush in-flight+forced lm_break events that may be
	 * referencing this dying vma.
	 */
	mds->mds_vma = NULL;
	set_bit(MAPDIRECT_BREAK, &mds->mds_state);
	vfs_setlease(vma->vm_file, F_UNLCK, NULL, &owner);
	flush_delayed_work(&mds->mds_work);
	iput(inode);

	put_map_direct(mds);
}

void generic_map_direct_close(struct vm_area_struct *vma)
{
	put_map_direct_vma(vma->vm_private_data);
}
EXPORT_SYMBOL_GPL(generic_map_direct_close);

static void get_map_direct_vma(struct map_direct_state *mds)
{
	atomic_inc(&mds->mds_vmaref);
}

void generic_map_direct_open(struct vm_area_struct *vma)
{
	get_map_direct_vma(vma->vm_private_data);
}
EXPORT_SYMBOL_GPL(generic_map_direct_open);

static void map_direct_invalidate(struct work_struct *work)
{
	struct map_direct_state *mds;
	struct vm_area_struct *vma;
	struct inode *inode;
	void *owner;

	mds = container_of(work, typeof(*mds), mds_work.work);

	clear_bit(MAPDIRECT_VALID, &mds->mds_state);

	vma = ACCESS_ONCE(mds->mds_vma);
	inode = mds->mds_inode;
	if (vma) {
		unsigned long len = vma->vm_end - vma->vm_start;
		loff_t start = (loff_t) vma->vm_pgoff * PAGE_SIZE;

		unmap_mapping_range(inode->i_mapping, start, len, 1);
	}
	owner = mds;
	vfs_setlease(vma->vm_file, F_UNLCK, NULL, &owner);

	put_map_direct(mds);
}

static bool map_direct_lm_break(struct file_lock *fl)
{
	struct map_direct_state *mds = fl->fl_owner;

	/*
	 * Given that we need to take sleeping locks to invalidate the
	 * mapping we schedule that work with the original timeout set
	 * by the file-locks core. Then we tell the core to hold off on
	 * continuing with the lease break until the delayed work
	 * completes the invalidation and the lease unlock.
	 *
	 * Note that this assumes that i_mapdcount is protecting against
	 * block-map modifying write-faults since we are unable to use
	 * leases in that path due to locking constraints.
	 */
	if (!test_and_set_bit(MAPDIRECT_BREAK, &mds->mds_state)) {
		schedule_delayed_work(&mds->mds_work, lease_break_time * HZ);
		kill_fasync(&fl->fl_fasync, SIGIO, POLL_MSG);
	}

	/* Tell the core lease code to wait for delayed work completion */
	fl->fl_break_time = 0;

	return false;
}

static int map_direct_lm_change(struct file_lock *fl, int arg,
		struct list_head *dispose)
{
	WARN_ON(!(arg & F_UNLCK));

	return lease_modify(fl, arg, dispose);
}

static void map_direct_lm_setup(struct file_lock *fl, void **priv)
{
	struct file *file = fl->fl_file;
	struct map_direct_state *mds = *priv;
	struct fasync_struct *fa = mds->mds_fa;

	/*
	 * Comment copied from lease_setup():
	 * fasync_insert_entry() returns the old entry if any. If there was no
	 * old entry, then it used "priv" and inserted it into the fasync list.
	 * Clear the pointer to indicate that it shouldn't be freed.
	 */
	if (!fasync_insert_entry(fa->fa_fd, file, &fl->fl_fasync, fa))
		*priv = NULL;

	__f_setown(file, task_pid(current), PIDTYPE_PID, 0);
}

static const struct lock_manager_operations map_direct_lm_ops = {
	.lm_break = map_direct_lm_break,
	.lm_change = map_direct_lm_change,
	.lm_setup = map_direct_lm_setup,
};

struct map_direct_state *map_direct_register(int fd, struct vm_area_struct *vma)
{
	struct map_direct_state *mds = kzalloc(sizeof(*mds), GFP_KERNEL);
	struct file *file = vma->vm_file;
	struct inode *inode = file_inode(file);
	struct fasync_struct *fa;
	struct file_lock *fl;
	void *owner = mds;
	int rc = -ENOMEM;

	if (!mds)
		return ERR_PTR(-ENOMEM);

	mds->mds_vma = vma;
	atomic_set(&mds->mds_ref, 1);
	atomic_set(&mds->mds_vmaref, 1);
	set_bit(MAPDIRECT_VALID, &mds->mds_state);
	mds->mds_inode = inode;
	ihold(inode);
	INIT_DELAYED_WORK(&mds->mds_work, map_direct_invalidate);

	fa = fasync_alloc();
	if (!fa)
		goto err_fasync_alloc;
	mds->mds_fa = fa;
	fa->fa_fd = fd;

	fl = locks_alloc_lock();
	if (!fl)
		goto err_lock_alloc;

	locks_init_lock(fl);
	fl->fl_lmops = &map_direct_lm_ops;
	fl->fl_flags = FL_LAYOUT;
	fl->fl_type = F_RDLCK;
	fl->fl_end = OFFSET_MAX;
	fl->fl_owner = mds;
	atomic_inc(&mds->mds_ref);
	fl->fl_pid = current->tgid;
	fl->fl_file = file;

	rc = vfs_setlease(file, fl->fl_type, &fl, &owner);
	if (rc)
		goto err_setlease;
	if (fl) {
		WARN_ON(1);
		owner = mds;
		vfs_setlease(file, F_UNLCK, NULL, &owner);
		owner = NULL;
		rc = -ENXIO;
		goto err_setlease;
	}

	return mds;

err_setlease:
	locks_free_lock(fl);
err_lock_alloc:
	/* if owner is NULL then the lease machinery is reponsible @fa */
	if (owner)
		fasync_free(fa);
err_fasync_alloc:
	iput(inode);
	kfree(mds);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL_GPL(map_direct_register);
