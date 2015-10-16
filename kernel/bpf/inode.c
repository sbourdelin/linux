/*
 * Minimal file system backend for special inodes holding eBPF maps and
 * programs, used by eBPF fd pinning.
 *
 * (C) 2015 Daniel Borkmann <daniel@iogearbox.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/parser.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/fsnotify.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/security.h>
#include <linux/xattr.h>

#define BPFFS_DEFAULT_MODE 0700

enum {
	BPF_OPT_UID,
	BPF_OPT_GID,
	BPF_OPT_MODE,
	BPF_OPT_ERR,
};

struct bpf_mnt_opts {
	kuid_t uid;
	kgid_t gid;
	umode_t mode;
};

struct bpf_fs_info {
	struct bpf_mnt_opts mnt_opts;
};

struct bpf_dir_state {
	unsigned long flags;
};

static const match_table_t bpf_tokens = {
	{ BPF_OPT_UID, "uid=%u" },
	{ BPF_OPT_GID, "gid=%u" },
	{ BPF_OPT_MODE, "mode=%o" },
	{ BPF_OPT_ERR, NULL },
};

static const struct inode_operations bpf_dir_iops;
static const struct inode_operations bpf_prog_iops;
static const struct inode_operations bpf_map_iops;

static struct inode *bpf_get_inode(struct super_block *sb,
				   const struct inode *dir,
				   umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return ERR_PTR(-ENOSPC);

	inode->i_ino = get_next_ino();
	inode->i_atime = CURRENT_TIME;
	inode->i_mtime = inode->i_atime;
	inode->i_ctime = inode->i_atime;
	inode_init_owner(inode, dir, mode);

	return inode;
}

static struct inode *bpf_mknod(struct inode *dir, umode_t mode)
{
	return bpf_get_inode(dir->i_sb, dir, mode);
}

static bool bpf_dentry_name_reserved(const struct dentry *dentry)
{
	return strchr(dentry->d_name.name, '.');
}

enum {
	/* Directory state is 'terminating', so no subdirectories
	 * are allowed anymore in this directory. This is being
	 * reserved so that in future, auto-generated directories
	 * could be added along with the special map/prog inodes.
	 */
	BPF_DSTATE_TERM_BIT,
};

static bool bpf_inode_is_term(struct inode *dir)
{
	struct bpf_dir_state *state = dir->i_private;

	return test_bit(BPF_DSTATE_TERM_BIT, &state->flags);
}

static bool bpf_inode_make_term(struct inode *dir)
{
	struct bpf_dir_state *state = dir->i_private;

	return dir->i_nlink != 2 ||
	       test_and_set_bit(BPF_DSTATE_TERM_BIT, &state->flags);
}

static void bpf_inode_undo_term(struct inode *dir)
{
	struct bpf_dir_state *state = dir->i_private;

	clear_bit(BPF_DSTATE_TERM_BIT, &state->flags);
}

static int bpf_inode_type(const struct inode *inode, enum bpf_fd_type *i_type)
{
	if (inode->i_op == &bpf_prog_iops)
		*i_type = BPF_FD_TYPE_PROG;
	else if (inode->i_op == &bpf_map_iops)
		*i_type = BPF_FD_TYPE_MAP;
	else
		return -EACCES;

	return 0;
}

static int bpf_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	void *i_private = inode->i_private;
	enum bpf_fd_type type;
	bool is_fd, drop_ref;
	int ret;

	is_fd = bpf_inode_type(inode, &type) == 0;
	drop_ref = inode->i_nlink == 1;

	ret = simple_unlink(dir, dentry);
	if (!ret && is_fd && drop_ref) {
		union bpf_any raw;

		raw.raw_ptr = i_private;
		bpf_any_put(raw, type);
		bpf_inode_undo_term(dir);
	}

	return ret;
}

