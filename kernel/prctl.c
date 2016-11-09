#include <linux/sched.h>
#include <linux/cn_proc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/prctl.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include <asm/processor.h>

#ifndef SET_UNALIGN_CTL
# define SET_UNALIGN_CTL(a, b)	(-EINVAL)
#endif
#ifndef GET_UNALIGN_CTL
# define GET_UNALIGN_CTL(a, b)	(-EINVAL)
#endif
#ifndef SET_FPEMU_CTL
# define SET_FPEMU_CTL(a, b)	(-EINVAL)
#endif
#ifndef GET_FPEMU_CTL
# define GET_FPEMU_CTL(a, b)	(-EINVAL)
#endif
#ifndef SET_FPEXC_CTL
# define SET_FPEXC_CTL(a, b)	(-EINVAL)
#endif
#ifndef GET_FPEXC_CTL
# define GET_FPEXC_CTL(a, b)	(-EINVAL)
#endif
#ifndef GET_ENDIAN
# define GET_ENDIAN(a, b)	(-EINVAL)
#endif
#ifndef SET_ENDIAN
# define SET_ENDIAN(a, b)	(-EINVAL)
#endif
#ifndef GET_TSC_CTL
# define GET_TSC_CTL(a)		(-EINVAL)
#endif
#ifndef SET_TSC_CTL
# define SET_TSC_CTL(a)		(-EINVAL)
#endif
#ifndef MPX_ENABLE_MANAGEMENT
# define MPX_ENABLE_MANAGEMENT()	(-EINVAL)
#endif
#ifndef MPX_DISABLE_MANAGEMENT
# define MPX_DISABLE_MANAGEMENT()	(-EINVAL)
#endif
#ifndef GET_FP_MODE
# define GET_FP_MODE(a)		(-EINVAL)
#endif
#ifndef SET_FP_MODE
# define SET_FP_MODE(a,b)	(-EINVAL)
#endif

static int prctl_set_mm_exe_file(struct mm_struct *mm, unsigned int fd)
{
	struct fd exe;
	struct file *old_exe, *exe_file;
	struct inode *inode;
	int err;

	exe = fdget(fd);
	if (!exe.file)
		return -EBADF;

	inode = file_inode(exe.file);

	/*
	 * Because the original mm->exe_file points to executable file, make
	 * sure that this one is executable as well, to avoid breaking an
	 * overall picture.
	 */
	err = -EACCES;
	if (!S_ISREG(inode->i_mode) || path_noexec(&exe.file->f_path))
		goto exit;

	err = inode_permission(inode, MAY_EXEC);
	if (err)
		goto exit;

	/*
	 * Forbid mm->exe_file change if old file still mapped.
	 */
	exe_file = get_mm_exe_file(mm);
	err = -EBUSY;
	if (exe_file) {
		struct vm_area_struct *vma;

		down_read(&mm->mmap_sem);
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			if (!vma->vm_file)
				continue;
			if (path_equal(&vma->vm_file->f_path,
				       &exe_file->f_path))
				goto exit_err;
		}

		up_read(&mm->mmap_sem);
		fput(exe_file);
	}

	/*
	 * The symlink can be changed only once, just to disallow arbitrary
	 * transitions malicious software might bring in. This means one
	 * could make a snapshot over all processes running and monitor
	 * /proc/pid/exe changes to notice unusual activity if needed.
	 */
	err = -EPERM;
	if (test_and_set_bit(MMF_EXE_FILE_CHANGED, &mm->flags))
		goto exit;

	err = 0;
	/* set the new file, lockless */
	get_file(exe.file);
	old_exe = xchg(&mm->exe_file, exe.file);
	if (old_exe)
		fput(old_exe);
exit:
	fdput(exe);
	return err;
exit_err:
	up_read(&mm->mmap_sem);
	fput(exe_file);
	goto exit;
}

/*
 * WARNING: we don't require any capability here so be very careful
 * in what is allowed for modification from userspace.
 */
static int validate_prctl_map(struct prctl_mm_map *prctl_map)
{
	unsigned long mmap_max_addr = TASK_SIZE;
	struct mm_struct *mm = current->mm;
	int error = -EINVAL, i;

	static const unsigned char offsets[] = {
		offsetof(struct prctl_mm_map, start_code),
		offsetof(struct prctl_mm_map, end_code),
		offsetof(struct prctl_mm_map, start_data),
		offsetof(struct prctl_mm_map, end_data),
		offsetof(struct prctl_mm_map, start_brk),
		offsetof(struct prctl_mm_map, brk),
		offsetof(struct prctl_mm_map, start_stack),
		offsetof(struct prctl_mm_map, arg_start),
		offsetof(struct prctl_mm_map, arg_end),
		offsetof(struct prctl_mm_map, env_start),
		offsetof(struct prctl_mm_map, env_end),
	};

	/*
	 * Make sure the members are not somewhere outside
	 * of allowed address space.
	 */
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		u64 val = *(u64 *)((char *)prctl_map + offsets[i]);

		if ((unsigned long)val >= mmap_max_addr ||
		    (unsigned long)val < mmap_min_addr)
			goto out;
	}

	/*
	 * Make sure the pairs are ordered.
	 */
