#include <linux/cred.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/parser.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/uidgid.h>
#include <linux/xattr.h>

struct shiftfs_super_info {
	struct vfsmount *mnt;
	struct uid_gid_map uid_map, gid_map;
};

static struct inode *shiftfs_new_inode(struct super_block *sb, umode_t mode,
				       struct dentry *dentry);

enum {
	OPT_UIDMAP,
	OPT_GIDMAP,
	OPT_LAST,
};

/* global filesystem options */
static const match_table_t tokens = {
	{ OPT_UIDMAP, "uidmap=%u:%u:%u" },
	{ OPT_GIDMAP, "gidmap=%u:%u:%u" },
	{ OPT_LAST, NULL }
};

/*
 * code stolen from user_namespace.c ... except that these functions
 * return the same id back if unmapped ... should probably have a
 * library?
 */
static u32 map_id_down(struct uid_gid_map *map, u32 id)
{
	unsigned idx, extents;
	u32 first, last;

	/* Find the matching extent */
	extents = map->nr_extents;
	smp_rmb();
	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last)
			break;
	}
	/* Map the id or note failure */
	if (idx < extents)
		id = (id - first) + map->extent[idx].lower_first;

	return id;
}

static u32 map_id_up(struct uid_gid_map *map, u32 id)
{
	unsigned idx, extents;
	u32 first, last;

	/* Find the matching extent */
	extents = map->nr_extents;
	smp_rmb();
	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].lower_first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last)
			break;
	}
	/* Map the id or note failure */
	if (idx < extents)
		id = (id - first) + map->extent[idx].first;

	return id;
}

static bool mappings_overlap(struct uid_gid_map *new_map,
			     struct uid_gid_extent *extent)
{
	u32 upper_first, lower_first, upper_last, lower_last;
	unsigned idx;

	upper_first = extent->first;
	lower_first = extent->lower_first;
	upper_last = upper_first + extent->count - 1;
	lower_last = lower_first + extent->count - 1;

	for (idx = 0; idx < new_map->nr_extents; idx++) {
		u32 prev_upper_first, prev_lower_first;
		u32 prev_upper_last, prev_lower_last;
		struct uid_gid_extent *prev;

		prev = &new_map->extent[idx];

		prev_upper_first = prev->first;
		prev_lower_first = prev->lower_first;
		prev_upper_last = prev_upper_first + prev->count - 1;
		prev_lower_last = prev_lower_first + prev->count - 1;

		/* Does the upper range intersect a previous extent? */
		if ((prev_upper_first <= upper_last) &&
		    (prev_upper_last >= upper_first))
			return true;

		/* Does the lower range intersect a previous extent? */
		if ((prev_lower_first <= lower_last) &&
		    (prev_lower_last >= lower_first))
			return true;
	}
	return false;
}
/* end code stolen from user_namespace.c */

static const struct cred *shiftfs_get_up_creds(struct super_block *sb)
{
	struct cred *cred = prepare_creds();
	struct shiftfs_super_info *ssi = sb->s_fs_info;

	if (!cred)
		return NULL;

	cred->fsuid = KUIDT_INIT(map_id_up(&ssi->uid_map, __kuid_val(cred->fsuid)));
	cred->fsgid = KGIDT_INIT(map_id_up(&ssi->gid_map, __kgid_val(cred->fsgid)));

	return cred;
}

static const struct cred *shiftfs_new_creds(const struct cred **newcred,
					    struct super_block *sb)
{
	const struct cred *cred = shiftfs_get_up_creds(sb);

	*newcred = cred;

	if (cred)
		cred = override_creds(cred);
	else
		printk(KERN_ERR "Credential override failed: no memory\n");

	return cred;
}

static void shiftfs_old_creds(const struct cred *oldcred,
			      const struct cred **newcred)
{
	if (!*newcred)
		return;

	revert_creds(oldcred);
	put_cred(*newcred);
}