static int bpf_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct bpf_dir_state *state;
	struct inode *inode;

	if (bpf_inode_is_term(dir))
		return -EPERM;
	if (bpf_dentry_name_reserved(dentry))
		return -EPERM;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOSPC;

	inode = bpf_mknod(dir, dir->i_mode);
	if (IS_ERR(inode)) {
		kfree(state);
		return PTR_ERR(inode);
	}

	inode->i_private = state;
	inode->i_op = &bpf_dir_iops;
	inode->i_fop = &simple_dir_operations;

	inc_nlink(inode);
	inc_nlink(dir);

	d_instantiate(dentry, inode);
	dget(dentry);

	return 0;
}

static int bpf_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	void *i_private = inode->i_private;
	int ret;

	ret = simple_rmdir(dir, dentry);
	if (!ret)
		kfree(i_private);

	return ret;
}

static const struct inode_operations bpf_dir_iops = {
	.lookup		= simple_lookup,
	.mkdir		= bpf_mkdir,
	.rmdir		= bpf_rmdir,
	.unlink		= bpf_unlink,
};

#define XATTR_TYPE_SUFFIX "type"

#define XATTR_NAME_BPF_TYPE (XATTR_BPF_PREFIX XATTR_TYPE_SUFFIX)
#define XATTR_NAME_BPF_TYPE_LEN (sizeof(XATTR_NAME_BPF_TYPE) - 1)

#define XATTR_VALUE_MAP "map"
#define XATTR_VALUE_PROG "prog"

static ssize_t bpf_getxattr(struct dentry *dentry, const char *name,
			    void *buffer, size_t size)
{
	enum bpf_fd_type type;
	ssize_t ret;

	if (strncmp(name, XATTR_NAME_BPF_TYPE, XATTR_NAME_BPF_TYPE_LEN))
		return -ENODATA;

	if (bpf_inode_type(d_inode(dentry), &type))
		return -ENODATA;

	switch (type) {
	case BPF_FD_TYPE_PROG:
		ret = sizeof(XATTR_VALUE_PROG);
		break;
	case BPF_FD_TYPE_MAP:
		ret = sizeof(XATTR_VALUE_MAP);
		break;
	}

	if (buffer) {
		if (size < ret)
			return -ERANGE;

		switch (type) {
		case BPF_FD_TYPE_PROG:
			strncpy(buffer, XATTR_VALUE_PROG, ret);
			break;
		case BPF_FD_TYPE_MAP:
			strncpy(buffer, XATTR_VALUE_MAP, ret);
			break;
		}
	}

	return ret;
}

static ssize_t bpf_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	ssize_t len, used = 0;

	len = security_inode_listsecurity(d_inode(dentry), buffer, size);
	if (len < 0)
		return len;

	used += len;
	if (buffer) {
		if (size < used)
			return -ERANGE;

		buffer += len;
	}

	len = XATTR_NAME_BPF_TYPE_LEN + 1;
	used += len;
	if (buffer) {
		if (size < used)
			return -ERANGE;

		memcpy(buffer, XATTR_NAME_BPF_TYPE, len);
		buffer += len;
	}

	return used;
}

/* Special inodes handling map/progs currently don't allow for syscalls
 * such as open/read/write/etc. We use the same bpf_{map,prog}_new_fd()
 * facility for installing an fd to the user as we do on BPF_MAP_CREATE
 * and BPF_PROG_LOAD, so an applications using bpf(2) don't see any
 * change in behaviour. In future, the set of open/read/write/etc could
 * be used f.e. for implementing things like debugging facilities on the
 * underlying map/prog that would work with non-bpf(2) aware tooling.
 */
static const struct inode_operations bpf_prog_iops = {
	.getxattr	= bpf_getxattr,
	.listxattr	= bpf_listxattr,
};

static const struct inode_operations bpf_map_iops = {
	.getxattr	= bpf_getxattr,
	.listxattr	= bpf_listxattr,
};

static int bpf_mkmap(struct inode *dir, struct dentry *dentry,
		     struct bpf_map *map, umode_t i_mode)
{
	struct inode *inode;

