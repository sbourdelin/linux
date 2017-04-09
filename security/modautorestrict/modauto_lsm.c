/*
 * ModAutoRestrict Linux Security Module
 *
 * Author: Djalal Harouni
 *
 * Copyright (C) 2017 Djalal Harouni
 * Copyright (C) 2017 Endocode AG.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 */

#include <linux/errno.h>
#include <linux/lsm_hooks.h>
#include <linux/prctl.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sysctl.h>

enum {
	MOD_AUTOLOAD_ALLOWED	= 0,
	MOD_AUTOLOAD_PRIVILEGED	= 1,
	MOD_AUTOLOAD_DENIED	= 2,
};

struct modautoload_task {
	bool usage;
	u8 flags;
};

static int autoload_restrict;

static int zero;
static int max_autoload_restrict = MOD_AUTOLOAD_DENIED;

/* Index number of per "struct task_struct" blob for ModAutoRestrict. */
u16 modautorestrict_task_security_index __ro_after_init;

static inline int modautoload_task_set_flag(struct modautoload_task *modtask,
					    unsigned long value)
{
	int ret = 0;

	if (value > MOD_AUTOLOAD_DENIED)
		ret = -EINVAL;
	else if (modtask->flags > value)
		ret = -EPERM;
	else if (modtask->flags < value)
		modtask->flags = value;

	return ret;
}

static inline struct modautoload_task *modautoload_task_security(struct task_struct *tsk)
{
	struct modautoload_task *modtask;

	modtask = task_security(tsk, modautorestrict_task_security_index);
	if (modtask->usage)
		return modtask;

	return NULL;
}

static inline struct modautoload_task *init_modautoload_task(struct task_struct *tsk,
							     unsigned long flags)
{
	struct modautoload_task *modtask;

	modtask = task_security(tsk, modautorestrict_task_security_index);

	modtask->flags = (u8)flags;
	modtask->usage = true;

	return modtask;
}

static inline void clear_modautoload_task(struct task_struct *tsk)
{
	struct modautoload_task *modtask;

	modtask = modautoload_task_security(tsk);
	if (modtask) {
		modtask->usage = false;
		modtask->flags = MOD_AUTOLOAD_ALLOWED;
	}
}

/*
 * Return 0 if CAP_SYS_MODULE or if CAP_NET_ADMIN and the module is
 * a netdev-%s module. Otherwise -EPERM is returned.
 */
static int modautoload_privileged_access(const char *name)
{
	int ret = -EPERM;

	if (capable(CAP_SYS_MODULE))
		ret = 0;
	else if (name && strstr(name, "netdev-") && capable(CAP_NET_ADMIN))
		ret = 0;

	return ret;
}

static int modautoload_sysctl_perm(unsigned long op, const char *name)
{
	int ret = -EINVAL;
	struct mm_struct *mm = NULL;

	if (op != PR_GET_MOD_AUTO_RESTRICT)
		return ret;

	switch (autoload_restrict) {
	case MOD_AUTOLOAD_ALLOWED:
		ret = 0;
		break;
	case MOD_AUTOLOAD_PRIVILEGED:
		/*
		 * Are we allowed to sleep here ?
		 * Also improve this check here
		 */
		ret = -EPERM;
		mm = get_task_mm(current);
		if (mm) {
			ret = modautoload_privileged_access(name);
			mmput(mm);
		}
		break;
	case MOD_AUTOLOAD_DENIED:
		ret = -EPERM;
		break;
	default:
		break;
	}

	return ret;
}

static int modautoload_task_perm(struct modautoload_task *mtask,
				 char *kmod_name)
{
	int ret = -EINVAL;

	switch (mtask->flags) {
	case MOD_AUTOLOAD_ALLOWED:
		ret = 0;
		break;
	case MOD_AUTOLOAD_PRIVILEGED:
		ret = modautoload_privileged_access(kmod_name);
		break;
	case MOD_AUTOLOAD_DENIED:
		ret = -EPERM;
		break;
	default:
		break;
	}

	return ret;
}

/* Set the given option in a modautorestrict task */
static int modautoload_set_op_value(struct task_struct *tsk,
				    unsigned long value)
{
	int ret = -EINVAL;
	struct modautoload_task *modtask;

	if (value > MOD_AUTOLOAD_DENIED)
		return ret;

	modtask = modautoload_task_security(tsk);
	if (!modtask) {
		modtask = init_modautoload_task(tsk, value);
		return 0;
	}

	return modautoload_task_set_flag(modtask, value);
}

static int modautoload_get_op_value(struct task_struct *tsk)
{
	struct modautoload_task *modtask;

	modtask = modautoload_task_security(tsk);
	if (!modtask)
		return -EINVAL;

	return modtask->flags;
}

/* Copy modautorestrict context from parent to child */
int modautoload_task_alloc(struct task_struct *tsk, unsigned long clone_flags)
{
	struct modautoload_task *modparent;

	modparent = modautoload_task_security(current);

	/* Parent has a modautorestrict context */
	if (modparent)
		init_modautoload_task(tsk, modparent->flags);

	return 0;
}

