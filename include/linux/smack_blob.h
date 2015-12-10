/*
 * Copyright (C) 2015 Intel Corporation
 * Copyright (C) 2015 Casey Schaufler <casey@schaufler-ca.com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, version 2.
 *
 * Author:
 *      Casey Schaufler <casey@schaufler-ca.com>
 *
 */

#ifndef _LINUX_SMACK_BLOB_H
#define _LINUX_SMACK_BLOB_H

#include <linux/spinlock.h>

struct smack_known;

/*
 * Inode smack data
 */
struct inode_smack {
	struct smack_known	*smk_inode;	/* label of the fso */
	struct smack_known	*smk_task;	/* label of the task */
	struct smack_known	*smk_mmap;	/* label of the mmap domain */
	struct mutex		smk_lock;	/* initialization lock */
	int			smk_flags;	/* smack inode flags */
};

#endif  /* _LINUX_SMACK_BLOB_H */
