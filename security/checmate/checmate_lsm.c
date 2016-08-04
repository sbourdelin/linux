/*
 * Checmate Linux Security Module
 *
 * Copyright (C) 2016 Sargun Dhillon <sargun@sargun.me>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 */

#include <linux/prctl.h>
#include <linux/checmate.h>
#include <linux/lsm_hooks.h>
#include <linux/mutex.h>
#include <linux/bpf.h>
#include <linux/filter.h>

#define HOOK_LIST_INIT(HOOK_NUM) \
	LIST_HEAD_INIT(checmate_bpf_hooks[HOOK_NUM].hook_list)

#define CHECMATE_HOOK(HOOK_NUM)				\
	[HOOK_NUM] =					\
	{						\
		.enabled = true,			\
		.hook_list = HOOK_LIST_INIT(HOOK_NUM),	\
	}

void register_checmate_prog_ops(void);

/*
 * Global write lock for all BPF program hook manipulation. This shouldn't
 * see much contention, as installation / reset / deny_reset are rare
 * operations.
 */
static DEFINE_MUTEX(checmate_write_lock);

struct checmate_bpf_hook {
	bool			enabled;
	bool			deny_reset;
	struct list_head	hook_list;
};

struct checmate_bpf_hook_instance {
	struct list_head	list;
	struct bpf_prog		*prog;
};

/* This is the internal array of the heads of BPF hooks */
static struct checmate_bpf_hook checmate_bpf_hooks[__CHECMATE_HOOK_MAX] = {
	CHECMATE_HOOK(CHECMATE_HOOK_FILE_OPEN),
	CHECMATE_HOOK(CHECMATE_HOOK_TASK_CREATE),
	CHECMATE_HOOK(CHECMATE_HOOK_TASK_FREE),
#ifdef CONFIG_SECURITY_NETWORK
	CHECMATE_HOOK(CHECMATE_HOOK_SOCKET_CONNECT),
#endif /* CONFIG_SECURITY_NETWORK */
};

/*
 * checmate_task_prctl_install_hook - Install a checmate hook
 * @hook: Hook ID
 * @prog_fd: BPF prog fd
 *
 * Return 0 on success, return -ve on error
 */
static int checmate_prctl_install_hook(int hook, int prog_fd)
{
	int rc = 0;
	struct bpf_prog *prog;
	struct checmate_bpf_hook_instance *hook_instance;

	prog = bpf_prog_get_type(prog_fd, BPF_PROG_TYPE_CHECMATE);
	if (IS_ERR(prog))
		return PTR_ERR(prog);

	mutex_lock(&checmate_write_lock);
	list_for_each_entry(hook_instance,
			    &checmate_bpf_hooks[hook].hook_list, list) {
		if (hook_instance->prog == prog) {
			rc = -EEXIST;
			goto err;
		}
	}
	hook_instance = kmalloc(sizeof(*hook_instance), GFP_KERNEL);

	if (!hook_instance) {
		rc = -ENOMEM;
		goto err;
	}
	hook_instance->prog = prog;
	list_add_tail_rcu(&hook_instance->list,
			  &checmate_bpf_hooks[hook].hook_list);
	mutex_unlock(&checmate_write_lock);
	return rc;

err:
	mutex_unlock(&checmate_write_lock);
	bpf_prog_put(prog);
	return rc;
}

/*
 * checmate_prctl_deny_reset - Set deny bit on hook
 * @hook: The Hook ID
 *
 * Return 0 or -EALREADY on success, to indicate the deny bit was set
 */
static int checmate_prctl_deny_reset(int hook)
{
	int rc = 0;

	mutex_lock(&checmate_write_lock);
	if (checmate_bpf_hooks[hook].deny_reset)
		rc = -EALREADY;
	else
		checmate_bpf_hooks[hook].deny_reset = true;
	mutex_unlock(&checmate_write_lock);

	return rc;
}

/*
 * checmate_reset - Reset (disassociate) the BPF programs for a checmate hook
 * @hook: Hook ID
 *
 * Return 0 on success, -ve on error.
 */
static int checmate_reset(int hook)
{
	int rc = 0;
	struct checmate_bpf_hook_instance *hook_instance, *next;

	mutex_lock(&checmate_write_lock);
	if (checmate_bpf_hooks[hook].deny_reset) {
		rc = -EPERM;
		goto out;
	}
	list_for_each_entry_safe(hook_instance, next,
				 &checmate_bpf_hooks[hook].hook_list, list) {
		list_del_rcu(&hook_instance->list);
		synchronize_rcu();
		bpf_prog_put(hook_instance->prog);
		kfree(hook_instance);
	}
out:
	mutex_unlock(&checmate_write_lock);
	return rc;
}

/* checmate_task_prctl_op - Run a checmate specific prctl operation
 * @op - Used to specify the Checmate operation ID
 * @hook - Hook ID
 * @ufd - BPF Program user file descriptor
 * @arg5 - Unused
 *
 * Return 0 on success, -ve on error. -EINVAL when option unhandled.
 */

