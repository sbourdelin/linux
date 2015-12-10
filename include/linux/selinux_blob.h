/*
 *  NSA Security-Enhanced Linux (SELinux) security module
 *
 *  This file contains the SELinux security data structures for kernel objects
 *  that are exposed outside the module.
 *
 *  Author(s):  Stephen Smalley, <sds@epoch.ncsc.mil>
 *		Chris Vance, <cvance@nai.com>
 *		Wayne Salamon, <wsalamon@nai.com>
 *		James Morris <jmorris@redhat.com>
 *		Casey Schaufler <casey@schaufler-ca.com>
 *
 *  Copyright (C) 2001,2002 Networks Associates Technology, Inc.
 *  Copyright (C) 2003 Red Hat, Inc., James Morris <jmorris@redhat.com>
 *  Copyright (C) 2015 Intel Corporation.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2,
 *	as published by the Free Software Foundation.
 */
#ifndef _LINUX_SELINUX_BLOB_H_
#define _LINUX_SELINUX_BLOB_H_

#include <linux/list.h>
#include <linux/fs.h>
#include <linux/spinlock.h>

struct inode_selinux {
	struct inode *inode;	/* back pointer to inode object */
	union {
		struct list_head list;	/* list of inode_security_struct */
		struct rcu_head rcu;	/* for freeing inode_selinux_struct */
	};
	u32 task_sid;		/* SID of creating task */
	u32 sid;		/* SID of this object */
	u16 sclass;		/* security class of this object */
	unsigned char initialized;	/* initialization flag */
	struct mutex lock;
};

#endif /* _LINUX_SELINUX_BLOB_H_ */
