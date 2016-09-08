/*
 * Dummy stubs used when CONFIG_POSIX_TIMERS=n
 *
 * Created by:  Nicolas Pitre, July 2016
 * Copyright:   (C) 2016 Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/errno.h>
#include <linux/posix-timers.h>

asmlinkage long sys_ni_posix_timers(void)
{
	pr_err_once("process %d (%s) attempted a POSIX timer syscall "
		    "while CONFIG_POSIX_TIMERS is not set\n",
		    current->pid, current->comm);
	return -ENOSYS;
}

#define SYS_NI(name)  SYSCALL_ALIAS(sys_##name, sys_ni_posix_timers)

SYS_NI(timer_create);
SYS_NI(timer_gettime);
SYS_NI(timer_getoverrun);
SYS_NI(timer_settime);
SYS_NI(timer_delete);
SYS_NI(clock_settime);
SYS_NI(clock_gettime);
SYS_NI(clock_adjtime);
SYS_NI(clock_getres);
SYS_NI(clock_nanosleep);

void do_schedule_next_timer(struct siginfo *info)
{
}

void exit_itimers(struct signal_struct *sig)
{
}

void posix_timers_register_clock(const clockid_t clock_id,
				 struct k_clock *new_clock)
{
}

int posix_timer_event(struct k_itimer *timr, int si_private)
{
	return 0;
}

void run_posix_cpu_timers(struct task_struct *tsk)
{
}

void posix_cpu_timers_exit(struct task_struct *tsk)
{
	add_device_randomness((const void*) &tsk->se.sum_exec_runtime,
			      sizeof(unsigned long long));
}

void posix_cpu_timers_exit_group(struct task_struct *tsk)
{
}

void set_process_cpu_timer(struct task_struct *tsk, unsigned int clock_idx,
			   cputime_t *newval, cputime_t *oldval)
{
}

void update_rlimit_cpu(struct task_struct *task, unsigned long rlim_new)
{
}

void thread_group_cputimer(struct task_struct *tsk, struct task_cputime *times)
{
}
