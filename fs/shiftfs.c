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
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

struct shiftfs_super_info {
	struct vfsmount *mnt;
	struct user_namespace *userns;
	bool mark;
};

static struct inode *shiftfs_new_inode(struct super_block *sb, umode_t mode,
				       struct dentry *dentry);
static void shiftfs_init_inode(struct inode *inode, umode_t mode);

enum {
	OPT_MARK,
	OPT_LAST,
};

/* global filesystem options */
static const match_table_t tokens = {
	{ OPT_MARK, "mark" },
	{ OPT_LAST, NULL }
};

static const struct cred *shiftfs_get_up_creds(struct super_block *sb)
{
	struct shiftfs_super_info *ssi = sb->s_fs_info;
	struct cred *cred = prepare_creds();

	if (!cred)
		return NULL;

	cred->fsuid = KUIDT_INIT(from_kuid(sb->s_user_ns, cred->fsuid));
	cred->fsgid = KGIDT_INIT(from_kgid(sb->s_user_ns, cred->fsgid));
	put_user_ns(cred->user_ns);
	cred->user_ns = get_user_ns(ssi->userns);

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
		printk(KERN_ERR "shiftfs: Credential override failed: no memory\n");

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

	ssi->mark = false;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case OPT_MARK:
			ssi->mark = true;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static void shiftfs_d_release(struct dentry *dentry)
{
	struct dentry *real = dentry->d_fsdata;

	dput(real);
}

static struct dentry *shiftfs_d_real(struct dentry *dentry,
				     const struct inode *inode,
				     unsigned int open_flags,
				     unsigned int dreal_flags)
{
	struct dentry *real = dentry->d_fsdata;

	if (unlikely(real->d_flags & DCACHE_OP_REAL))
		return real->d_op->d_real(real, real->d_inode,
					  open_flags, dreal_flags);

	return real;
}

static int shiftfs_d_weak_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *real = dentry->d_fsdata;

	if (d_unhashed(real))
		return 0;

	if (!(real->d_flags & DCACHE_OP_WEAK_REVALIDATE))
		return 1;

	return real->d_op->d_weak_revalidate(real, flags);
}

static int shiftfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *real = dentry->d_fsdata;
	int ret;

	if (d_unhashed(real))
		return 0;

	/*
	 * inode state of underlying changed from positive to negative
	 * or vice versa; force a lookup to update our view
	 */
	if (d_is_negative(real) != d_is_negative(dentry))
		return 0;

	if (!(real->d_flags & DCACHE_OP_REVALIDATE))
		return 1;

	ret = real->d_op->d_revalidate(real, flags);

	if (ret == 0 && !(flags & LOOKUP_RCU))
		d_invalidate(real);

	return ret;
}