/*
 * Return 0 on success, -error on error.  -ENOSYS is returned when modautorestrict
 * does not handle the given option, or -EINVAL if the passed arguments are not
 * valid.
 */
int modautoload_task_prctl(int option, unsigned long arg2, unsigned long arg3,
			   unsigned long arg4, unsigned long arg5)
{
	int ret = -EINVAL;
	struct task_struct *myself = current;

	if (option != PR_MOD_AUTO_RESTRICT_OPTS)
		return -ENOSYS;

	get_task_struct(myself);

	switch (arg2) {
	case PR_SET_MOD_AUTO_RESTRICT:
		if (arg4 || arg5)
			goto out;

		ret = modautoload_set_op_value(myself, arg3);
		break;
	case PR_GET_MOD_AUTO_RESTRICT:
		if (arg3 || arg4 || arg5)
			goto out;

		ret = modautoload_get_op_value(myself);
		break;
	default:
		break;
	}

out:
	put_task_struct(myself);
	return ret;
}

void modautoload_task_free(struct task_struct *tsk)
{
	clear_modautoload_task(tsk);
}

/*
 * TODO:
 * if this is covered entirely by CAP_SYS_MODULE then we should removed it.
 */
static int modautoload_kernel_module_file(struct file *file)
{
	int ret = 0;
	struct modautoload_task *modtask;
	struct task_struct *myself = current;

	/* First check if the task allows that */
	modtask = modautoload_task_security(myself);
	if (modtask) {
		ret = modautoload_task_perm(modtask, NULL);
		if (ret < 0)
			return ret;
	}

	return modautoload_sysctl_perm(PR_GET_MOD_AUTO_RESTRICT, NULL);
}

static int modautoload_kernel_module_request(char *kmod_name)
{
	int ret = 0;
	struct modautoload_task *modtask;
	struct task_struct *myself = current;

	/* First check if the task allows that */
	modtask = modautoload_task_security(myself);
	if (modtask) {
		ret = modautoload_task_perm(modtask, kmod_name);
		if (ret < 0)
			return ret;
	}

	return modautoload_sysctl_perm(PR_GET_MOD_AUTO_RESTRICT, kmod_name);
}

/*
 * TODO:
 * if this is covered entirely by CAP_SYS_MODULE then we should removed it.
 */
static int modautoload_kernel_read_file(struct file *file,
					enum kernel_read_file_id id)
{
	int ret = 0;

	switch (id) {
	case READING_MODULE:
		ret = modautoload_kernel_module_file(file);
		break;
	default:
		break;
	}

	return ret;
}

static struct security_hook_list modautoload_hooks[] = {
	LSM_HOOK_INIT(kernel_module_request, modautoload_kernel_module_request),
	LSM_HOOK_INIT(kernel_read_file, modautoload_kernel_read_file),
	LSM_HOOK_INIT(task_alloc, modautoload_task_alloc),
	LSM_HOOK_INIT(task_prctl, modautoload_task_prctl),
	LSM_HOOK_INIT(task_free, modautoload_task_free),
};

#ifdef CONFIG_SYSCTL
static int modautoload_dointvec_minmax(struct ctl_table *table, int write,
				       void __user *buffer, size_t *lenp,
				       loff_t *ppos)
{
	struct ctl_table table_copy;

	if (write && !capable(CAP_SYS_MODULE))
		return -EPERM;

	table_copy = *table;
	if (*(int *)table_copy.data == *(int *)table_copy.extra2)
		table_copy.extra1 = table_copy.extra2;

	return proc_dointvec_minmax(&table_copy, write, buffer, lenp, ppos);
}

struct ctl_path modautoload_sysctl_path[] = {
	{ .procname = "kernel", },
	{ .procname = "modautorestrict", },
	{ }
};

static struct ctl_table modautoload_sysctl_table[] = {
	{
		.procname       = "autoload",
		.data           = &autoload_restrict,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = modautoload_dointvec_minmax,
		.extra1         = &zero,
		.extra2         = &max_autoload_restrict,
	},
	{ }
};

static void __init modautoload_init_sysctl(void)
{
	if (!register_sysctl_paths(modautoload_sysctl_path, modautoload_sysctl_table))
		panic("modautorestrict: sysctl registration failed.\n");
}
#else
static inline void modautoload_init_sysctl(void) { }
#endif /* CONFIG_SYSCTL */

void __init modautorestrict_init(void)
{
	modautorestrict_task_security_index =
		security_reserve_task_blob_index(sizeof(struct modautoload_task));
	security_add_hooks(modautoload_hooks,
			   ARRAY_SIZE(modautoload_hooks), "modautorestrict");

	modautoload_init_sysctl();
	pr_info("ModAutoRestrict LSM:  Initialized\n");
}