static int shiftfs_parse_options(struct shiftfs_super_info *ssi, char *options)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int from, to, count;
	struct uid_gid_map *map, *maps[2] = {
		[OPT_UIDMAP] = &ssi->uid_map,
		[OPT_GIDMAP] = &ssi->gid_map,
	};

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		struct uid_gid_extent ext;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		if (token != OPT_UIDMAP && token != OPT_GIDMAP)
			return -EINVAL;
		if (match_int(&args[0], &from) ||
		    match_int(&args[1], &to) ||
		    match_int(&args[2], &count))
			return -EINVAL;
		map = maps[token];
		if (map->nr_extents >= UID_GID_MAP_MAX_EXTENTS)
			return -EINVAL;
		ext.first = from;
		ext.lower_first = to;
		ext.count = count;
		if (mappings_overlap(map, &ext))
			return -EINVAL;
		map->extent[map->nr_extents++] = ext;
	}
	return 0;
}

static void shiftfs_d_release(struct dentry *dentry)
{
	struct dentry *real = dentry->d_fsdata;

	dput(real);
}

static struct dentry *shiftfs_d_real(struct dentry *dentry, struct inode *inode)
{
	struct dentry *real = dentry->d_fsdata;

	if (unlikely(real->d_flags & DCACHE_OP_REAL))
		return real->d_op->d_real(real, real->d_inode);

	return real;
}

static const struct dentry_operations shiftfs_dentry_ops = {
	.d_release	= shiftfs_d_release,
	.d_real		= shiftfs_d_real,
};

static int shiftfs_readlink(struct dentry *dentry, char __user *data,
			    int flags)
{
	struct dentry *real = dentry->d_fsdata;
	const struct inode_operations *iop = real->d_inode->i_op;

	if (iop->readlink)
		return iop->readlink(real, data, flags);

	return -EINVAL;
}

static const char *shiftfs_get_link(struct dentry *dentry, struct inode *inode,
				    struct delayed_call *done)
{
	if (dentry) {
		struct dentry *real = dentry->d_fsdata;
		struct inode *reali = real->d_inode;
		const struct inode_operations *iop = reali->i_op;
		const char *res = ERR_PTR(-EPERM);

		if (iop->get_link)
			res = iop->get_link(real, reali, done);

		return res;
	} else {
		/* RCU lookup not supported */
		return ERR_PTR(-ECHILD);
	}
}