static const struct dentry_operations shiftfs_dentry_ops = {
	.d_release	= shiftfs_d_release,
	.d_real		= shiftfs_d_real,
	.d_revalidate	= shiftfs_d_revalidate,
	.d_weak_revalidate = shiftfs_d_weak_revalidate,
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

static int shiftfs_xattr_get(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
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

static int shiftfs_xattr_set(const struct xattr_handler *handler,
			     struct dentry *dentry, struct inode *inode,
			     const char *name, const void *value, size_t size,
			     int flags)
{
	if (!value)
		return shiftfs_removexattr(dentry, name);
	return shiftfs_setxattr(dentry, inode, name, value, size, flags);
}

static kuid_t shift_kuid(struct user_namespace *from, struct user_namespace *to,
			 kuid_t kuid)
{
	uid_t uid = from_kuid(from, kuid);
	return make_kuid(to, uid);
}

static kgid_t shift_kgid(struct user_namespace *from, struct user_namespace *to,
			 kgid_t kgid)
{
	gid_t gid = from_kgid(from, kgid);
	return make_kgid(to, gid);
}

static void shiftfs_copyattr(struct inode *from, struct inode *to)
{
	struct user_namespace *from_ns = from->i_sb->s_user_ns;
	struct user_namespace *to_ns = to->i_sb->s_user_ns;

	to->i_uid = shift_kuid(from_ns, to_ns, from->i_uid);
	to->i_gid = shift_kgid(from_ns, to_ns, from->i_gid);
	to->i_mode = from->i_mode;
	to->i_atime = from->i_atime;
	to->i_mtime = from->i_mtime;
	to->i_ctime = from->i_ctime;
}

static void shiftfs_fill_inode(struct inode *inode, struct dentry *dentry)
{
	struct inode *reali;

	if (!dentry)
		return;

	reali = dentry->d_inode;

	if (!reali->i_op->get_link)
		inode->i_opflags |= IOP_NOFOLLOW;

	shiftfs_copyattr(reali, inode);
	inode->i_mapping = reali->i_mapping;
	inode->i_private = reali;
	set_nlink(inode, reali->i_nlink);
}

static int shiftfs_inode_test(struct inode *inode, void *data)
{
	return inode->i_private == data;
}

static int shiftfs_inode_set(struct inode *inode, void *data)
{
	inode->i_private = data;
	return 0;
}

static int shiftfs_make_object(struct inode *dir, struct dentry *dentry,
			       umode_t mode, const char *symlink,
			       struct dentry *hardlink, bool excl)
{
	struct dentry *new = dentry->d_fsdata;
	struct inode *reali = dir->i_private, *inode, *newi;
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


	if (hardlink) {
		inode = d_inode(hardlink);
		ihold(inode);
	} else {
		inode = shiftfs_new_inode(dentry->d_sb, mode, NULL);
		if (!inode)
			return -ENOMEM;
	}

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

	if (hardlink) {
		WARN_ON(inode->i_private != new->d_inode);
		inc_nlink(inode);
	} else {
		shiftfs_fill_inode(inode, new);

		newi = inode_insert5(inode, (unsigned long)new->d_inode,
				     shiftfs_inode_test, shiftfs_inode_set,
				     new->d_inode);
		if (newi != inode) {
			pr_warn_ratelimited("shiftfs: newly created inode found in cache\n");
			iput(inode);
			inode = newi;
		}
	}

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);

	d_instantiate(dentry, inode);

	new = NULL;
	inode = NULL;

 out_dput:
	dput(new);
	iput(inode);
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
	struct dentry *new = dentry->d_fsdata;
	struct inode *reali = dir->i_private;
	int err;
	const struct cred *oldcred, *newcred;

	inode_lock_nested(reali, I_MUTEX_PARENT);

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);

	if (rmdir)
		err = vfs_rmdir(reali, new);
	else
		err = vfs_unlink(reali, new, NULL);

	if (!err) {
		if (rmdir)
			clear_nlink(d_inode(dentry));
		else
			drop_nlink(d_inode(dentry));
	}

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

static int shiftfs_rename(struct inode *olddir, struct dentry *old,
			  struct inode *newdir, struct dentry *new,
			  unsigned int flags)
{
	struct dentry *rodd = old->d_parent->d_fsdata,
		*rndd = new->d_parent->d_fsdata,
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
	struct dentry *real = dentry->d_parent->d_fsdata, *new;
	struct inode *reali = real->d_inode, *newi, *inode;
	const struct cred *oldcred, *newcred;

	inode_lock(reali);
	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	new = lookup_one_len(dentry->d_name.name, real, dentry->d_name.len);
	shiftfs_old_creds(oldcred, &newcred);
	inode_unlock(reali);

	if (IS_ERR(new))
		return new;

	dentry->d_fsdata = new;

	inode = NULL;
	newi = new->d_inode;
	if (!newi)
		goto out;

	inode = iget5_locked(dentry->d_sb, (unsigned long)newi,
			     shiftfs_inode_test, shiftfs_inode_set, newi);
	if (!inode) {
		dput(new);
		return ERR_PTR(-ENOMEM);
	}
	if (inode->i_state & I_NEW) {
		shiftfs_init_inode(inode, newi->i_mode);
		shiftfs_fill_inode(inode, new);
		unlock_new_inode(inode);
	}

 out:
	return d_splice_alias(inode, dentry);
}

static int shiftfs_permission(struct inode *inode, int mask)
{
	struct inode *reali = inode->i_private;
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
	struct super_block *sb = dentry->d_sb;
	int err;

	newattr.ia_uid = KUIDT_INIT(from_kuid(sb->s_user_ns, attr->ia_uid));
	newattr.ia_gid = KGIDT_INIT(from_kgid(sb->s_user_ns, attr->ia_gid));

	oldcred = shiftfs_new_creds(&newcred, dentry->d_sb);
	inode_lock(reali);
	if (iop->setattr)
		err = iop->setattr(real, &newattr);
	else
		err = simple_setattr(real, &newattr);
	inode_unlock(reali);
	shiftfs_old_creds(oldcred, &newcred);

	if (err)
		return err;

	/* all OK, reflect the change on our inode */
	shiftfs_copyattr(reali, d_inode(dentry));
	return 0;
}

static int shiftfs_getattr(const struct path *path, struct kstat *stat,
			   u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = path->dentry->d_inode;
	struct dentry *real = path->dentry->d_fsdata;
	struct inode *reali = real->d_inode;
	const struct inode_operations *iop = reali->i_op;
	struct path newpath = { .mnt = path->dentry->d_sb->s_fs_info, .dentry = real };
	struct user_namespace *from_ns = reali->i_sb->s_user_ns;
	struct user_namespace *to_ns = inode->i_sb->s_user_ns;
	int err = 0;

	if (iop->getattr)
		err = iop->getattr(&newpath, stat, request_mask, query_flags);
	else
		generic_fillattr(reali, stat);

	if (err)
		return err;

	/* transform the underlying id */
	stat->uid = shift_kuid(from_ns, to_ns, stat->uid);
	stat->gid = shift_kgid(from_ns, to_ns, stat->gid);
	return 0;
}

#ifdef CONFIG_SHIFT_FS_POSIX_ACL

static int
shift_acl_ids(struct user_namespace *from, struct user_namespace *to,
	      struct posix_acl *acl)
{
	int i;

	for (i = 0; i < acl->a_count; i++) {
		struct posix_acl_entry *e = &acl->a_entries[i];
		switch(e->e_tag) {
		case ACL_USER:
			e->e_uid = shift_kuid(from, to, e->e_uid);
			if (!uid_valid(e->e_uid))
				return -EOVERFLOW;
			break;
		case ACL_GROUP:
			e->e_gid = shift_kgid(from, to, e->e_gid);
			if (!gid_valid(e->e_gid))
				return -EOVERFLOW;
			break;
		}
	}
	return 0;
}

static void
shift_acl_xattr_ids(struct user_namespace *from, struct user_namespace *to,
		    void *value, size_t size)
{
	struct posix_acl_xattr_header *header = value;
	struct posix_acl_xattr_entry *entry = (void *)(header + 1), *end;
	int count;
	kuid_t kuid;
	kgid_t kgid;

	if (!value)
		return;
	if (size < sizeof(struct posix_acl_xattr_header))
		return;
	if (header->a_version != cpu_to_le32(POSIX_ACL_XATTR_VERSION))
		return;

	count = posix_acl_xattr_count(size);
	if (count < 0)
		return;
	if (count == 0)
		return;

	for (end = entry + count; entry != end; entry++) {
		switch(le16_to_cpu(entry->e_tag)) {
		case ACL_USER:
			kuid = make_kuid(&init_user_ns, le32_to_cpu(entry->e_id));
			kuid = shift_kuid(from, to, kuid);
			entry->e_id = cpu_to_le32(from_kuid(&init_user_ns, kuid));
			break;
		case ACL_GROUP:
			kgid = make_kgid(&init_user_ns, le32_to_cpu(entry->e_id));
			kgid = shift_kgid(from, to, kgid);
			entry->e_id = cpu_to_le32(from_kgid(&init_user_ns, kgid));
			break;
		default:
			break;
		}
	}
}

static struct posix_acl *shiftfs_get_acl(struct inode *inode, int type)
{
	struct inode *reali = inode->i_private;
	const struct cred *oldcred, *newcred;
	struct posix_acl *real_acl, *acl = NULL;
	struct user_namespace *from_ns = reali->i_sb->s_user_ns;
	struct user_namespace *to_ns = inode->i_sb->s_user_ns;
	int size;
	int err;

	if (!IS_POSIXACL(reali))
		return NULL;

	oldcred = shiftfs_new_creds(&newcred, inode->i_sb);
	real_acl = get_acl(reali, type);
	shiftfs_old_creds(oldcred, &newcred);

	if (real_acl && !IS_ERR(acl)) {
		/* XXX: export posix_acl_clone? */
		size = sizeof(struct posix_acl) +
			   real_acl->a_count * sizeof(struct posix_acl_entry);
		acl = kmemdup(acl, size, GFP_KERNEL);
		posix_acl_release(real_acl);

		if (!acl)
			return ERR_PTR(-ENOMEM);

		refcount_set(&acl->a_refcount, 1);

		err = shift_acl_ids(from_ns, to_ns, acl);
		if (err) {
			kfree(acl);
			return ERR_PTR(err);
		}
	}

	return acl;
}

static int
shiftfs_posix_acl_xattr_get(const struct xattr_handler *handler,
			   struct dentry *dentry, struct inode *inode,
			   const char *name, void *buffer, size_t size)
{
	struct inode *reali = inode->i_private;
	int ret;

	ret = shiftfs_xattr_get(NULL, dentry, inode, handler->name,
				buffer, size);
	if (ret < 0)
		return ret;

	shift_acl_xattr_ids(reali->i_sb->s_user_ns, inode->i_sb->s_user_ns,
			    buffer, size);
	return ret;
}

static int
shiftfs_posix_acl_xattr_set(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, const void *value,
			    size_t size, int flags)
{
	struct inode *reali = inode->i_private;
	int err;

	if (!IS_POSIXACL(reali) || !reali->i_op->set_acl)
		return -EOPNOTSUPP;
	if (handler->flags == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode))
		return value ? -EACCES : 0;
	if (!inode_owner_or_capable(inode))
		return -EPERM;

	if (value) {
		shift_acl_xattr_ids(inode->i_sb->s_user_ns,
				    reali->i_sb->s_user_ns,
				    (void *)value, size);
		err = shiftfs_setxattr(dentry, inode, handler->name, value,
				       size, flags);
	} else {
		err = shiftfs_removexattr(dentry, handler->name);
	}

	if (!err)
		shiftfs_copyattr(reali, inode);
	return err;
}

