#include <linux/syscalls.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include "internal.h"

static int flags_by_mnt(int mnt_flags)
{
	int flags = 0;

	if (mnt_flags & MNT_READONLY)
		flags |= ST_RDONLY;
	if (mnt_flags & MNT_NOSUID)
		flags |= ST_NOSUID;
	if (mnt_flags & MNT_NODEV)
		flags |= ST_NODEV;
	if (mnt_flags & MNT_NOEXEC)
		flags |= ST_NOEXEC;
	if (mnt_flags & MNT_NOATIME)
		flags |= ST_NOATIME;
	if (mnt_flags & MNT_NODIRATIME)
		flags |= ST_NODIRATIME;
	if (mnt_flags & MNT_RELATIME)
		flags |= ST_RELATIME;
	return flags;
}

static int flags_by_sb(int s_flags)
{
	int flags = 0;
	if (s_flags & MS_SYNCHRONOUS)
		flags |= ST_SYNCHRONOUS;
	if (s_flags & MS_MANDLOCK)
		flags |= ST_MANDLOCK;
	return flags;
}

static int calculate_f_flags(struct vfsmount *mnt)
{
	return ST_VALID | flags_by_mnt(mnt->mnt_flags) |
		flags_by_sb(mnt->mnt_sb->s_flags);
}

static int statfs_by_dentry(struct dentry *dentry, struct kstatfs *buf)
{
	int retval;

	if (!dentry->d_sb->s_op->statfs)
		return -ENOSYS;

	memset(buf, 0, sizeof(*buf));
	retval = security_sb_statfs(dentry);
	if (retval)
		return retval;
	retval = dentry->d_sb->s_op->statfs(dentry, buf);
	if (retval == 0 && buf->f_frsize == 0)
		buf->f_frsize = buf->f_bsize;
	return retval;
}

int vfs_statfs(struct path *path, struct kstatfs *buf)
{
	int error;

	error = statfs_by_dentry(path->dentry, buf);
	if (!error)
		buf->f_flags = calculate_f_flags(path->mnt);
	return error;
}
EXPORT_SYMBOL(vfs_statfs);

int user_statfs(const char __user *pathname, struct kstatfs *st)
{
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_FOLLOW|LOOKUP_AUTOMOUNT;
retry:
	error = user_path_at(AT_FDCWD, pathname, lookup_flags, &path);
	if (!error) {
		error = vfs_statfs(&path, st);
		path_put(&path);
		if (retry_estale(error, lookup_flags)) {
			lookup_flags |= LOOKUP_REVAL;
			goto retry;
		}
	}
	return error;
}

int fd_statfs(int fd, struct kstatfs *st)
{
	struct fd f = fdget_raw(fd);
	int error = -EBADF;
	if (f.file) {
		error = vfs_statfs(&f.file->f_path, st);
		fdput(f);
	}
	return error;
}

static int do_statfs_native(struct kstatfs *st, struct statfs __user *p)
{
	struct statfs buf;

	if (sizeof(buf) == sizeof(*st))
		memcpy(&buf, st, sizeof(*st));
	else {
		if (sizeof buf.f_blocks == 4) {
			if ((st->f_blocks | st->f_bfree | st->f_bavail |
			     st->f_bsize | st->f_frsize) &
			    0xffffffff00000000ULL)
				return -EOVERFLOW;
			/*
			 * f_files and f_ffree may be -1; it's okay to stuff
			 * that into 32 bits
			 */
			if (st->f_files != -1 &&
			    (st->f_files & 0xffffffff00000000ULL))
				return -EOVERFLOW;
			if (st->f_ffree != -1 &&
			    (st->f_ffree & 0xffffffff00000000ULL))
				return -EOVERFLOW;
		}

		buf.f_type = st->f_type;
		buf.f_bsize = st->f_bsize;
		buf.f_blocks = st->f_blocks;
		buf.f_bfree = st->f_bfree;
		buf.f_bavail = st->f_bavail;
		buf.f_files = st->f_files;
		buf.f_ffree = st->f_ffree;
		buf.f_fsid = st->f_fsid;
		buf.f_namelen = st->f_namelen;
		buf.f_frsize = st->f_frsize;
		buf.f_flags = st->f_flags;
		memset(buf.f_spare, 0, sizeof(buf.f_spare));
	}
	if (copy_to_user(p, &buf, sizeof(buf)))
		return -EFAULT;
	return 0;
}

static int do_statfs64(struct kstatfs *st, struct statfs64 __user *p)
{
	struct statfs64 buf;
	if (sizeof(buf) == sizeof(*st))
		memcpy(&buf, st, sizeof(*st));
	else {
		buf.f_type = st->f_type;
		buf.f_bsize = st->f_bsize;
		buf.f_blocks = st->f_blocks;
		buf.f_bfree = st->f_bfree;
		buf.f_bavail = st->f_bavail;
		buf.f_files = st->f_files;
		buf.f_ffree = st->f_ffree;
		buf.f_fsid = st->f_fsid;
		buf.f_namelen = st->f_namelen;
		buf.f_frsize = st->f_frsize;
		buf.f_flags = st->f_flags;
		memset(buf.f_spare, 0, sizeof(buf.f_spare));
	}
	if (copy_to_user(p, &buf, sizeof(buf)))
		return -EFAULT;
	return 0;
}

