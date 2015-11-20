#ifndef _UAPI_LINUX_STAT_H
#define _UAPI_LINUX_STAT_H

#include <linux/types.h>

#if defined(__KERNEL__) || !defined(__GLIBC__) || (__GLIBC__ < 2)

#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK	 0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#endif

/*
 * Structures for the extended file attribute retrieval system call
 * (statx()).
 *
 * The caller passes a mask of what they're specifically interested in as a
 * parameter to statx().  What statx() actually got will be indicated in
 * st_mask upon return.
 *
 * For each bit in the mask argument:
 *
 * - if the datum is not available at all, the field and the bit will both be
 *   cleared;
 *
 * - otherwise, if explicitly requested:
 *
 *   - the datum will be synchronised to the server if AT_FORCE_ATTR_SYNC is
 *     set or if the datum is considered out of date, and
 *
 *   - the field will be filled in and the bit will be set;
 *
 * - otherwise, if not requested, but available in approximate form without any
 *   effort, it will be filled in anyway, and the bit will be set upon return
 *   (it might not be up to date, however, and no attempt will be made to
 *   synchronise the internal state first);
 *
 * - otherwise the field and the bit will be cleared before returning.
 *
 * Items in STATX_BASIC_STATS may be marked unavailable on return, but they
 * will have values installed for compatibility purposes so that stat() and
 * co. can be emulated in userspace.
 */
struct statx {
	/* 0x00 */
	__u32	st_mask;	/* What results were written [uncond] */
	__u32	st_information;	/* Information about the file [uncond] */
	__u16	st_mode;	/* File mode */
	__u16	__spare0[1];
	/* 0xc */
	__u32	st_nlink;	/* Number of hard links */
	__u32	st_uid;		/* User ID of owner */
	__u32	st_gid;		/* Group ID of owner */
	/* 0x18 - I/O parameters */
	__u32	st_blksize;	/* Preferred general I/O size [uncond] */
	__u32	__spare1[3];
	/* 0x28 */
	__u32	st_rdev_major;	/* Device ID of special file */
	__u32	st_rdev_minor;
	__u32	st_dev_major;	/* ID of device containing file [uncond] */
	__u32	st_dev_minor;
	/* 0x38 */
	__s32	st_atime_ns;	/* Last access time (ns part) */
	__s32	st_btime_ns;	/* File creation time (ns part) */
	__s32	st_ctime_ns;	/* Last attribute change time (ns part) */
	__s32	st_mtime_ns;	/* Last data modification time (ns part) */
	/* 0x48 */
	__s64	st_atime_s;	/* Last access time */
	__s64	st_btime_s;	/* File creation time */
	__s64	st_ctime_s;	/* Last attribute change time */
	__s64	st_mtime_s;	/* Last data modification time */
	/* 0x68 */
	__u64	st_ino;		/* Inode number */
	__u64	st_size;	/* File size */
	__u64	st_blocks;	/* Number of 512-byte blocks allocated */
	__u64	st_version;	/* Data version number */
	__u64	st_ioc_flags;	/* As FS_IOC_GETFLAGS */
	/* 0x90 */
	__u64	__spare2[14];	/* Spare space for future expansion */
	/* 0x100 */
};

/*
 * Flags to be st_mask
 *
 * Query request/result mask for statx() and struct statx::st_mask.
 *
 * These bits should be set in the mask argument of statx() to request
 * particular items when calling statx().
 */
#define STATX_MODE		0x00000001U	/* Want/got st_mode */
#define STATX_NLINK		0x00000002U	/* Want/got st_nlink */
#define STATX_UID		0x00000004U	/* Want/got st_uid */
#define STATX_GID		0x00000008U	/* Want/got st_gid */
#define STATX_RDEV		0x00000010U	/* Want/got st_rdev */
#define STATX_ATIME		0x00000020U	/* Want/got st_atime */
#define STATX_MTIME		0x00000040U	/* Want/got st_mtime */
#define STATX_CTIME		0x00000080U	/* Want/got st_ctime */
#define STATX_INO		0x00000100U	/* Want/got st_ino */
#define STATX_SIZE		0x00000200U	/* Want/got st_size */
#define STATX_BLOCKS		0x00000400U	/* Want/got st_blocks */
#define STATX_BASIC_STATS	0x000007ffU	/* The stuff in the normal stat struct */
#define STATX_BTIME		0x00000800U	/* Want/got st_btime */
#define STATX_VERSION		0x00001000U	/* Want/got st_version */
#define STATX_IOC_FLAGS		0x00002000U	/* Want/got FS_IOC_GETFLAGS */
#define STATX_ALL_STATS		0x00003fffU	/* All supported stats */