#define __prctl_check_order(__m1, __op, __m2)				\
	((unsigned long)prctl_map->__m1 __op				\
	 (unsigned long)prctl_map->__m2) ? 0 : -EINVAL
	error  = __prctl_check_order(start_code, <, end_code);
	error |= __prctl_check_order(start_data, <, end_data);
	error |= __prctl_check_order(start_brk, <=, brk);
	error |= __prctl_check_order(arg_start, <=, arg_end);
	error |= __prctl_check_order(env_start, <=, env_end);
	if (error)
		goto out;
#undef __prctl_check_order

	error = -EINVAL;

	/*
	 * @brk should be after @end_data in traditional maps.
	 */
	if (prctl_map->start_brk <= prctl_map->end_data ||
	    prctl_map->brk <= prctl_map->end_data)
		goto out;

	/*
	 * Neither we should allow to override limits if they set.
	 */
	if (check_data_rlimit(rlimit(RLIMIT_DATA), prctl_map->brk,
			      prctl_map->start_brk, prctl_map->end_data,
			      prctl_map->start_data))
			goto out;

	/*
	 * Someone is trying to cheat the auxv vector.
	 */
	if (prctl_map->auxv_size) {
		if (!prctl_map->auxv || prctl_map->auxv_size > sizeof(mm->saved_auxv))
			goto out;
	}

	/*
	 * Finally, make sure the caller has the rights to
	 * change /proc/pid/exe link: only local root should
	 * be allowed to.
	 */
	if (prctl_map->exe_fd != (u32)-1) {
		struct user_namespace *ns = current_user_ns();
		const struct cred *cred = current_cred();

		if (!uid_eq(cred->uid, make_kuid(ns, 0)) ||
		    !gid_eq(cred->gid, make_kgid(ns, 0)))
			goto out;
	}

	error = 0;
out:
	return error;
}

#ifdef CONFIG_CHECKPOINT_RESTORE
static int prctl_set_mm_map(int opt, const void __user *addr, unsigned long data_size)
{
	struct prctl_mm_map prctl_map = { .exe_fd = (u32)-1, };
	unsigned long user_auxv[AT_VECTOR_SIZE];
	struct mm_struct *mm = current->mm;
	int error;

	BUILD_BUG_ON(sizeof(user_auxv) != sizeof(mm->saved_auxv));
	BUILD_BUG_ON(sizeof(struct prctl_mm_map) > 256);

	if (opt == PR_SET_MM_MAP_SIZE)
		return put_user((unsigned int)sizeof(prctl_map),
				(unsigned int __user *)addr);

	if (data_size != sizeof(prctl_map))
		return -EINVAL;

	if (copy_from_user(&prctl_map, addr, sizeof(prctl_map)))
		return -EFAULT;

	error = validate_prctl_map(&prctl_map);
	if (error)
		return error;

	if (prctl_map.auxv_size) {
		memset(user_auxv, 0, sizeof(user_auxv));
		if (copy_from_user(user_auxv,
				   (const void __user *)prctl_map.auxv,
				   prctl_map.auxv_size))
			return -EFAULT;

		/* Last entry must be AT_NULL as specification requires */
		user_auxv[AT_VECTOR_SIZE - 2] = AT_NULL;
		user_auxv[AT_VECTOR_SIZE - 1] = AT_NULL;
	}

	if (prctl_map.exe_fd != (u32)-1) {
		error = prctl_set_mm_exe_file(mm, prctl_map.exe_fd);
		if (error)
			return error;
	}

	down_write(&mm->mmap_sem);

	/*
	 * We don't validate if these members are pointing to
	 * real present VMAs because application may have correspond
	 * VMAs already unmapped and kernel uses these members for statistics
	 * output in procfs mostly, except
	 *
	 *  - @start_brk/@brk which are used in do_brk but kernel lookups
	 *    for VMAs when updating these memvers so anything wrong written
	 *    here cause kernel to swear at userspace program but won't lead
	 *    to any problem in kernel itself
	 */

	mm->start_code	= prctl_map.start_code;
	mm->end_code	= prctl_map.end_code;
	mm->start_data	= prctl_map.start_data;
	mm->end_data	= prctl_map.end_data;
	mm->start_brk	= prctl_map.start_brk;
	mm->brk		= prctl_map.brk;
	mm->start_stack	= prctl_map.start_stack;
	mm->arg_start	= prctl_map.arg_start;
	mm->arg_end	= prctl_map.arg_end;
	mm->env_start	= prctl_map.env_start;
	mm->env_end	= prctl_map.env_end;

	/*
	 * Note this update of @saved_auxv is lockless thus
	 * if someone reads this member in procfs while we're
	 * updating -- it may get partly updated results. It's
	 * known and acceptable trade off: we leave it as is to
	 * not introduce additional locks here making the kernel
	 * more complex.
	 */
	if (prctl_map.auxv_size)
		memcpy(mm->saved_auxv, user_auxv, sizeof(user_auxv));

	up_write(&mm->mmap_sem);
	return 0;
}
#endif /* CONFIG_CHECKPOINT_RESTORE */