static int shiftfs_setxattr(struct dentry *dentry, struct inode *inode,
			    const char *name, const void *value,
			    size_t size, int flags)
{
	struct dentry *real = dentry->d_fsdata;
	int err = -EOPNOTSUPP;
	const struct cred *oldcred, *newcred;

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	err = vfs_setxattr(real, name, value, size, flags);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static ssize_t shiftfs_getxattr(struct dentry *dentry, struct inode *inode,
				const char *name, void *value, size_t size)
{
	struct dentry *real = dentry->d_fsdata;
	int err;
	const struct cred *oldcred, *newcred;

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	err = vfs_getxattr(real, name, value, size);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static ssize_t shiftfs_listxattr(struct dentry *dentry, char *list,
				 size_t size)
{
	struct dentry *real = dentry->d_fsdata;
	int err;
	const struct cred *oldcred, *newcred;

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	err = vfs_listxattr(real, list, size);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static int shiftfs_removexattr(struct dentry *dentry, const char *name)
{
	struct dentry *real = dentry->d_fsdata;
	int err;
	const struct cred *oldcred, *newcred;

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	err = vfs_removexattr(real, name);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static void shiftfs_fill_inode(struct inode *inode, struct dentry *dentry)
{
	struct inode *reali;
	struct shiftfs_super_info *ssi = inode->i_sb->s_fs_info;

	if (!dentry)
		return;

	reali = dentry->d_inode;

	if (!reali->i_op->get_link)
		inode->i_opflags |= IOP_NOFOLLOW;

	inode->i_mapping = reali->i_mapping;
	inode->i_private = dentry;

	inode->i_uid = KUIDT_INIT(map_id_down(&ssi->uid_map, __kuid_val(reali->i_uid)));
	inode->i_gid = KGIDT_INIT(map_id_down(&ssi->gid_map, __kgid_val(reali->i_gid)));
}

static int shiftfs_make_object(struct inode *dir, struct dentry *dentry,
			       umode_t mode, const char *symlink,
			       struct dentry *hardlink, bool excl)
{
	struct dentry *real = dir->i_private, *new = dentry->d_fsdata;
	struct inode *reali = real->d_inode, *newi;
	const struct inode_operations *iop = reali->i_op;
	int err;
	const struct cred *oldcred, *newcred;
	bool op_ok = false;

	if (hardlink) {
		op_ok = iop->link;
	} else {
		switch (mode & S_IFMT) {
		case S_IFDIR:
			op_ok = iop->mkdir;
			break;
		case S_IFREG:
			op_ok = iop->create;
			break;
		case S_IFLNK:
			op_ok = iop->symlink;
		}
	}
	if (!op_ok)
		return -EINVAL;


	newi = shiftfs_new_inode(dentry->d_sb, mode, NULL);
	if (!newi)
		return -ENOMEM;

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);

	inode_lock_nested(reali, I_MUTEX_PARENT);

	err = -EINVAL;		/* shut gcc up about uninit var */
	if (hardlink) {
		struct dentry *realhardlink = hardlink->d_fsdata;

		err = vfs_link(realhardlink, reali, new, NULL);
	} else {
		switch (mode & S_IFMT) {
		case S_IFDIR:
			err = vfs_mkdir(reali, new, mode);
			break;
		case S_IFREG:
			err = vfs_create(reali, new, mode, excl);
			break;
		case S_IFLNK:
			err = vfs_symlink(reali, new, symlink);
		}
	}

	shiftfs_old_creds(oldcred, &newcred);

	if (err)
		goto out_dput;

	shiftfs_fill_inode(newi, new);

	d_splice_alias(newi, dentry);

	new = NULL;
	newi = NULL;

 out_dput:
	dput(new);
	iput(newi);
	inode_unlock(reali);

	return err;
}

static int shiftfs_create(struct inode *dir, struct dentry *dentry,
			  umode_t mode,  bool excl)
{
	mode |= S_IFREG;

	return shiftfs_make_object(dir, dentry, mode, NULL, NULL, excl);
}

static int shiftfs_mkdir(struct inode *dir, struct dentry *dentry,
			 umode_t mode)
{
	mode |= S_IFDIR;

	return shiftfs_make_object(dir, dentry, mode, NULL, NULL, false);
}

static int shiftfs_link(struct dentry *hardlink, struct inode *dir,
			struct dentry *dentry)
{
	return shiftfs_make_object(dir, dentry, 0, NULL, hardlink, false);
}

static int shiftfs_symlink(struct inode *dir, struct dentry *dentry,
			   const char *symlink)
{
	return shiftfs_make_object(dir, dentry, S_IFLNK, symlink, NULL, false);
}

static int shiftfs_rm(struct inode *dir, struct dentry *dentry, bool rmdir)
{
	struct dentry *real = dir->i_private, *new = dentry->d_fsdata;
	struct inode *reali = real->d_inode;
	int err;
	const struct cred *oldcred, *newcred;

	inode_lock_nested(reali, I_MUTEX_PARENT);

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);

	if (rmdir)
		err = vfs_rmdir(reali, new);
	else
		err = vfs_unlink(reali, new, NULL);

	shiftfs_old_creds(oldcred, &newcred);
	inode_unlock(reali);

	return err;
}

static int shiftfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return shiftfs_rm(dir, dentry, false);
}

static int shiftfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	return shiftfs_rm(dir, dentry, true);
}

