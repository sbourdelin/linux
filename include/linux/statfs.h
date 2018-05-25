/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATFS_H
#define _LINUX_STATFS_H

#include <linux/types.h>
#include <asm/statfs.h>

struct kstatfs {
	long f_type;
	long f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	__kernel_fsid_t f_fsid;
	long f_namelen;
	long f_frsize;
	long f_flags;
	long f_spare[4];
};

/*
 * Definitions for the flag in f_flag.
 *
 * Generally these flags are equivalent to the MS_ flags used in the mount
 * ABI.  The exception is ST_VALID which has the same value as MS_REMOUNT
 * which doesn't make any sense for statfs.
 */
#define ST_RDONLY	(1<<0) /* mount read-only */
#define ST_NOSUID	(1<<1) /* ignore suid and sgid bits */
#define ST_NODEV	(1<<2) /* disallow access to device special files */
#define ST_NOEXEC	(1<<3) /* disallow program execution */
#define ST_SYNCHRONOUS	(1<<4) /* writes are synced at once */
#define ST_VALID	(1<<5) /* f_flags support is implemented */
#define ST_MANDLOCK	(1<<6) /* allow mandatory locks on an FS */
/* (1<<7) used for ST_WRITE in glibc */
/* (1<<8) used for ST_APPEND in glibc */
/* (1<<9) used for ST_IMMUTABLE in glibc */
#define ST_NOATIME	(1<<10) /* do not update access times */
#define ST_NODIRATIME	(1<<11) /* do not update directory access times */
#define ST_RELATIME	(1<<12) /* update atime relative to mtime/ctime */
#define ST_UNBINDABLE	(1<<17)	/* change to unbindable */
#define ST_SLAVE	(1<<19)	/* change to slave */
#define ST_SHARED	(1<<20)	/* change to shared */

#endif