static int checmate_task_prctl_op(unsigned long op, unsigned long hook,
				  unsigned long ufd, unsigned long arg5)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!(hook > 0 && hook < __CHECMATE_HOOK_MAX))
		return -EINVAL;
	if (!checmate_bpf_hooks[hook].enabled)
		return -ENOENT;

	if (op == CHECMATE_INSTALL_HOOK)
		return checmate_prctl_install_hook(hook, ufd);
	else if (op == CHECMATE_DENY_RESET)
		return checmate_prctl_deny_reset(hook);
	else if (op == CHECMATE_RESET)
		return checmate_reset(hook);

	return -EINVAL;
}

/*
 * checmate_task_prctl - check for Checmate-specific prctl operations
 * @option: If PR_CHECMATE, passes to handler
 * @arg2:
 * @arg3:
 * @arg4:
 * @arg5:
 *
 * Return 0 on success, -ve on error.  -ENOSYS is returned when checmate
 * does not handle the given option.
 */
static int checmate_task_prctl(int option, unsigned long arg2,
			       unsigned long arg3, unsigned long arg4,
			       unsigned long arg5)
{
	if (option == PR_CHECMATE)
		return checmate_task_prctl_op(arg2, arg3, arg4, arg5);
	return -ENOSYS;
}

/*
 * call_bpf_int_hook - Run all the BPF programs associated with a hook
 * @hook: The Hook ID
 * @ctx: The context which is passed to the hook
 *
 * Return 0 on success, on first hook erroring, the error is returned
 * to the caller
 *
 * Requires that the context struct is populated before passing, but
 * the actual ctx->hook is set inside this function
 */
static int call_bpf_int_hook(int hook, struct checmate_ctx *ctx)
{
	int rc = 0;
	struct checmate_bpf_hook_instance *hook_instance;

	ctx->hook = hook;

	preempt_disable();
	rcu_read_lock();
	list_for_each_entry_rcu(hook_instance,
				&checmate_bpf_hooks[hook].hook_list, list) {
		rc = BPF_PROG_RUN(hook_instance->prog, (void *)ctx);
		if (rc != 0)
			goto out;
	}
out:
	rcu_read_unlock();
	preempt_enable();
	return rc;
}

/*
 * call_bpf_void_hook - Run all the BPF programs associated with a hook
 * @hook: The Hook ID
 * @ctx: The context which is passed to the hook
 *
 * Return 0 on success, on first hook erroring, the error is returned
 * to the caller
 *
 * Requires that the context struct is populated before passing, but
 * the actual ctx->hook is set inside this function
 */
static void call_bpf_void_hook(int hook, struct checmate_ctx *ctx)
{
	call_bpf_int_hook(hook, ctx);
}

/* Checmate hooks */
static int checmate_file_open(struct file *file, const struct cred *cred)
{
	struct checmate_ctx ctx;

	ctx.file_open_ctx.file = file;
	ctx.file_open_ctx.cred = cred;
	return call_bpf_int_hook(CHECMATE_HOOK_FILE_OPEN, &ctx);
}

static int checmate_task_create(unsigned long clone_flags)
{
	struct checmate_ctx ctx;

	ctx.task_create_ctx.clone_flags = clone_flags;
	return call_bpf_int_hook(CHECMATE_HOOK_TASK_CREATE, &ctx);
}

static void checmate_task_free(struct task_struct *task)
{
	struct checmate_ctx ctx;

	ctx.task_free_ctx.task = task;
	call_bpf_void_hook(CHECMATE_HOOK_TASK_FREE, &ctx);
}

#ifdef CONFIG_SECURITY_NETWORK
static int checmate_socket_connect(struct socket *sock,
				   struct sockaddr *address, int addrlen)
{
	struct checmate_ctx ctx;

	ctx.socket_connect_ctx.sock = sock;
	ctx.socket_connect_ctx.address = address;
	ctx.socket_connect_ctx.addrlen = addrlen;
	return call_bpf_int_hook(CHECMATE_HOOK_SOCKET_CONNECT, &ctx);
}

#endif /* CONFIG_SECURITY_NETWORK */

static struct security_hook_list checmate_hooks[] = {
	LSM_HOOK_INIT(task_prctl, checmate_task_prctl),
	LSM_HOOK_INIT(file_open, checmate_file_open),
	LSM_HOOK_INIT(task_create, checmate_task_create),
	LSM_HOOK_INIT(task_free, checmate_task_free),
#ifdef CONFIG_SECURITY_NETWORK
	LSM_HOOK_INIT(socket_connect, checmate_socket_connect),
#endif /* CONFIG_SECURITY_NETWORK */
};

static int __init checmate_setup(void)
{
	pr_info("Checmate activating.\n");
	register_checmate_prog_ops();
	security_add_hooks(checmate_hooks, ARRAY_SIZE(checmate_hooks));
	return 0;
}
late_initcall(checmate_setup);
