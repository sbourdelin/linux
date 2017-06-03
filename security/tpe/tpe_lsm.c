/*
 * Trusted Path Execution Security Module
 *
 * Copyright 2017 Matt Brown
 *
 * Author: Matt Brown <matt@nmatt.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
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

#define TPE_GLOBAL_UID(x) from_kuid_munged(&init_user_ns, (x))
#define TPE_GLOBAL_GID(x) from_kgid_munged(&init_user_ns, (x))
#define global_root(x) uid_eq((x), GLOBAL_ROOT_UID)
#define global_nonroot(x) (!uid_eq((x), GLOBAL_ROOT_UID))
#define global_nonroot_gid(x) (!gid_eq((x), GLOBAL_ROOT_GID))

static int tpe_enabled __read_mostly = IS_ENABLED(CONFIG_SECURITY_TPE);
static kgid_t tpe_gid __read_mostly = KGIDT_INIT(CONFIG_SECURITY_TPE_GID);
static int tpe_all __read_mostly = IS_ENABLED(CONFIG_SECURITY_TPE_ALL);
static int tpe_invert __read_mostly = IS_ENABLED(CONFIG_SECURITY_TPE_INVERT);

int print_tpe_error(struct file *file, char *reason1, char *reason2)
{
	char *filepath;

	filepath = kstrdup_quotable_file(file, GFP_KERNEL);

	if (!filepath)
		return -ENOMEM;

	pr_warn_ratelimited("TPE: Denied execution of %s Reason: %s%s%s\n",
		(IS_ERR(filepath) ? "failed fetching file path" : filepath),
		reason1, reason2 ? " and " : "", reason2 ?: "");
	kfree(filepath);
	return -EPERM;
}

/*
 * Return 0 if the hook is successful and permission is granted.
 * Otherwise return the proper error message
 *
 */
static int tpe_bprm_set_creds(struct linux_binprm *bprm)
{
	struct file *file = bprm->file;
	struct inode *inode = d_backing_inode(file->f_path.dentry->d_parent);
	struct inode *file_inode = d_backing_inode(file->f_path.dentry);
	const struct cred *cred = current_cred();
	char *reason1 = NULL;
	char *reason2 = NULL;

	if (!tpe_enabled)
		return 0;

	/* never restrict root */
	if (global_root(cred->uid))
		return 0;

	if (!tpe_all)
		goto general_tpe_check;

	/* TPE_ALL: restrictions enforced even if the gid is trusted */
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
	if (tpe_invert && !in_group_p(tpe_gid))
		reason2 = "not in trusted group";
	else if (!tpe_invert && in_group_p(tpe_gid))
		reason2 = "in untrusted group";
	else
		return 0;

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
	if (reason1)
		return print_tpe_error(file, reason1, reason2);
	else
		return 0;
}

static struct security_hook_list tpe_hooks[] = {
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
		.procname	= "gid_invert",
		.data		= &tpe_invert,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "restrict_all",
		.data		= &tpe_all,
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