SYSCALL_DEFINE2(statfs, const char __user *, pathname, struct statfs __user *, buf)
{
	struct kstatfs st;
	int error = user_statfs(pathname, &st);
	if (!error)
		error = do_statfs_native(&st, buf);
	return error;
}

SYSCALL_DEFINE3(statfs64, const char __user *, pathname, size_t, sz, struct statfs64 __user *, buf)
{
	struct kstatfs st;
	int error;
	if (sz != sizeof(*buf))
		return -EINVAL;
	error = user_statfs(pathname, &st);
	if (!error)
		error = do_statfs64(&st, buf);
	return error;
}

SYSCALL_DEFINE2(fstatfs, unsigned int, fd, struct statfs __user *, buf)
{
	struct kstatfs st;
	int error = fd_statfs(fd, &st);
	if (!error)
		error = do_statfs_native(&st, buf);
	return error;
}

SYSCALL_DEFINE3(fstatfs64, unsigned int, fd, size_t, sz, struct statfs64 __user *, buf)
{
	struct kstatfs st;
	int error;

	if (sz != sizeof(*buf))
		return -EINVAL;

	error = fd_statfs(fd, &st);
	if (!error)
		error = do_statfs64(&st, buf);
	return error;
}

int vfs_ustat(dev_t dev, struct kstatfs *sbuf)
{
	struct super_block *s = user_get_super(dev);
	int err;
	if (!s)
		return -EINVAL;

	err = statfs_by_dentry(s->s_root, sbuf);
	drop_super(s);
	return err;
}

SYSCALL_DEFINE2(ustat, unsigned, dev, struct ustat __user *, ubuf)
{
	struct ustat tmp;
	struct kstatfs sbuf;
	int err = vfs_ustat(new_decode_dev(dev), &sbuf);
	if (err)
		return err;

	memset(&tmp,0,sizeof(struct ustat));
	tmp.f_tfree = sbuf.f_bfree;
	tmp.f_tinode = sbuf.f_ffree;

	return copy_to_user(ubuf, &tmp, sizeof(struct ustat)) ? -EFAULT : 0;
}

/**
 * vfs_get_fsinfo_from_statfs - Fill in some of fsinfo from ->statfs()
 * @dentry: The filesystem to query
 * @fsinfo: The filesystem information record to fill in
 * @flags: One of AT_{NO|FORCE}_SYNC_ATTR or 0
 *
 * Fill in some of the filesystem information record from data retrieved via
 * the statfs superblock method.  This is called if there is no ->fsinfo() op
 * and may also be called by a filesystem's ->fsinfo() op.
 */
int vfs_get_fsinfo_from_statfs(struct dentry *dentry,
			       struct fsinfo *fsinfo, unsigned flags)
{
	struct kstatfs buf;
	int ret;

	ret = statfs_by_dentry(dentry, &buf);
	if (ret < 0)
		return ret;

	if (buf.f_blocks) {
		fsinfo->f_mask |= FSINFO_BLOCKS_INFO;
		fsinfo->f_blocks = buf.f_blocks;
		fsinfo->f_bfree  = buf.f_bfree;
		fsinfo->f_bavail = buf.f_bavail;
	}

	if (buf.f_files) {
		fsinfo->f_mask |= FSINFO_FILES_INFO;
		fsinfo->f_files  = buf.f_files;
		fsinfo->f_ffree  = buf.f_ffree;
		fsinfo->f_favail = buf.f_ffree;
	}

	fsinfo->f_namelen = buf.f_namelen;
	if (buf.f_bsize > 0) {
		fsinfo->f_mask |= FSINFO_BSIZE;
		fsinfo->f_bsize	= buf.f_bsize;
	}
	if (buf.f_frsize > 0) {
		fsinfo->f_frsize = buf.f_frsize;
		fsinfo->f_mask |= FSINFO_FRSIZE;
	} else if (fsinfo->f_mask & FSINFO_BSIZE) {
		fsinfo->f_frsize = fsinfo->f_bsize;
	}

	if (dentry->d_sb->s_op->statfs != simple_statfs) {
		memcpy(&fsinfo->f_fsid, &buf.f_fsid, sizeof(fsinfo->f_fsid));
		fsinfo->f_mask |= FSINFO_FSID;
	}
	return 0;
}
EXPORT_SYMBOL(vfs_get_fsinfo_from_statfs);

/*
 * Preset bits of the data to be returned with defaults.
 */