	if (bpf_dentry_name_reserved(dentry))
		return -EPERM;
	if (bpf_inode_make_term(dir))
		return -EBUSY;

	inode = bpf_mknod(dir, i_mode);
	if (IS_ERR(inode)) {
		bpf_inode_undo_term(dir);
		return PTR_ERR(inode);
	}

	inode->i_private = map;
	inode->i_op = &bpf_map_iops;

	d_instantiate(dentry, inode);
	dget(dentry);

	return 0;
}

static int bpf_mkprog(struct inode *dir, struct dentry *dentry,
		      struct bpf_prog *prog, umode_t i_mode)
{
	struct inode *inode;

	if (bpf_dentry_name_reserved(dentry))
		return -EPERM;
	if (bpf_inode_make_term(dir))
		return -EBUSY;

	inode = bpf_mknod(dir, i_mode);
	if (IS_ERR(inode)) {
		bpf_inode_undo_term(dir);
		return PTR_ERR(inode);
	}

	inode->i_private = prog;
	inode->i_op = &bpf_prog_iops;

	d_instantiate(dentry, inode);
	dget(dentry);

	return 0;
}

static const struct bpf_mnt_opts *bpf_sb_mnt_opts(const struct super_block *sb)
{
	const struct bpf_fs_info *bfi = sb->s_fs_info;

	return &bfi->mnt_opts;
}

static int bpf_parse_options(char *opt_data, struct bpf_mnt_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	unsigned int opt_val, token;
	char *opt_ptr;
	kuid_t uid;
	kgid_t gid;

	opts->mode = BPFFS_DEFAULT_MODE;

	while ((opt_ptr = strsep(&opt_data, ",")) != NULL) {
		if (!*opt_ptr)
			continue;

		token = match_token(opt_ptr, bpf_tokens, args);
		switch (token) {
		case BPF_OPT_UID:
			if (match_int(&args[0], &opt_val))
				return -EINVAL;

			uid = make_kuid(current_user_ns(), opt_val);
			if (!uid_valid(uid))
				return -EINVAL;

			opts->uid = uid;
			break;
		case BPF_OPT_GID:
			if (match_int(&args[0], &opt_val))
				return -EINVAL;

			gid = make_kgid(current_user_ns(), opt_val);
			if (!gid_valid(gid))
				return -EINVAL;

			opts->gid = gid;
			break;
		case BPF_OPT_MODE:
			if (match_octal(&args[0], &opt_val))
				return -EINVAL;

			opts->mode = opt_val & S_IALLUGO;
			break;
		default:
			return -EINVAL;
		};
	}

	return 0;
}

static int bpf_apply_options(struct super_block *sb)
{
	const struct bpf_mnt_opts *opts = bpf_sb_mnt_opts(sb);
	struct inode *inode = sb->s_root->d_inode;

	inode->i_mode &= ~S_IALLUGO;
	inode->i_mode |= opts->mode;

	inode->i_uid = opts->uid;
	inode->i_gid = opts->gid;

	return 0;
}

