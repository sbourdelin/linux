#ifndef _LINUX_FILE_TYPE_H
#define _LINUX_FILE_TYPE_H

/*
 * This is a common implementation of dirent to fs on-disk
 * file type conversion.  Although the fs on-disk bits are
 * specific to every file system, in practice, many file systems
 * use the exact same on-disk format to describe the lower 3
 * file type bits that represent the 7 POSIX file types.
 * All those file systems can use this generic code for the
 * conversions:
 *  i_mode -> fs on-disk file type (ftype)
 *  fs on-disk file type (ftype) -> dirent file type (dtype)
 *  i_mode -> dirent file type (dtype)
 */

/*
 * struct dirent file types
 * exposed to user via getdents(2), readdir(3)
 *
 * These match bits 12..15 of stat.st_mode
 * (ie "(i_mode >> 12) & 15").
 */
#define S_DT_SHIFT	12
#define S_DT(mode)	(((mode) & S_IFMT) >> S_DT_SHIFT)
#define DT_MASK		(S_IFMT >> S_DT_SHIFT)

#define DT_UNKNOWN	0
#define DT_FIFO		S_DT(S_IFIFO) /* 1 */
#define DT_CHR		S_DT(S_IFCHR) /* 2 */
#define DT_DIR		S_DT(S_IFDIR) /* 4 */
#define DT_BLK		S_DT(S_IFBLK) /* 6 */
#define DT_REG		S_DT(S_IFREG) /* 8 */
#define DT_LNK		S_DT(S_IFLNK) /* 10 */
#define DT_SOCK		S_DT(S_IFSOCK) /* 12 */
#define DT_WHT		14

#define DT_MAX		(DT_MASK + 1) /* 16 */

/*
 * fs on-disk file types.
 * Only the low 3 bits are used for the POSIX file types.
 * Other bits are reserved for fs private use.
 *
 * Note that no fs currently stores the whiteout type on-disk,
 * so whiteout dirents are exposed to user as DT_CHR.
 */
#define FT_UNKNOWN	0
#define FT_REG_FILE	1
#define FT_DIR		2
#define FT_CHRDEV	3
#define FT_BLKDEV	4
#define FT_FIFO		5
#define FT_SOCK		6
#define FT_SYMLINK	7

#define FT_MAX		8

/*
 * fs on-disk file type to dirent file type conversion
 */
static unsigned char fs_dtype_by_ftype[] = {
	DT_UNKNOWN,
	DT_REG,
	DT_DIR,
	DT_CHR,
	DT_BLK,
	DT_FIFO,
	DT_SOCK,
	DT_LNK
};

static inline unsigned char fs_dtype(int filetype)
{
	if (filetype >= FT_MAX)
		return DT_UNKNOWN;

	return fs_dtype_by_ftype[filetype];
}

/*
 * dirent file type to fs on-disk file type conversion
 * Values not initialized explicitly are FT_UNKNOWN (0).
 */
static unsigned char fs_ftype_by_dtype[DT_MAX] = {
	[DT_REG]	= FT_REG_FILE,
	[DT_DIR]	= FT_DIR,
	[DT_LNK]	= FT_SYMLINK,
	[DT_CHR]	= FT_CHRDEV,
	[DT_BLK]	= FT_BLKDEV,
	[DT_FIFO]	= FT_FIFO,
	[DT_SOCK]	= FT_SOCK,
};

/* st_mode to fs on-disk file type conversion */
static inline unsigned char fs_umode_to_ftype(umode_t mode)
{
	return fs_ftype_by_dtype[S_DT(mode)];
}

/* st_mode to dirent file type conversion */
static inline unsigned char fs_umode_to_dtype(umode_t mode)
{
	return fs_dtype(fs_umode_to_ftype(mode));
}

#endif