static int prctl_set_auxv(struct mm_struct *mm, unsigned long addr,
			  unsigned long len)
{
	/*
	 * This doesn't move the auxiliary vector itself since it's pinned to
	 * mm_struct, but it permits filling the vector with new values.  It's
	 * up to the caller to provide sane values here, otherwise userspace
	 * tools which use this vector might be unhappy.
	 */
	unsigned long user_auxv[AT_VECTOR_SIZE];

	if (len > sizeof(user_auxv))
		return -EINVAL;

	if (copy_from_user(user_auxv, (const void __user *)addr, len))
		return -EFAULT;

	/* Make sure the last entry is always AT_NULL */
	user_auxv[AT_VECTOR_SIZE - 2] = 0;
	user_auxv[AT_VECTOR_SIZE - 1] = 0;

	BUILD_BUG_ON(sizeof(user_auxv) != sizeof(mm->saved_auxv));

	task_lock(current);
	memcpy(mm->saved_auxv, user_auxv, len);
	task_unlock(current);

	return 0;
}

static int prctl_set_mm(int opt, unsigned long addr,
			unsigned long arg4, unsigned long arg5)
{
	struct mm_struct *mm = current->mm;
	struct prctl_mm_map prctl_map;
	struct vm_area_struct *vma;
	int error;

	if (arg5 || (arg4 && (opt != PR_SET_MM_AUXV &&
			      opt != PR_SET_MM_MAP &&
			      opt != PR_SET_MM_MAP_SIZE)))
		return -EINVAL;

#ifdef CONFIG_CHECKPOINT_RESTORE
	if (opt == PR_SET_MM_MAP || opt == PR_SET_MM_MAP_SIZE)
		return prctl_set_mm_map(opt, (const void __user *)addr, arg4);
#endif

	if (!capable(CAP_SYS_RESOURCE))
		return -EPERM;

	if (opt == PR_SET_MM_EXE_FILE)
		return prctl_set_mm_exe_file(mm, (unsigned int)addr);

	if (opt == PR_SET_MM_AUXV)
		return prctl_set_auxv(mm, addr, arg4);

	if (addr >= TASK_SIZE || addr < mmap_min_addr)
		return -EINVAL;

	error = -EINVAL;

	down_write(&mm->mmap_sem);
	vma = find_vma(mm, addr);

	prctl_map.start_code	= mm->start_code;
	prctl_map.end_code	= mm->end_code;
	prctl_map.start_data	= mm->start_data;
	prctl_map.end_data	= mm->end_data;
	prctl_map.start_brk	= mm->start_brk;
	prctl_map.brk		= mm->brk;
	prctl_map.start_stack	= mm->start_stack;
	prctl_map.arg_start	= mm->arg_start;
	prctl_map.arg_end	= mm->arg_end;
	prctl_map.env_start	= mm->env_start;
	prctl_map.env_end	= mm->env_end;
	prctl_map.auxv		= NULL;
	prctl_map.auxv_size	= 0;
	prctl_map.exe_fd	= -1;

	switch (opt) {
	case PR_SET_MM_START_CODE:
		prctl_map.start_code = addr;
		break;
	case PR_SET_MM_END_CODE:
		prctl_map.end_code = addr;
		break;
	case PR_SET_MM_START_DATA:
		prctl_map.start_data = addr;
		break;
	case PR_SET_MM_END_DATA:
		prctl_map.end_data = addr;
		break;
	case PR_SET_MM_START_STACK:
		prctl_map.start_stack = addr;
		break;
	case PR_SET_MM_START_BRK:
		prctl_map.start_brk = addr;
		break;
	case PR_SET_MM_BRK:
		prctl_map.brk = addr;
		break;
	case PR_SET_MM_ARG_START:
		prctl_map.arg_start = addr;
		break;
	case PR_SET_MM_ARG_END:
		prctl_map.arg_end = addr;
		break;
	case PR_SET_MM_ENV_START:
		prctl_map.env_start = addr;
		break;
	case PR_SET_MM_ENV_END:
		prctl_map.env_end = addr;
		break;
	default:
		goto out;
	}

	error = validate_prctl_map(&prctl_map);
	if (error)
		goto out;

	switch (opt) {
	/*
	 * If command line arguments and environment
	 * are placed somewhere else on stack, we can
	 * set them up here, ARG_START/END to setup
	 * command line argumets and ENV_START/END
	 * for environment.
	 */
	case PR_SET_MM_START_STACK:
	case PR_SET_MM_ARG_START:
	case PR_SET_MM_ARG_END:
	case PR_SET_MM_ENV_START:
	case PR_SET_MM_ENV_END:
		if (!vma) {
			error = -EFAULT;
			goto out;
		}
	}

	mm->start_code	= prctl_map.start_code;
	mm->end_code	= prctl_map.end_code;
	mm->start_data	= prctl_map.start_data;
	mm->end_data	= prctl_map.end_data;
	mm->start_brk	= prctl_map.start_brk;
	mm->brk		= prctl_map.brk;
	mm->start_stack	= prctl_map.start_stack;
	mm->arg_start	= prctl_map.arg_start;
	mm->arg_end	= prctl_map.arg_end;
	mm->env_start	= prctl_map.env_start;
	mm->env_end	= prctl_map.env_end;

	error = 0;
out:
	up_write(&mm->mmap_sem);
	return error;
}

