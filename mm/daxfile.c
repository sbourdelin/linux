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
#include <linux/dax.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>

/*
 * TODO: a list to lookup daxfiles assumes a low number of instances,
 * revisit.
 */
static LIST_HEAD(daxfiles);
static DEFINE_SPINLOCK(dax_lock);

struct dax_info {
	struct list_head list;
	struct file *daxfile;
};

static int daxfile_disable(struct file *victim)
{
	int found = 0;
	struct dax_info *d;
	struct inode *inode;
	struct file *daxfile;
	struct address_space *mapping;

	mapping = victim->f_mapping;
	spin_lock(&dax_lock);
	list_for_each_entry(d, &daxfiles, list)
		if (d->daxfile->f_mapping == mapping) {
			list_del(&d->list);
			found = 1;
			break;
		}
	spin_unlock(&dax_lock);

	if (!found)
		return -EINVAL;

	daxfile = d->daxfile;

	inode = mapping->host;
	inode->i_flags &= ~(S_SWAPFILE | S_DAXFILE);
	filp_close(daxfile, NULL);

	return 0;
}

static int claim_daxfile_checks(struct inode *inode)
{
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (!IS_DAX(inode))
		return -EINVAL;

	if (IS_SWAPFILE(inode) || IS_DAXFILE(inode))
		return -EBUSY;

	return 0;
}

int daxfile_enable(struct file *daxfile, int align)
{
	struct address_space *mapping;
	struct inode *inode;
	struct dax_info *d;
	int rc;

	if (align < 0)
		return -EINVAL;

	mapping = daxfile->f_mapping;
	inode = mapping->host;

	rc = claim_daxfile_checks(inode);
	if (rc)
		return rc;

	rc = daxfile_activate(daxfile, align);
	if (rc)
		return rc;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	INIT_LIST_HEAD(&d->list);
	d->daxfile = daxfile;

	spin_lock(&dax_lock);
	list_add(&d->list, &daxfiles);
	spin_unlock(&dax_lock);

	/*
	 * We set S_SWAPFILE to gain "no truncate" / static block
	 * allocation semantics, and S_DAXFILE so we can differentiate
	 * traditional swapfiles and assume static block mappings in the
	 * dax mmap path.
	 */
	inode->i_flags |= S_SWAPFILE | S_DAXFILE;
	return 0;
}

SYSCALL_DEFINE3(daxctl, const char __user *, path, int, flags, int, align)
{
	int rc;
	struct filename *name;
	struct inode *inode = NULL;
	struct file *daxfile = NULL;
	struct address_space *mapping;

	if (flags & ~DAXCTL_VALID_FLAGS)
		return -EINVAL;

	name = getname(path);
	if (IS_ERR(name))
		return PTR_ERR(name);

	daxfile = file_open_name(name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(daxfile)) {
		rc = PTR_ERR(daxfile);
		daxfile = NULL;
		goto out;
	}

	mapping = daxfile->f_mapping;
	inode = mapping->host;
	if (flags & DAXCTL_F_GET) {
		/*
		 * We only report the state of DAXCTL_F_STATIC since
		 * there is no actions for applications to take based on
		 * the setting of S_DAX. However, if this interface is
		 * used for toggling S_DAX presumably userspace would
		 * want to know the state of the flag.
		 *
		 * TODO: revisit whether we want to report DAXCTL_F_DAX
		 * in the IS_DAX() case.
		 */
		if (IS_DAXFILE(inode))
			rc = DAXCTL_F_STATIC;
		else
			rc = 0;

		goto out;
	}

	/*
	 * TODO: Should unprivileged users be allowed to control daxfile
	 * behavior? Perhaps a mount flag... is -o dax that flag?
	 */
	if (!capable(CAP_LINUX_IMMUTABLE)) {
		rc = -EPERM;
		goto out;
	}

	inode_lock(inode);
	if (!IS_DAXFILE(inode) && (flags & DAXCTL_F_STATIC)) {
		rc = daxfile_enable(daxfile, align);
		/* if successfully enabled hold daxfile open */
		if (rc == 0)
			daxfile = NULL;
	} else if (IS_DAXFILE(inode) && !(flags & DAXCTL_F_STATIC))
		rc = daxfile_disable(daxfile);
	else
		rc = 0;
	inode_unlock(inode);

out:
	if (daxfile)
		filp_close(daxfile, NULL);
	if (name)
		putname(name);
	return rc;
}
