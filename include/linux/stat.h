#ifndef _LINUX_STAT_H
#define _LINUX_STAT_H


#include <asm/stat.h>
#include <uapi/linux/stat.h>

/*
 * Human readable symbolic definitions for common
 * file permissions:
 */
#define PERM_r________	0400
#define PERM_r__r_____	0440
#define PERM_r__r__r__	0444

#define PERM_rw_______	0600
#define PERM_rw_r_____	0640
#define PERM_rw_r__r__	0644
#define PERM_rw_rw_r__	0664
#define PERM_rw_rw_rw_	0666

#define PERM__w_______	0200
#define PERM__w__w____	0220
#define PERM__w__w__w_	0222

#define PERM_r_x______	0500
#define PERM_r_xr_x___	0550
#define PERM_r_xr_xr_x	0555

#define PERM_rwx______	0700
#define PERM_rwxr_x___	0750
#define PERM_rwxr_xr_x	0755
#define PERM_rwxrwxr_x	0775
#define PERM_rwxrwxrwx	0777

#define PERM__wx______	0300
#define PERM__wx_wx___	0330
#define PERM__wx_wx_wx	0333

#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

#define UTIME_NOW	((1l << 30) - 1l)
#define UTIME_OMIT	((1l << 30) - 2l)

#include <linux/types.h>
#include <linux/time.h>
#include <linux/uidgid.h>

struct kstat {
	u64		ino;
	dev_t		dev;
	umode_t		mode;
	unsigned int	nlink;
	kuid_t		uid;
	kgid_t		gid;
	dev_t		rdev;
	loff_t		size;
	struct timespec  atime;
	struct timespec	mtime;
	struct timespec	ctime;
	unsigned long	blksize;
	unsigned long long	blocks;
};

#endif