static int shiftfs_rename2(struct inode *olddir, struct dentry *old,
			   struct inode *newdir, struct dentry *new,
			   unsigned int flags)
{
	struct dentry *rodd = olddir->i_private, *rndd = newdir->i_private,
		*realold = old->d_fsdata,
		*realnew = new->d_fsdata, *trap;
	struct inode *realolddir = rodd->d_inode, *realnewdir = rndd->d_inode;
	int err = -EINVAL;
	const struct cred *oldcred, *newcred;

	trap = lock_rename(rndd, rodd);

	if (trap == realold || trap == realnew)
		goto out_unlock;

	oldcred = shiftfs_new_creds(&newcred, old->d_sb);

	err = vfs_rename(realolddir, realold, realnewdir,
			 realnew, NULL, flags);

	shiftfs_old_creds(oldcred, &newcred);

 out_unlock:
	unlock_rename(rndd, rodd);

	return err;
}

static struct dentry *shiftfs_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	struct dentry *real = dir->i_private, *new;
	struct inode *reali = real->d_inode, *newi;
	const struct cred *oldcred, *newcred;

	/* note: violation of usual fs rules here: dentries are never
	 * added with d_add.  This is because we want no dentry cache
	 * for shiftfs.  All lookups proceed through the dentry cache
	 * of the underlying filesystem, meaning we always see any
	 * changes in the underlying */

	inode_lock(reali);
	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	new = lookup_one_len(dentry->d_name.name, real, dentry->d_name.len);
	shiftfs_old_creds(oldcred, &newcred);
	inode_unlock(reali);

	if (IS_ERR(new))
		return new;

	dentry->d_fsdata = new;

	if (!new->d_inode)
		return NULL;

	newi = shiftfs_new_inode(dentry->d_sb, new->d_inode->i_mode, new);
	if (!newi) {
		dput(new);
		return ERR_PTR(-ENOMEM);
	}

	d_splice_alias(newi, dentry);

	return NULL;
}

static int shiftfs_permission(struct inode *inode, int mask)
{
	struct dentry *real = inode->i_private;
	struct inode *reali = real->d_inode;
	const struct inode_operations *iop = reali->i_op;
	int err;
	const struct cred *oldcred, *newcred;

	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;

	oldcred = shiftfs_new_creds(&newcred, inode->i_sb);
	if (iop->permission)
		err = iop->permission(reali, mask);
	else
		err = generic_permission(reali, mask);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static int shiftfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct dentry *real = dentry->d_fsdata;
	struct inode *reali = real->d_inode;
	const struct inode_operations *iop = reali->i_op;
	struct iattr newattr = *attr;
	const struct cred *oldcred, *newcred;
	struct shiftfs_super_info *ssi = dentry->d_sb->s_fs_info;
	int err;

	newattr.ia_uid = KUIDT_INIT(map_id_up(&ssi->uid_map, __kuid_val(attr->ia_uid)));
	newattr.ia_gid = KGIDT_INIT(map_id_up(&ssi->gid_map, __kgid_val(attr->ia_gid)));

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	inode_lock(reali);
	if (iop->setattr)
		err = iop->setattr(real, &newattr);
	else
		err = simple_setattr(real, &newattr);
	inode_unlock(reali);
	shiftfs_old_creds(oldcred, &newcred);

	return err;
}

static int shiftfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct dentry *real = inode->i_private;
	struct inode *reali = real->d_inode;
	const struct inode_operations *iop = reali->i_op;
	int err = 0;

	mnt = dentry->d_sb->s_fs_info;

	if (iop->getattr)
		err = iop->getattr(mnt, real, stat);
	else
		generic_fillattr(reali, stat);

	if (err)
		return err;

	stat->uid = inode->i_uid;
	stat->gid = inode->i_gid;
	return 0;
}

struct shiftfs_fop_carrier {
	struct inode *inode;
	int (*release)(struct inode *, struct file *);
	struct file_operations fop;
};