/*
 * Flags to be found in st_information
 *
 * These give information about the features or the state of a file that might
 * be of use to ordinary userspace programs such as GUIs or ls rather than
 * specialised tools.
 *
 * Additional information may be found in st_ioc_flags and we try not to
 * overlap with it.
 */
#define STATX_INFO_ENCRYPTED		0x00000001U /* File is encrypted */
#define STATX_INFO_TEMPORARY		0x00000002U /* File is temporary (NTFS/CIFS) */
#define STATX_INFO_FABRICATED		0x00000004U /* File was made up by filesystem */
#define STATX_INFO_KERNEL_API		0x00000008U /* File is kernel API (eg: procfs/sysfs) */
#define STATX_INFO_REMOTE		0x00000010U /* File is remote */
#define STATX_INFO_OFFLINE		0x00000020U /* File is offline (CIFS) */
#define STATX_INFO_AUTOMOUNT		0x00000040U /* Dir is automount trigger */
#define STATX_INFO_AUTODIR		0x00000080U /* Dir provides unlisted automounts */
#define STATX_INFO_NONSYSTEM_OWNERSHIP	0x00000100U /* File has non-system ownership details */
#define STATX_INFO_REPARSE_POINT	0x00000200U /* File is reparse point (NTFS/CIFS) */

/*
 * Information struct for fsinfo() request 0.
 */
struct fsinfo {
	/* 0x00 - General info */
	__u32	f_mask;		/* What optional fields are filled in */
	__u32	f_fstype;	/* Filesystem type from linux/magic.h [uncond] */
	__u32	f_dev_major;	/* As st_dev_* from struct statx [uncond] */
	__u32	f_dev_minor;

	/* 0x10 - statfs information */
	__u64	f_blocks;	/* Total number of blocks in fs */
	__u64	f_bfree;	/* Total number of free blocks */
	__u64	f_bavail;	/* Number of free blocks available to ordinary user */
	__u64	f_files;	/* Total number of file nodes in fs */
	__u64	f_ffree;	/* Number of free file nodes */
	__u64	f_favail;	/* Number of free file nodes available to ordinary user */
	/* 0x40 */
	__u32	f_bsize;	/* Optimal block size */
	__u16	f_frsize;	/* Fragment size */
	__u16	f_namelen;	/* Maximum name length [uncond] */
	__u64	f_flags;	/* Filesystem mount flags */
	/* 0x50 */
	__u64	f_fsid;		/* Short 64-bit Filesystem ID (as statfs) */
	__u64	f_supported_ioc_flags; /* supported FS_IOC_GETFLAGS flags  */

	/* 0x60 - File timestamp info */
	__s64	f_min_time;	/* Minimum timestamp value in seconds */
	__s64	f_max_time;	/* Maximum timestamp value in seconds */
	/* 0x70 */
	__u16	f_atime_gran_mantissa;	/* granularity(secs) = mant * 10^exp */
	__u16	f_btime_gran_mantissa;
	__u16	f_ctime_gran_mantissa;
	__u16	f_mtime_gran_mantissa;
	__s8	f_atime_gran_exponent;
	__s8	f_btime_gran_exponent;
	__s8	f_ctime_gran_exponent;
	__s8	f_mtime_gran_exponent;
	__u8	__spare6c[0x80 - 0x7c];

	/* 0x80 */
	__u8	__spare80[0xd0 - 0x80];
	/* 0xd0 */
	char	f_fs_name[15 + 1]; /* Filesystem name [uncond] */
	/* 0xe0 */
	__u8	f_volume_id[16]; /* Volume/fs identifier */
	__u8	f_volume_uuid[16]; /* Volume/fs UUID */
	/* 0x100 */
	char	f_volume_name[255 + 1]; /* Volume name */
	/* 0x200 */
	char	f_domain_name[255 + 1]; /* Domain/cell/workgroup name */
	/* 0x300 */
	__u8	__spare300[0x400 - 0x300];
	/* 0x400 */
};

/*
 * Flags to be found in f_mask.
 */
#define FSINFO_BLOCKS_INFO	0x00000001	/* Got f_blocks, f_bfree, f_bavail */
#define FSINFO_FILES_INFO	0x00000002	/* Got f_files, f_ffree, f_favail */
#define FSINFO_BSIZE		0x00000004	/* Got f_bsize */
#define FSINFO_FRSIZE		0x00000008	/* Got f_frsize */
#define FSINFO_FSID		0x00000010	/* Got f_fsid */
#define FSINFO_VOLUME_ID	0x00000020	/* Got f_volume_id */
#define FSINFO_VOLUME_UUID	0x00000040	/* Got f_volume_uuid */
#define FSINFO_VOLUME_NAME	0x00000080	/* Got f_volume_name */
#define FSINFO_DOMAIN_NAME	0x00000100	/* Got f_domain_name */

#endif /* _UAPI_LINUX_STAT_H */