static const struct xattr_handler
shiftfs_posix_acl_access_xattr_handler = {
	.name = XATTR_NAME_POSIX_ACL_ACCESS,
	.flags = ACL_TYPE_ACCESS,
	.get = shiftfs_posix_acl_xattr_get,
	.set = shiftfs_posix_acl_xattr_set,
};

static const struct xattr_handler
shiftfs_posix_acl_default_xattr_handler = {
	.name = XATTR_NAME_POSIX_ACL_DEFAULT,
	.flags = ACL_TYPE_DEFAULT,
	.get = shiftfs_posix_acl_xattr_get,
	.set = shiftfs_posix_acl_xattr_set,
};

#else /* !CONFIG_SHIFT_FS_POSIX_ACL */

#define shiftfs_get_acl NULL

#endif /* CONFIG_SHIFT_FS_POSIX_ACL */

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
	.rename		= shiftfs_rename,
	.link		= shiftfs_link,
	.create		= shiftfs_create,
	.mknod		= NULL,	/* no special files currently */
	.listxattr	= shiftfs_listxattr,
	.get_acl	= shiftfs_get_acl,
};

static struct inode *shiftfs_new_inode(struct super_block *sb, umode_t mode,
				       struct dentry *dentry)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	shiftfs_init_inode(inode, mode);
	shiftfs_fill_inode(inode, dentry);

	return inode;
}

