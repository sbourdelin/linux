/*
 * Copyright 1997-1998 Transmeta Corporation -- All Rights Reserved
 * Copyright 2005-2006 Ian Kent <raven@themaw.net>
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 */

#include <linux/seq_file.h>
#include <linux/pagemap.h>
#include <linux/parser.h>

#include "autofs_i.h"

struct autofs_info *autofs_new_ino(struct autofs_sb_info *sbi)
{
	struct autofs_info *ino;

	ino = kzalloc(sizeof(*ino), GFP_KERNEL);
	if (ino) {
		INIT_LIST_HEAD(&ino->active);
		INIT_LIST_HEAD(&ino->expiring);
		ino->last_used = jiffies;
		ino->sbi = sbi;
	}
	return ino;
}

void autofs_clean_ino(struct autofs_info *ino)
{
	ino->uid = GLOBAL_ROOT_UID;
	ino->gid = GLOBAL_ROOT_GID;
	ino->last_used = jiffies;
}

void autofs_free_ino(struct autofs_info *ino)
{
	kfree(ino);
}

void autofs_kill_sb(struct super_block *sb)
{
	struct autofs_sb_info *sbi = autofs_sbi(sb);

	/*
	 * In the event of a failure in get_sb_nodev the superblock
	 * info is not present so nothing else has been setup, so
	 * just call kill_anon_super when we are called from
	 * deactivate_super.
	 */
	if (sbi) {
		/* Free wait queues, close pipe */
		autofs_catatonic_mode(sbi);
		put_pid(sbi->oz_pgrp);
	}

	pr_debug("shutting down\n");
	kill_litter_super(sb);
	if (sbi)
		kfree_rcu(sbi, rcu);
}

static int autofs_show_options(struct seq_file *m, struct dentry *root)
{
	struct autofs_sb_info *sbi = autofs_sbi(root->d_sb);
	struct inode *root_inode = d_inode(root->d_sb->s_root);

	if (!sbi)
		return 0;

	seq_printf(m, ",fd=%d", sbi->pipefd);
	if (!uid_eq(root_inode->i_uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			from_kuid_munged(&init_user_ns, root_inode->i_uid));
	if (!gid_eq(root_inode->i_gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			from_kgid_munged(&init_user_ns, root_inode->i_gid));
	seq_printf(m, ",pgrp=%d", pid_vnr(sbi->oz_pgrp));
	seq_printf(m, ",timeout=%lu", sbi->exp_timeout/HZ);
	seq_printf(m, ",minproto=%d", sbi->min_proto);
	seq_printf(m, ",maxproto=%d", sbi->max_proto);

	if (autofs_type_offset(sbi->type))
		seq_printf(m, ",offset");
	else if (autofs_type_direct(sbi->type))
		seq_printf(m, ",direct");
	else
		seq_printf(m, ",indirect");
#ifdef CONFIG_CHECKPOINT_RESTORE
	if (sbi->pipe)
		seq_printf(m, ",pipe_ino=%ld", file_inode(sbi->pipe)->i_ino);
	else
		seq_printf(m, ",pipe_ino=-1");
#endif
	return 0;
}

static void autofs_evict_inode(struct inode *inode)
{
	clear_inode(inode);
	kfree(inode->i_private);
}

static const struct super_operations autofs_sops = {
	.statfs		= simple_statfs,
	.show_options	= autofs_show_options,
	.evict_inode	= autofs_evict_inode,
};

struct autofs_fs_params {
	int pipefd;
	kuid_t uid;
	kgid_t gid;
	int pgrp;
	bool pgrp_set;
	int min_proto;
	int max_proto;
	unsigned int type;
};

enum {Opt_err, Opt_fd, Opt_uid, Opt_gid, Opt_pgrp, Opt_minproto, Opt_maxproto,
	Opt_indirect, Opt_direct, Opt_offset};

static const match_table_t tokens = {
	{Opt_fd, "fd=%u"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_pgrp, "pgrp=%u"},
	{Opt_minproto, "minproto=%u"},
	{Opt_maxproto, "maxproto=%u"},
	{Opt_indirect, "indirect"},
	{Opt_direct, "direct"},
	{Opt_offset, "offset"},
	{Opt_err, NULL}
};

static int autofs_parse_options(char *options, struct autofs_fs_params *params)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	kuid_t uid;
	kgid_t gid;

	if (!options)
		return 1;

	params->pipefd = -1;

	params->uid = current_uid();
	params->gid = current_gid();

	params->min_proto = AUTOFS_MIN_PROTO_VERSION;
	params->max_proto = AUTOFS_MAX_PROTO_VERSION;

	params->pgrp_set = false;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_fd:
			if (match_int(args, &option))
				return 1;
			params->pipefd = option;
			break;
		case Opt_uid:
			if (match_int(args, &option))
				return 1;
			uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(uid))
				return 1;
			params->uid = uid;
			break;
		case Opt_gid:
			if (match_int(args, &option))
				return 1;
			gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(gid))
				return 1;
			params->gid = gid;
			break;
		case Opt_pgrp:
			if (match_int(args, &option))
				return 1;
			params->pgrp = option;
			params->pgrp_set = true;
			break;
		case Opt_minproto:
			if (match_int(args, &option))
				return 1;
			params->min_proto = option;
			break;
		case Opt_maxproto:
			if (match_int(args, &option))
				return 1;
			params->max_proto = option;
			break;
		case Opt_indirect:
			set_autofs_type_indirect(&params->type);
			break;
		case Opt_direct:
			set_autofs_type_direct(&params->type);
			break;
		case Opt_offset:
			set_autofs_type_offset(&params->type);
			break;
		default:
			return 1;
		}
	}
	return (params->pipefd < 0);
}