static int shiftfs_release(struct inode *inode, struct file *file)
{
	struct shiftfs_fop_carrier *sfc;
	const struct cred *oldcred, *newcred;
	int err = 0;

	sfc = container_of(file->f_op, struct shiftfs_fop_carrier, fop);

	oldcred = shiftfs_new_creds(&newcred, sfc->inode->i_sb);
	if (sfc->release)
		err = sfc->release(inode, file);
	shiftfs_old_creds(oldcred, &newcred);

	/* FIXME: read/write accounting calls are always done
	 * on a saved copy of the inode, so in open they're always
	 * done on the upper inode, but in __fput() they're always
	 * done on the lower inode.  Fix that here by mirroring on
	 * the upper one */
	if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_dec(sfc->inode);
	if (file->f_mode & FMODE_WRITER)
		put_write_access(sfc->inode);

	file->f_inode = sfc->inode;
	file->f_op = sfc->inode->i_fop;
	fops_put(inode->i_fop);

	kfree(sfc);

	return err;
}

static int shiftfs_open(struct inode *inode, struct file *file)
{
	struct dentry *real = inode->i_private;
	struct inode *reali = real->d_inode;
	const struct file_operations *fop;
	struct shiftfs_fop_carrier *sfc;
	const struct cred *oldcred, *newcred;
	int err = 0;

	sfc = kmalloc(sizeof(*sfc), GFP_KERNEL);
	if (!sfc)
		return -ENOMEM;

	if (real->d_flags & DCACHE_OP_SELECT_INODE)
		reali = real->d_op->d_select_inode(real, file->f_flags);

	fop = fops_get(reali->i_fop);
	sfc->inode = inode;
	memcpy(&sfc->fop, fop, sizeof(*fop));
	sfc->release = sfc->fop.release;
	sfc->fop.release = shiftfs_release;

	file->f_op = &sfc->fop;
	file->f_inode = reali;

	if (file->f_mode & FMODE_WRITER) {
		err = get_write_access(reali);
		if (err)
			goto out_err;
	}

	oldcred = shiftfs_new_creds(&newcred, inode->i_sb);
	if (fop->open)
		err = fop->open(reali, file);
	shiftfs_old_creds(oldcred, &newcred);


	if (err) {
		put_write_access(reali);
 out_err:
		file->f_inode = inode;
		file->f_op = inode->i_fop;
		fops_put(reali->i_fop);
		kfree(sfc);
	} else if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ) {
		i_readcount_inc(reali);
	}
	return err;
}

static const struct inode_operations shiftfs_inode_ops = {
	.lookup		= shiftfs_lookup,
	.getattr	= shiftfs_getattr,
	.setattr	= shiftfs_setattr,
	.permission	= shiftfs_permission,
	.mkdir		= shiftfs_mkdir,
	.symlink	= shiftfs_symlink,
	.get_link	= shiftfs_get_link,
	.readlink	= shiftfs_readlink,
	.unlink		= shiftfs_unlink,
	.rmdir		= shiftfs_rmdir,
	.rename2	= shiftfs_rename2,
	.link		= shiftfs_link,
	.create		= shiftfs_create,
	.mknod		= NULL,	/* no special files currently */
	.setxattr	= shiftfs_setxattr,
	.getxattr	= shiftfs_getxattr,
	.listxattr	= shiftfs_listxattr,
	.removexattr	= shiftfs_removexattr,
};

static const struct file_operations shiftfs_file_ops = {
	.open		= shiftfs_open,
};

static struct inode *shiftfs_new_inode(struct super_block *sb, umode_t mode,
				       struct dentry *dentry)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	mode &= S_IFMT;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_flags |= S_NOATIME | S_NOCMTIME;

	inode->i_op = &shiftfs_inode_ops;
	inode->i_fop = &shiftfs_file_ops;

	shiftfs_fill_inode(inode, dentry);

	return inode;
}

