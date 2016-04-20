/* -*- linux-c -*- --------------------------------------------------------- *
 *
 * linux/include/linux/devpts_fs.h
 *
 *  Copyright 1998-2004 H. Peter Anvin -- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * ------------------------------------------------------------------------- */

#ifndef _LINUX_DEVPTS_FS_H
#define _LINUX_DEVPTS_FS_H

#include <linux/errno.h>

#ifdef CONFIG_UNIX98_PTYS

struct pts_fs_info;

struct pts_fs_info *devpts_acquire(struct file *filp);
void devpts_release(struct pts_fs_info *fsi);

int devpts_new_index(struct pts_fs_info *fsi);
void devpts_kill_index(struct pts_fs_info *fsi, int idx);
/* mknod in devpts */
struct inode *devpts_pty_new(struct pts_fs_info *fsi, dev_t device, int index,
		void *priv);
/* get private structure */
void *devpts_get_priv(struct inode *pts_inode);
/* unlink */
void devpts_pty_kill(struct inode *inode);

#endif


#endif /* _LINUX_DEVPTS_FS_H */
