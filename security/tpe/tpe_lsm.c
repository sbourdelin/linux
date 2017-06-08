/*
 * Trusted Path Execution Security Module
 *
 * Copyright (C) 2017 Matt Brown
 * Copyright (C) 2001-2014 Bradley Spengler, Open Source Security, Inc.
 * http://www.grsecurity.net spender@grsecurity.net
 * Copyright (C) 2011 Corey Henderson
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#include <linux/kernel.h>
#include <linux/uidgid.h>
#include <linux/ratelimit.h>
#include <linux/limits.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/lsm_hooks.h>
#include <linux/sysctl.h>
#include <linux/binfmts.h>
#include <linux/string_helpers.h>
#include <linux/dcache.h>
#include <uapi/asm-generic/mman-common.h>

#define global_root(x) uid_eq((x), GLOBAL_ROOT_UID)
#define global_nonroot(x) (!uid_eq((x), GLOBAL_ROOT_UID))
#define global_root_gid(x) (gid_eq((x), GLOBAL_ROOT_GID))
#define global_nonroot_gid(x) (!gid_eq((x), GLOBAL_ROOT_GID))

static int tpe_enabled __read_mostly = IS_ENABLED(CONFIG_SECURITY_TPE);
static kgid_t tpe_gid __read_mostly = KGIDT_INIT(CONFIG_SECURITY_TPE_GID);
static int tpe_invert_gid __read_mostly =
	IS_ENABLED(CONFIG_SECURITY_TPE_INVERT_GID);
static int tpe_strict __read_mostly = IS_ENABLED(CONFIG_SECURITY_TPE_STRICT);
static int tpe_restrict_root __read_mostly =
	IS_ENABLED(CONFIG_SECURITY_TPE_RESTRICT_ROOT);

int print_tpe_error(struct file *file, char *reason1, char *reason2,
	char *method)
{
	char *filepath;

	filepath = kstrdup_quotable_file(file, GFP_KERNEL);

	if (!filepath)
		return -ENOMEM;

	pr_warn_ratelimited("TPE: Denied %s of %s Reason: %s%s%s\n", method,
		(IS_ERR(filepath) ? "failed fetching file path" : filepath),
		reason1, reason2 ? " and " : "", reason2 ?: "");
	kfree(filepath);
	return -EPERM;
}

static int tpe_check(struct file *file, char *method)
{
	struct inode *inode;
	struct inode *file_inode;
	struct dentry *dir;
	const struct cred *cred = current_cred();
	char *reason1 = NULL;
	char *reason2 = NULL;

	dir = dget_parent(file->f_path.dentry);
	inode = d_backing_inode(dir);
	file_inode = d_backing_inode(file->f_path.dentry);

	if (!tpe_enabled)
		return 0;

	/* never restrict root unless restrict_root sysctl is 1*/
	if (global_root(cred->uid) && !tpe_restrict_root)
		return 0;

	if (!tpe_strict)
		goto general_tpe_check;

	/* TPE_STRICT: restrictions enforced even if the gid is trusted */
	if (global_nonroot(inode->i_uid) && !uid_eq(inode->i_uid, cred->uid))
		reason1 = "directory not owned by user";
	else if (inode->i_mode & 0002)
		reason1 = "file in world-writable directory";
	else if ((inode->i_mode & 0020) && global_nonroot_gid(inode->i_gid))
		reason1 = "file in group-writable directory";
	else if (file_inode->i_mode & 0002)
		reason1 = "file is world-writable";

	if (reason1)
		goto end;

general_tpe_check:
	/* determine if group is trusted */
	if (global_root_gid(tpe_gid))
		goto next_check;
	if (!tpe_invert_gid && !in_group_p(tpe_gid))
		reason2 = "not in trusted group";
	else if (tpe_invert_gid && in_group_p(tpe_gid))
		reason2 = "in untrusted group";
	else
		return 0;

next_check:
	/* main TPE checks */
	if (global_nonroot(inode->i_uid))
		reason1 = "file in non-root-owned directory";
	else if (inode->i_mode & 0002)
		reason1 = "file in world-writable directory";
	else if ((inode->i_mode & 0020) && global_nonroot_gid(inode->i_gid))
		reason1 = "file in group-writable directory";
	else if (file_inode->i_mode & 0002)
		reason1 = "file is world-writable";

end:
	dput(dir);
	if (reason1)
		return print_tpe_error(file, reason1, reason2, method);
	else
		return 0;
}

int tpe_mmap_file(struct file *file, unsigned long reqprot,
	unsigned long prot, unsigned long flags)
{
	if (!file || !(prot & PROT_EXEC))
		return 0;

	return tpe_check(file, "mmap");
}

int tpe_file_mprotect(struct vm_area_struct *vma, unsigned long reqprot,
	unsigned long prot)
{
	if (!vma->vm_file)
		return 0;
	return tpe_check(vma->vm_file, "mprotect");
}

static int tpe_bprm_set_creds(struct linux_binprm *bprm)
{
	if (!bprm->file)
		return 0;
	return tpe_check(bprm->file, "exec");

}

static struct security_hook_list tpe_hooks[] = {
	LSM_HOOK_INIT(mmap_file, tpe_mmap_file),
	LSM_HOOK_INIT(file_mprotect, tpe_file_mprotect),
	LSM_HOOK_INIT(bprm_set_creds, tpe_bprm_set_creds),
};

#ifdef CONFIG_SYSCTL
struct ctl_path tpe_sysctl_path[] = {
	{ .procname = "kernel", },
	{ .procname = "tpe", },
	{ }
};

static struct ctl_table tpe_sysctl_table[] = {
	{
		.procname	= "enabled",
		.data		= &tpe_enabled,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "gid",
		.data		= &tpe_gid,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "invert_gid",
		.data		= &tpe_invert_gid,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "strict",
		.data		= &tpe_strict,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "restrict_root",
		.data		= &tpe_restrict_root,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{ }
};
static void __init tpe_init_sysctl(void)
{
	if (!register_sysctl_paths(tpe_sysctl_path, tpe_sysctl_table))
		panic("TPE: sysctl registration failed.\n");
}
#else
static inline void tpe_init_sysctl(void) { }
#endif /* CONFIG_SYSCTL */

void __init tpe_add_hooks(void)
{
	pr_info("TPE: securing systems like it's 1998\n");
	security_add_hooks(tpe_hooks, ARRAY_SIZE(tpe_hooks), "tpe");
	tpe_init_sysctl();
}