static int shiftfs_show_options(struct seq_file *m, struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	struct shiftfs_super_info *ssi = sb->s_fs_info;

	static const char *options[] = { "uidmap", "gidmap" };
	const struct uid_gid_map *map[ARRAY_SIZE(options)] =
		{ &ssi->uid_map, &ssi->gid_map };
	int i, j;

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		for (j = 0; j < map[i]->nr_extents; j++) {
			const struct uid_gid_extent *ext = &map[i]->extent[j];

			seq_show_option(m, options[i], NULL);
			seq_printf(m, "=%u:%u:%u", ext->first,
				   ext->lower_first, ext->count);
		}
	}

	return 0;
}

static int shiftfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct shiftfs_super_info *ssi = sb->s_fs_info;
	struct dentry *root = sb->s_root;
	struct dentry *realroot = root->d_fsdata;
	struct path realpath = { .mnt = ssi->mnt, .dentry = realroot };
	int err;

	err = vfs_statfs(&realpath, buf);
	if (err)
		return err;

	buf->f_type = sb->s_magic;

	return 0;
}

static void shiftfs_put_super(struct super_block *sb)
{
	struct shiftfs_super_info *ssi = sb->s_fs_info;

	mntput(ssi->mnt);
	kfree(ssi);
}

static const struct super_operations shiftfs_super_ops = {
	.put_super	= shiftfs_put_super,
	.show_options	= shiftfs_show_options,
	.statfs		= shiftfs_statfs,
};

struct shiftfs_data {
	void *data;
	const char *path;
};

static int shiftfs_fill_super(struct super_block *sb, void *raw_data,
			      int silent)
{
	struct shiftfs_data *data = raw_data;
	char *name = kstrdup(data->path, GFP_KERNEL);
	int err = -ENOMEM;
	struct shiftfs_super_info *ssi = NULL;
	struct path path;

	if (!name)
		goto out;

	ssi = kzalloc(sizeof(*ssi), GFP_KERNEL);
	if (!ssi)
		goto out;

	err = -EPERM;
	if (!capable(CAP_SYS_ADMIN))
		goto out;

	err = shiftfs_parse_options(ssi, data->data);
	if (err)
		goto out;

	err = kern_path(name, LOOKUP_FOLLOW, &path);
	if (err)
		goto out;

	if (!S_ISDIR(path.dentry->d_inode->i_mode)) {
		err = -ENOTDIR;
		goto out_put;
	}
	ssi->mnt = path.mnt;

	sb->s_fs_info = ssi;
	sb->s_magic = SHIFTFS_MAGIC;
	sb->s_op = &shiftfs_super_ops;
	sb->s_d_op = &shiftfs_dentry_ops;
	sb->s_root = d_make_root(shiftfs_new_inode(sb, S_IFDIR, path.dentry));
	sb->s_root->d_fsdata = path.dentry;

	return 0;

 out_put:
	path_put(&path);
 out:
	kfree(name);
	if (err)
		kfree(ssi);
	return err;
}

static struct dentry *shiftfs_mount(struct file_system_type *fs_type,
				    int flags, const char *dev_name, void *data)
{
	struct shiftfs_data d = { data, dev_name };

	return mount_nodev(fs_type, flags, &d, shiftfs_fill_super);
}

static struct file_system_type shiftfs_type = {
	.owner		= THIS_MODULE,
	.name		= "shiftfs",
	.mount		= shiftfs_mount,
	.kill_sb	= kill_anon_super,
};

static int __init shiftfs_init(void)
{
	return register_filesystem(&shiftfs_type);
}

static void __exit shiftfs_exit(void)
{
	unregister_filesystem(&shiftfs_type);
}

MODULE_ALIAS_FS("shiftfs");
MODULE_AUTHOR("James Bottomley");
MODULE_DESCRIPTION("uid/gid shifting bind filesystem");
MODULE_LICENSE("GPL v2");
module_init(shiftfs_init)
module_exit(shiftfs_exit)
