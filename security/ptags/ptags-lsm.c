/*
 * Copyright (C) 2016 José Bollo <jobol@nonadev.net>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, version 2.
 *
 * Author:
 *      José Bollo <jobol@nonadev.net>
 */

#include <linux/types.h>

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/binfmts.h>
#include <linux/lsm_hooks.h>
#include <linux/printk.h>

#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
#include <linux/user_namespace.h>
#endif

#include "ptags.c"

#define set_ptags_of_cred(cred,value)	((cred)->ptags = value)
#define ptags_of_cred(cred)		((struct ptags*)((cred)->ptags))
#define ptags_of_bprm(bprm)		ptags_of_cred((bprm)->cred)
#define ptags_of_task(task)		((struct ptags*) \
						task_cred_xxx(task, ptags))
#define ptags_of_current()		((struct ptags*) \
						current_cred_xxx(ptags))

/**
 * ptags_is_ptags_file - Is 'name' of ptags entry?
 *
 * @name:	the name to test
 *
 * Returns 1 when 'name' is the ptags entry name
 * or, otherwise, returns 0.
 */
static int inline ptags_is_ptags_file(const char *name)
{
	return !strcmp(name, "ptags");
}

/**
 * ptags_bprm_committing_creds - Prepare to install the new credentials
 * from bprm.
 *
 * @bprm: binprm for exec
 */
static void ptags_bprm_committing_creds(struct linux_binprm *bprm)
{
	ptags_prune(ptags_of_bprm(bprm));
}

/**
 * ptags_cred_alloc_blank - "allocate" blank task-level security credentials
 * @new: the new credentials
 * @gfp: the atomicity of any memory allocations
 *
 * Prepare a blank set of credentials for modification.  This must allocate all
 * the memory the LSM module might require such that cred_transfer() can
 * complete without error.
 */
static int ptags_cred_alloc_blank(struct cred *cred, gfp_t gfp)
{
	struct ptags *root;

	root = ptags_create();
	set_ptags_of_cred(cred, root);
	return root ? 0 : -ENOMEM;
}

/**
 * ptags_cred_free - "free" task-level security credentials
 * @cred: the credentials in question
 *
 */
static void ptags_cred_free(struct cred *cred)
{
	struct ptags *root;

	root = ptags_of_cred(cred);
	set_ptags_of_cred(cred, NULL);
	ptags_free(root);
}

/**
 * ptags_cred_prepare - prepare new set of credentials for modification
 * @new: the new credentials
 * @old: the original credentials
 * @gfp: the atomicity of any memory allocations
 *
 * Prepare a new set of credentials for modification.
 */
static int ptags_cred_prepare(struct cred *new, const struct cred *old,
			      gfp_t gfp)
{
	int rc;

	rc = ptags_cred_alloc_blank(new, gfp);
	if (rc == 0)
		rc = ptags_copy(ptags_of_cred(new), ptags_of_cred(old));
	return rc;
}

/**
 * ptags_cred_transfer - Transfer the old credentials to the new credentials
 * @new: the new credentials
 * @old: the original credentials
 *
 * Fill in a set of blank credentials from another set of credentials.
 */
static void ptags_cred_transfer(struct cred *new, const struct cred *old)
{
	ptags_move(ptags_of_cred(new), ptags_of_cred(old));
}

/**
 * ptags_getprocattr - reads the file 'name' of the task 'task'
 * @task: the object task
 * @name: the name of the attribute in /proc/.../attr
 * @value: where to put the result
 *
 * Reads the ptags
 *
 * Returns the length read
 */
static int ptags_getprocattr(struct task_struct *task, char *name, char **value)
{
	if (ptags_is_ptags_file(name))
		return ptags_read(ptags_of_task(task), value);
	return 0;
}

/**
 * ptags_setprocattr - write the file 'name' of the task 'task
 *
 * @task: the object task
 * @name: the name of the attribute in /proc/.../attr
 * @value: the value to set
 * @size: the size of the value
 *
 * Sets ptags
 *
 * Returns the length writen data
 */
static int ptags_setprocattr(struct task_struct *task, char *name,
			     void *value, size_t size)
{
	struct ptags *croot;
	if (ptags_is_ptags_file(name)) {
		if ((current->flags & PF_KTHREAD) ||
#ifdef CONFIG_SECURITY_PTAGS_WITH_USER_NS
		    has_ns_capability(task, task_cred_xxx(task, user_ns), CAP_MAC_ADMIN)
#else
		    has_capability(task, CAP_MAC_ADMIN)
#endif
		    )
			croot = NULL;
		else
			croot = ptags_of_current();
		return ptags_write(croot, ptags_of_task(task), value, size);
	}
	return 0;
}

/*
 * List of hooks
 */
static struct security_hook_list ptags_hooks[] = {

	LSM_HOOK_INIT(bprm_committing_creds, ptags_bprm_committing_creds),

	LSM_HOOK_INIT(cred_alloc_blank, ptags_cred_alloc_blank),
	LSM_HOOK_INIT(cred_free, ptags_cred_free),
	LSM_HOOK_INIT(cred_prepare, ptags_cred_prepare),
	LSM_HOOK_INIT(cred_transfer, ptags_cred_transfer),

	LSM_HOOK_INIT(getprocattr, ptags_getprocattr),
	LSM_HOOK_INIT(setprocattr, ptags_setprocattr),
};

/**
 * ptags_init - initialize the tags system
 *
 * Returns 0
 */
static __init int ptags_init(void)
{
	int rc;

	pr_info("PTags:  Initialising.\n");

	/* Set the tags for the initial task. */
	rc = ptags_cred_alloc_blank((struct cred *)current->cred, GFP_KERNEL);
	if (rc != 0)
		return -ENOMEM;

	/* Register with LSM */
	security_add_hooks(ptags_hooks, ARRAY_SIZE(ptags_hooks));

	return 0;
}

/*
 * Smack requires early initialization in order to label
 * all processes and objects when they are created.
 */
security_initcall(ptags_init);