static int bpf_show_options(struct seq_file *m, struct dentry *root)
{
	const struct bpf_mnt_opts *opts = bpf_sb_mnt_opts(root->d_sb);

	if (!uid_eq(opts->uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, opts->uid));

	if (!gid_eq(opts->gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, opts->gid));

	if (opts->mode != BPFFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", opts->mode);

	return 0;
}

static int bpf_remount(struct super_block *sb, int *flags, char *opt_data)
{
	struct bpf_fs_info *bfi = sb->s_fs_info;
	int ret;

	sync_filesystem(sb);

	ret = bpf_parse_options(opt_data, &bfi->mnt_opts);
	if (ret)
		return ret;

	bpf_apply_options(sb);
	return 0;
}

static const struct super_operations bpf_super_ops = {
	.statfs		= simple_statfs,
	.remount_fs	= bpf_remount,
	.show_options	= bpf_show_options,
};

static int bpf_fill_super(struct super_block *sb, void *opt_data, int silent)
{
	static struct tree_descr bpf_files[] = { { "" } };
	struct bpf_dir_state *state;
	struct bpf_fs_info *bfi;
	struct inode *inode;
	int ret = -ENOMEM;

	bfi = kzalloc(sizeof(*bfi), GFP_KERNEL);
	if (!bfi)
		return ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		goto err_bfi;

	save_mount_options(sb, opt_data);
	sb->s_fs_info = bfi;

	ret = bpf_parse_options(opt_data, &bfi->mnt_opts);
	if (ret)
		goto err_state;

	ret = simple_fill_super(sb, BPFFS_MAGIC, bpf_files);
	if (ret)
		goto err_state;

	sb->s_op = &bpf_super_ops;

	inode = sb->s_root->d_inode;
	inode->i_op = &bpf_dir_iops;
	inode->i_private = state;

	bpf_apply_options(sb);

	return 0;
err_state:
	kfree(state);
err_bfi:
	kfree(bfi);
	return ret;
}

static void bpf_kill_super(struct super_block *sb)
{
	kfree(sb->s_root->d_inode->i_private);
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct dentry *bpf_mount(struct file_system_type *type,
				int flags, const char *dev_name,
				void *opt_data)
{
	return mount_nodev(type, flags, opt_data, bpf_fill_super);
}

static struct file_system_type bpf_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "bpf",
	.mount		= bpf_mount,
	.kill_sb	= bpf_kill_super,
	.fs_flags	= FS_USERNS_MOUNT,
};

MODULE_ALIAS_FS("bpf");
MODULE_ALIAS_FS("bpffs");

static int __init bpf_init(void)
{
	return register_filesystem(&bpf_fs_type);
}
fs_initcall(bpf_init);

int bpf_fd_inode_add(const struct filename *pathname,
		     union bpf_any raw, enum bpf_fd_type type)
{
	umode_t i_mode = S_IFREG | S_IRUSR | S_IWUSR;
	struct inode *dir_inode;
	struct dentry *dentry;
	struct path path;
	int ret;

	dentry = kern_path_create(AT_FDCWD, pathname->name, &path, 0);
	if (IS_ERR(dentry)) {
		ret = PTR_ERR(dentry);
		return ret;
	}

	ret = security_path_mknod(&path, dentry, i_mode, 0);
	if (ret)
		goto out;

	dir_inode = d_inode(path.dentry);
	if (dir_inode->i_op != &bpf_dir_iops) {
		ret = -EACCES;
		goto out;
	}

	ret = security_inode_mknod(dir_inode, dentry, i_mode, 0);
	if (ret)
		goto out;

	switch (type) {
	case BPF_FD_TYPE_PROG:
		ret = bpf_mkprog(dir_inode, dentry, raw.prog, i_mode);
		break;
	case BPF_FD_TYPE_MAP:
		ret = bpf_mkmap(dir_inode, dentry, raw.map, i_mode);
		break;
	}
out:
	done_path_create(&path, dentry);
	return ret;
}

union bpf_any bpf_fd_inode_get(const struct filename *pathname,
			       enum bpf_fd_type *type)
{
	struct inode *inode;
	union bpf_any raw;
	struct path path;
	int ret;

	ret = kern_path(pathname->name, LOOKUP_FOLLOW, &path);
	if (ret)
		goto out;

	inode = d_backing_inode(path.dentry);
	ret = inode_permission(inode, MAY_WRITE);
	if (ret)
		goto out_path;

	ret = bpf_inode_type(inode, type);
	if (ret)
		goto out_path;

	raw.raw_ptr = inode->i_private;
	if (!raw.raw_ptr) {
		ret = -EACCES;
		goto out_path;
	}

	bpf_any_get(raw, *type);
	touch_atime(&path);
	path_put(&path);

	return raw;
out_path:
	path_put(&path);
out:
	raw.raw_ptr = ERR_PTR(ret);
	return raw;
}