static void shiftfs_init_inode(struct inode *inode, umode_t mode)
{
	/*
	 * our inode is completely vestigial.  All lookups, getattr
	 * and permission checks are done on the underlying inode, so
	 * what the user sees is entirely from the underlying inode.
	 */
	mode &= S_IFMT;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_flags |= S_NOATIME | S_NOCMTIME;

	inode->i_op = &shiftfs_inode_ops;
}

static int shiftfs_show_options(struct seq_file *m, struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	struct shiftfs_super_info *ssi = sb->s_fs_info;

	if (ssi->mark)
		seq_show_option(m, "mark", NULL);

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
	put_user_ns(ssi->userns);
	kfree(ssi);
}

static const struct xattr_handler shiftfs_xattr_handler = {
	.prefix = "",
	.get    = shiftfs_xattr_get,
	.set    = shiftfs_xattr_set,
};

const struct xattr_handler *shiftfs_xattr_handlers[] = {
#ifdef CONFIG_SHIFT_FS_POSIX_ACL
	&shiftfs_posix_acl_access_xattr_handler,
	&shiftfs_posix_acl_default_xattr_handler,
#endif
	&shiftfs_xattr_handler,
	NULL
};

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
	struct shiftfs_super_info *ssi = NULL, *mp_ssi;
	struct path path;
	struct dentry *dentry;

	if (!name)
		goto out;

	ssi = kzalloc(sizeof(*ssi), GFP_KERNEL);
	if (!ssi)
		goto out;

	err = -EPERM;
	err = shiftfs_parse_options(ssi, data->data);
	if (err)
		goto out;

	/* to mount a mark, must be userns admin */
	if (!ssi->mark && !ns_capable(current_user_ns(), CAP_SYS_ADMIN))
		goto out;

	err = kern_path(name, LOOKUP_FOLLOW, &path);
	if (err)
		goto out;

	err = -EPERM;

	if (!S_ISDIR(path.dentry->d_inode->i_mode)) {
		err = -ENOTDIR;
		goto out_put_path;
	}

	if (ssi->mark) {
		struct super_block *lower_sb = path.mnt->mnt_sb;

		/* to mark a mount point, must root wrt lower s_user_ns */
		if (!ns_capable(lower_sb->s_user_ns, CAP_SYS_ADMIN))
			goto out_put_path;


		/*
		 * this part is visible unshifted, so make sure no
		 * executables that could be used to give suid
		 * privileges
		 */
		sb->s_iflags = SB_I_NOEXEC;

		/*
		 * Handle nesting of shiftfs mounts by referring this mark
		 * mount back to the original mark mount. This is more
		 * efficient and alleviates concerns about stack depth.
		 */
		if (lower_sb->s_magic == SHIFTFS_MAGIC) {
			mp_ssi = lower_sb->s_fs_info;

			/* Doesn't make sense to mark a mark mount */
			if (mp_ssi->mark) {
				err = -EINVAL;
				goto out_put_path;
			}

			ssi->mnt = mntget(mp_ssi->mnt);
			dentry = dget(path.dentry->d_fsdata);
		} else {
			ssi->mnt = mntget(path.mnt);
			dentry = dget(path.dentry);
		}
	} else {
		/*
		 * this leg executes if we're admin capable in
		 * the namespace, so be very careful
		 */
		if (path.dentry->d_sb->s_magic != SHIFTFS_MAGIC)
			goto out_put_path;
		mp_ssi = path.dentry->d_sb->s_fs_info;
		if (!mp_ssi->mark)
			goto out_put_path;
		ssi->mnt = mntget(mp_ssi->mnt);
		dentry = dget(path.dentry->d_fsdata);
	}

	sb->s_stack_depth = dentry->d_sb->s_stack_depth + 1;
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		printk(KERN_ERR "shiftfs: maximum stacking depth exceeded\n");
		err = -EINVAL;
		goto out_put_mnt;
	}

	path_put(&path);
	ssi->userns = get_user_ns(dentry->d_sb->s_user_ns);
	sb->s_fs_info = ssi;
	sb->s_magic = SHIFTFS_MAGIC;
	sb->s_op = &shiftfs_super_ops;
	sb->s_xattr = shiftfs_xattr_handlers;
	sb->s_d_op = &shiftfs_dentry_ops;
	sb->s_flags |= SB_POSIXACL;
	sb->s_root = d_make_root(shiftfs_new_inode(sb, S_IFDIR, dentry));
	sb->s_root->d_fsdata = dentry;

	return 0;

 out_put_mnt:
	mntput(ssi->mnt);
	dput(dentry);
 out_put_path:
	path_put(&path);
 out:
	kfree(name);
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
	.fs_flags	= FS_USERNS_MOUNT,
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