static void vfs_fsinfo_preset(struct dentry *dentry, struct fsinfo *fsinfo)
{
	struct super_block *sb = dentry->d_sb;
	/* If unset, assume 1s granularity */
	uint16_t mantissa = 1;
	uint8_t exponent = 0;
	u32 x;

	fsinfo->f_fstype = sb->s_magic;
	strcpy(fsinfo->f_fs_name, sb->s_type->name);

	fsinfo->f_min_time = S64_MIN;
	fsinfo->f_max_time = S64_MAX;
	if (sb->s_time_gran < 1000000000) {
		if (sb->s_time_gran < 1000)
			exponent = -9;
		else if (sb->s_time_gran < 1000000)
			exponent = -6;
		else
			exponent = -3;
	}
#define set_gran(x)						\
	do {							\
		fsinfo->f_##x##_mantissa = mantissa;		\
		fsinfo->f_##x##_exponent = exponent;		\
	} while (0)
	set_gran(atime_gran);
	set_gran(btime_gran);
	set_gran(ctime_gran);
	set_gran(mtime_gran);

	x  = ((u32 *)&fsinfo->f_volume_uuid)[0] = ((u32 *)&sb->s_uuid)[0];
	x |= ((u32 *)&fsinfo->f_volume_uuid)[1] = ((u32 *)&sb->s_uuid)[1];
	x |= ((u32 *)&fsinfo->f_volume_uuid)[2] = ((u32 *)&sb->s_uuid)[2];
	x |= ((u32 *)&fsinfo->f_volume_uuid)[3] = ((u32 *)&sb->s_uuid)[3];
	if (x)
		fsinfo->f_mask |= FSINFO_VOLUME_UUID;
}

/*
 * Retrieve the filesystem info.  We make some stuff up if the operation is not
 * supported.
 */
static int vfs_fsinfo(struct path *path, struct fsinfo *fsinfo, unsigned flags)
{
	struct dentry *dentry = path->dentry;
	int (*get_fsinfo)(struct dentry *, struct fsinfo *, unsigned) =
		dentry->d_sb->s_op->get_fsinfo;
	int ret;

	if (!get_fsinfo) {
		if (!dentry->d_sb->s_op->statfs)
			return -ENOSYS;
		get_fsinfo = vfs_get_fsinfo_from_statfs;
	}

	ret = security_sb_statfs(dentry);
	if (ret)
		return ret;

	vfs_fsinfo_preset(dentry, fsinfo);
	ret = get_fsinfo(dentry, fsinfo, flags);
	if (ret < 0)
		return ret;

	fsinfo->f_dev_major = MAJOR(dentry->d_sb->s_dev);
	fsinfo->f_dev_minor = MINOR(dentry->d_sb->s_dev);
	fsinfo->f_flags = calculate_f_flags(path->mnt);
	return 0;
}

static int vfs_fsinfo_path(int dfd, const char __user *filename, int flags,
			   struct fsinfo *fsinfo)
{
	struct path path;
	unsigned lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	int ret = -EINVAL;

	if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT |
		       AT_EMPTY_PATH | KSTAT_QUERY_FLAGS)) != 0)
		return -EINVAL;

	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	if (flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;

retry:
	ret = user_path_at(dfd, filename, lookup_flags, &path);
	if (ret)
		goto out;

	ret = vfs_fsinfo(&path, fsinfo, flags);
	path_put(&path);
	if (retry_estale(ret, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return ret;
}

static int vfs_fsinfo_fd(unsigned int fd, unsigned flags, struct fsinfo *fsinfo)
{
	struct fd f = fdget_raw(fd);
	int ret = -EBADF;

	if (f.file) {
		ret = vfs_fsinfo(&f.file->f_path, fsinfo, flags);
		fdput(f);
	}
	return ret;
}

/**
 * sys_fsinfo - System call to get enhanced filesystem information
 * @dfd: Base directory to pathwalk from *or* fd to stat.
 * @filename: File to stat *or* NULL.
 * @flags: AT_* flags to control pathwalk.
 * @request: Request being made.
 * @buffer: Result buffer.
 *
 * Note that if filename is NULL, then dfd is used to indicate the file of
 * interest.
 *
 * Currently, the only permitted request value is 0.
 */
SYSCALL_DEFINE5(fsinfo,
		int, dfd, const char __user *, filename, unsigned, flags,
		unsigned, request, void __user *, buffer)
{
	struct fsinfo *fsinfo;
	int ret;

	if (request != 0)
		return -EINVAL;
	if ((flags & AT_FORCE_ATTR_SYNC) && (flags & AT_NO_ATTR_SYNC))
		return -EINVAL;
	if (!access_ok(VERIFY_WRITE, buffer, sizeof(*buffer)))
		return -EFAULT;

	fsinfo = kzalloc(sizeof(struct fsinfo), GFP_KERNEL);
	if (!fsinfo)
		return -ENOMEM;

	if (filename)
		ret = vfs_fsinfo_path(dfd, filename, flags, fsinfo);
	else
		ret = vfs_fsinfo_fd(dfd, flags, fsinfo);
	if (ret)
		goto error;

	if (copy_to_user(buffer, fsinfo, sizeof(struct fsinfo)))
		ret = -EFAULT;
error:
	kfree(fsinfo);
	return ret;
}