static int autofs_apply_sbi_options(struct autofs_sb_info *sbi,
				    struct autofs_fs_params *params)
{
	int err;

	sbi->pipefd = params->pipefd;

	if (params->type)
		sbi->type = params->type;

	/* Test versions first */
	if (params->max_proto < AUTOFS_MIN_PROTO_VERSION ||
	    params->min_proto > AUTOFS_MAX_PROTO_VERSION) {
		pr_err("kernel does not match daemon version\n");
		pr_err("daemon (%d, %d) kernel (%d, %d)\n",
			params->min_proto, params->max_proto,
			AUTOFS_MIN_PROTO_VERSION, AUTOFS_MAX_PROTO_VERSION);
		goto out;
	}

	sbi->max_proto = params->max_proto;
	sbi->min_proto = params->min_proto;

	if (sbi->min_proto > sbi->max_proto)
		sbi->min_proto = params->max_proto;

	/* Establish highest kernel protocol version */
	if (sbi->max_proto > AUTOFS_MAX_PROTO_VERSION)
		sbi->version = AUTOFS_MAX_PROTO_VERSION;
	else
		sbi->version = params->max_proto;

	sbi->sub_version = AUTOFS_PROTO_SUBVERSION;

	if (!params->pgrp_set)
		sbi->oz_pgrp = get_task_pid(current, PIDTYPE_PGID);
	else {
		sbi->oz_pgrp = find_get_pid(params->pgrp);
		if (!sbi->oz_pgrp) {
			pr_err("could not find process group %d\n",
			       params->pgrp);
			goto out;
		}
	}

	pr_debug("pipe fd = %d, pgrp = %u\n",
		  sbi->pipefd, pid_nr(sbi->oz_pgrp));

	sbi->pipe = fget(sbi->pipefd);
	if (!sbi->pipe) {
		pr_err("could not open pipe file descriptor\n");
		goto out_put_pid;
	}

	err = autofs_prepare_pipe(sbi->pipe);
	if (err < 0)
		goto out_fput;

	sbi->catatonic = 0;

	return 0;

out_fput:
	fput(sbi->pipe);
out_put_pid:
	put_pid(sbi->oz_pgrp);
out:
	return 1;
}

static struct autofs_sb_info *autofs_alloc_sbi(struct super_block *s)
{
	struct autofs_sb_info *sbi;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return NULL;

	sbi->magic = AUTOFS_SBI_MAGIC;
	sbi->sb = s;
	sbi->pipefd = -1;
	sbi->pipe = NULL;
	sbi->catatonic = 1;
	set_autofs_type_indirect(&sbi->type);
	mutex_init(&sbi->wq_mutex);
	mutex_init(&sbi->pipe_mutex);
	spin_lock_init(&sbi->fs_lock);
	spin_lock_init(&sbi->lookup_lock);
	INIT_LIST_HEAD(&sbi->active_list);
	INIT_LIST_HEAD(&sbi->expiring_list);

	return sbi;
}

int autofs_fill_super(struct super_block *s, void *data, int silent)
{
	struct inode *root_inode;
	struct dentry *root;
	struct autofs_fs_params params;
	struct autofs_sb_info *sbi;
	struct autofs_info *ino;
	int ret = -EINVAL;

	sbi = autofs_alloc_sbi(s);
	if (!sbi)
		return -ENOMEM;

	pr_debug("starting up, sbi = %p\n", sbi);

	s->s_fs_info = sbi;
	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = AUTOFS_SUPER_MAGIC;
	s->s_op = &autofs_sops;
	s->s_d_op = &autofs_dentry_operations;
	s->s_time_gran = 1;

	/*
	 * Get the root inode and dentry, but defer checking for errors.
	 */
	ino = autofs_new_ino(sbi);
	if (!ino) {
		ret = -ENOMEM;
		goto fail_free;
	}
	root_inode = autofs_get_inode(s, S_IFDIR | 0755);
	if (!root_inode) {
		ret = -ENOMEM;
		goto fail_ino;
	}
	root = d_make_root(root_inode);
	if (!root)
		goto fail_iput;

	root->d_fsdata = ino;

	memset(&params, 0, sizeof(struct autofs_fs_params));
	if (autofs_parse_options(data, &params)) {
		pr_err("called with bogus options\n");
		goto fail_dput;
	}
	root_inode->i_uid = params->uid;
	root_inode->i_gid = params->gid;

	ret = autofs_apply_sbi_options(sbi, &params);
	if (ret)
		goto fail_dput;

	if (autofs_type_trigger(sbi->type))
		__managed_dentry_set_managed(root);

	root_inode->i_fop = &autofs_root_operations;
	root_inode->i_op = &autofs_dir_inode_operations;

	/*
	 * Success! Install the root dentry now to indicate completion.
	 */
	s->s_root = root;
	return 0;

	/*
	 * Failure ... clean up.
	 */
fail_dput:
	dput(root);
	goto fail_free;
fail_iput:
	iput(root_inode);
fail_ino:
	autofs_free_ino(ino);
fail_free:
	kfree(sbi);
	s->s_fs_info = NULL;
	return ret;
}

struct inode *autofs_get_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (inode == NULL)
		return NULL;

	inode->i_mode = mode;
	if (sb->s_root) {
		inode->i_uid = d_inode(sb->s_root)->i_uid;
		inode->i_gid = d_inode(sb->s_root)->i_gid;
	}
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_ino = get_next_ino();

	if (S_ISDIR(mode)) {
		set_nlink(inode, 2);
		inode->i_op = &autofs_dir_inode_operations;
		inode->i_fop = &autofs_dir_operations;
	} else if (S_ISLNK(mode)) {
		inode->i_op = &autofs_symlink_inode_operations;
	} else
		WARN_ON(1);

	return inode;
}