#ifdef CONFIG_CHECKPOINT_RESTORE
static int prctl_get_tid_address(struct task_struct *me, int __user **tid_addr)
{
	return put_user(me->clear_child_tid, tid_addr);
}
#else
static int prctl_get_tid_address(struct task_struct *me, int __user **tid_addr)
{
	return -EINVAL;
}
#endif

SYSCALL_DEFINE5(prctl, int, option, unsigned long, arg2, unsigned long, arg3,
		unsigned long, arg4, unsigned long, arg5)
{
	struct task_struct *me = current;
	unsigned char comm[sizeof(me->comm)];
	long error;

	error = security_task_prctl(option, arg2, arg3, arg4, arg5);
	if (error != -ENOSYS)
		return error;

	error = 0;
	switch (option) {
	case PR_SET_PDEATHSIG:
		if (!valid_signal(arg2)) {
			error = -EINVAL;
			break;
		}
		me->pdeath_signal = arg2;
		break;
	case PR_GET_PDEATHSIG:
		error = put_user(me->pdeath_signal, (int __user *)arg2);
		break;
	case PR_GET_DUMPABLE:
		error = get_dumpable(me->mm);
		break;
	case PR_SET_DUMPABLE:
		if (arg2 != SUID_DUMP_DISABLE && arg2 != SUID_DUMP_USER) {
			error = -EINVAL;
			break;
		}
		set_dumpable(me->mm, arg2);
		break;

	case PR_SET_UNALIGN:
		error = SET_UNALIGN_CTL(me, arg2);
		break;
	case PR_GET_UNALIGN:
		error = GET_UNALIGN_CTL(me, arg2);
		break;
	case PR_SET_FPEMU:
		error = SET_FPEMU_CTL(me, arg2);
		break;
	case PR_GET_FPEMU:
		error = GET_FPEMU_CTL(me, arg2);
		break;
	case PR_SET_FPEXC:
		error = SET_FPEXC_CTL(me, arg2);
		break;
	case PR_GET_FPEXC:
		error = GET_FPEXC_CTL(me, arg2);
		break;
	case PR_GET_TIMING:
		error = PR_TIMING_STATISTICAL;
		break;
	case PR_SET_TIMING:
		if (arg2 != PR_TIMING_STATISTICAL)
			error = -EINVAL;
		break;
	case PR_SET_NAME:
		comm[sizeof(me->comm) - 1] = 0;
		if (strncpy_from_user(comm, (char __user *)arg2,
				      sizeof(me->comm) - 1) < 0)
			return -EFAULT;
		set_task_comm(me, comm);
		proc_comm_connector(me);
		break;
	case PR_GET_NAME:
		get_task_comm(comm, me);
		if (copy_to_user((char __user *)arg2, comm, sizeof(comm)))
			return -EFAULT;
		break;
	case PR_GET_ENDIAN:
		error = GET_ENDIAN(me, arg2);
		break;
	case PR_SET_ENDIAN:
		error = SET_ENDIAN(me, arg2);
		break;
	case PR_GET_SECCOMP:
		error = prctl_get_seccomp();
		break;
	case PR_SET_SECCOMP:
		error = prctl_set_seccomp(arg2, (char __user *)arg3);
		break;
	case PR_GET_TSC:
		error = GET_TSC_CTL(arg2);
		break;
	case PR_SET_TSC:
		error = SET_TSC_CTL(arg2);
		break;
	case PR_TASK_PERF_EVENTS_DISABLE:
		error = perf_event_task_disable();
		break;
	case PR_TASK_PERF_EVENTS_ENABLE:
		error = perf_event_task_enable();
		break;
	case PR_GET_TIMERSLACK:
		if (current->timer_slack_ns > ULONG_MAX)
			error = ULONG_MAX;
		else
			error = current->timer_slack_ns;
		break;
	case PR_SET_TIMERSLACK:
		if (arg2 <= 0)
			current->timer_slack_ns =
					current->default_timer_slack_ns;
		else
			current->timer_slack_ns = arg2;
		break;
	case PR_MCE_KILL:
		if (arg4 | arg5)
			return -EINVAL;
		switch (arg2) {
		case PR_MCE_KILL_CLEAR:
			if (arg3 != 0)
				return -EINVAL;
			current->flags &= ~PF_MCE_PROCESS;
			break;
		case PR_MCE_KILL_SET:
			current->flags |= PF_MCE_PROCESS;
			if (arg3 == PR_MCE_KILL_EARLY)
				current->flags |= PF_MCE_EARLY;
			else if (arg3 == PR_MCE_KILL_LATE)
				current->flags &= ~PF_MCE_EARLY;
			else if (arg3 == PR_MCE_KILL_DEFAULT)
				current->flags &=
						~(PF_MCE_EARLY|PF_MCE_PROCESS);
			else
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
		break;
	case PR_MCE_KILL_GET:
		if (arg2 | arg3 | arg4 | arg5)
			return -EINVAL;
		if (current->flags & PF_MCE_PROCESS)
			error = (current->flags & PF_MCE_EARLY) ?
				PR_MCE_KILL_EARLY : PR_MCE_KILL_LATE;
		else
			error = PR_MCE_KILL_DEFAULT;
		break;
	case PR_SET_MM:
		error = prctl_set_mm(arg2, arg3, arg4, arg5);
		break;
	case PR_GET_TID_ADDRESS:
		error = prctl_get_tid_address(me, (int __user **)arg2);
		break;
	case PR_SET_CHILD_SUBREAPER:
		me->signal->is_child_subreaper = !!arg2;
		break;
	case PR_GET_CHILD_SUBREAPER:
		error = put_user(me->signal->is_child_subreaper,
				 (int __user *)arg2);
		break;
	case PR_SET_NO_NEW_PRIVS:
		if (arg2 != 1 || arg3 || arg4 || arg5)
			return -EINVAL;

		task_set_no_new_privs(current);
		break;
	case PR_GET_NO_NEW_PRIVS:
		if (arg2 || arg3 || arg4 || arg5)
			return -EINVAL;
		return task_no_new_privs(current) ? 1 : 0;
	case PR_GET_THP_DISABLE:
		if (arg2 || arg3 || arg4 || arg5)
			return -EINVAL;
		error = !!(me->mm->def_flags & VM_NOHUGEPAGE);
		break;
	case PR_SET_THP_DISABLE:
		if (arg3 || arg4 || arg5)
			return -EINVAL;
		if (down_write_killable(&me->mm->mmap_sem))
			return -EINTR;
		if (arg2)
			me->mm->def_flags |= VM_NOHUGEPAGE;
		else
			me->mm->def_flags &= ~VM_NOHUGEPAGE;
		up_write(&me->mm->mmap_sem);
		break;
	case PR_MPX_ENABLE_MANAGEMENT:
		if (arg2 || arg3 || arg4 || arg5)
			return -EINVAL;
		error = MPX_ENABLE_MANAGEMENT();
		break;
	case PR_MPX_DISABLE_MANAGEMENT:
		if (arg2 || arg3 || arg4 || arg5)
			return -EINVAL;
		error = MPX_DISABLE_MANAGEMENT();
		break;
	case PR_SET_FP_MODE:
		error = SET_FP_MODE(me, arg2);
		break;
	case PR_GET_FP_MODE:
		error = GET_FP_MODE(me);
		break;
	default:
		error = -EINVAL;
		break;
	}
	return error;
}

