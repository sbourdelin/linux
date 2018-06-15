// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 Linaro Limited
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * The idle injection framework proposes a way to force a cpu to enter
 * an idle state during a specified amount of time for a specified
 * period.
 *
 * It relies on the smpboot kthreads which handles, via its main loop,
 * the common code for hotplugging and [un]parking.
 *
 * At init time, all the kthreads are created.
 *
 * A cpumask is specified as parameter for the idle injection
 * registering function. The kthreads will be synchronized regarding
 * this cpumask.
 *
 * The idle + run duration is specified via the helpers and then the
 * idle injection can be started at this point.
 *
 * A kthread will call play_idle() with the specified idle duration
 * from above.
 *
 * A timer is set after waking up all the tasks, to the next idle
 * injection cycle.
 *
 * The task handling the timer interrupt will wakeup all the kthreads
 * belonging to the cpumask.
 *
 * Stopping the idle injection is synchonuous, when the function
 * returns, there is the guarantee there is no more idle injection
 * kthread in activity.
 *
 * It is up to the user of this framework to provide a lock at an
 * upper level to prevent stupid things to happen, like starting while
 * we are unregistering.
 */
#define pr_fmt(fmt) "ii_dev: " fmt

#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smpboot.h>

#include <uapi/linux/sched/types.h>

/**
 * struct idle_injection_thread - task on/off switch structure
 * @tsk: a pointer to a task_struct injecting the idle cycles
 * @should_run: a integer used as a boolean by the smpboot kthread API
 */
struct idle_injection_thread {
	struct task_struct *tsk;
	int should_run;
};

/**
 * struct idle_injection_device - data for the idle injection
 * @timer: a hrtimer giving the tempo for the idle injection
 * @idle_duration_ms: an unsigned int specifying the idle duration
 * @run_duration_ms: an unsigned int specifying the running duration
 * @cpumask: a cpumask containing the list of CPUs managed by the device
 */
struct idle_injection_device {
	struct hrtimer timer;
	unsigned int idle_duration_ms;
	unsigned int run_duration_ms;
	unsigned long int cpumask[0];
};

static DEFINE_PER_CPU(struct idle_injection_thread, idle_injection_thread);
static DEFINE_PER_CPU(struct idle_injection_device *, idle_injection_device);

/**
 * idle_injection_wakeup - Wake up all idle injection threads
 * @ii_dev: the idle injection device
 *
 * Every idle injection task belonging to the idle injection device
 * and running on an online CPU will be wake up by this call.
 */
static void idle_injection_wakeup(struct idle_injection_device *ii_dev)
{
	struct idle_injection_thread *iit;
	unsigned int cpu;

	for_each_cpu_and(cpu, to_cpumask(ii_dev->cpumask), cpu_online_mask) {
		iit = per_cpu_ptr(&idle_injection_thread, cpu);
		iit->should_run = 1;
		wake_up_process(iit->tsk);
	}
}

/**
 * idle_injection_wakeup_fn - idle injection timer callback
 * @timer: a hrtimer structure
 *
 * This function is called when the idle injection timer expires which
 * will wake up the idle injection tasks and these ones, in turn, play
 * idle a specified amount of time.
 *
 * Return: HRTIMER_RESTART.
 */
static enum hrtimer_restart idle_injection_wakeup_fn(struct hrtimer *timer)
{
	unsigned int run_duration_ms;
	unsigned int idle_duration_ms;
	struct idle_injection_device *ii_dev =
		container_of(timer, struct idle_injection_device, timer);

	run_duration_ms = READ_ONCE(ii_dev->run_duration_ms);
	idle_duration_ms = READ_ONCE(ii_dev->idle_duration_ms);

	idle_injection_wakeup(ii_dev);

	hrtimer_forward_now(timer,
			    ms_to_ktime(idle_duration_ms + run_duration_ms));

	return HRTIMER_RESTART;
}

/**
 * idle_injection_fn - idle injection routine
 * @cpu: the CPU number the task belongs to
 *
 * The idle injection routine will stay idle the specified amount of
 * time
 */
static void idle_injection_fn(unsigned int cpu)
{
	struct idle_injection_device *ii_dev;
	struct idle_injection_thread *iit;

	ii_dev = per_cpu(idle_injection_device, cpu);
	iit = per_cpu_ptr(&idle_injection_thread, cpu);

	/*
	 * Boolean used by the smpboot main loop and used as a
	 * flip-flop in this function
	 */
	iit->should_run = 0;

	play_idle(READ_ONCE(ii_dev->idle_duration_ms));
}

/**
 * idle_injection_set_duration - idle and run duration helper
 * @run_duration_ms: an unsigned int giving the running time in milliseconds
 * @idle_duration_ms: an unsigned int giving the idle time in milliseconds
 */
void idle_injection_set_duration(struct idle_injection_device *ii_dev,
				 unsigned int run_duration_ms,
				 unsigned int idle_duration_ms)
{
	if (run_duration_ms && idle_duration_ms) {
		WRITE_ONCE(ii_dev->run_duration_ms, run_duration_ms);
		WRITE_ONCE(ii_dev->idle_duration_ms, idle_duration_ms);
	}
}

/**
 * idle_injection_get_duration - idle and run duration helper
 * @run_duration_ms: a pointer to an unsigned int to store the running time
 * @idle_duration_ms: a pointer to an unsigned int to store the idle time
 */
void idle_injection_get_duration(struct idle_injection_device *ii_dev,
				 unsigned int *run_duration_ms,
				 unsigned int *idle_duration_ms)
{
	*run_duration_ms = READ_ONCE(ii_dev->run_duration_ms);
	*idle_duration_ms = READ_ONCE(ii_dev->idle_duration_ms);
}

/**
 * idle_injection_start - starts the idle injections
 * @ii_dev: a pointer to an idle_injection_device structure
 *
 * The function starts the idle injection cycles by first waking up
 * all the tasks the ii_dev is attached to and let them handle the
 * idle-run periods.
 *
 * Return: -EINVAL if the idle or the running durations are not set.
 */
int idle_injection_start(struct idle_injection_device *ii_dev)
{
	unsigned int idle_duration_ms = READ_ONCE(ii_dev->idle_duration_ms);
	unsigned int run_duration_ms = READ_ONCE(ii_dev->run_duration_ms);

	if (!idle_duration_ms || !run_duration_ms)
		return -EINVAL;

	pr_debug("Starting injecting idle cycles on CPUs '%*pbl'\n",
		 cpumask_pr_args(to_cpumask(ii_dev->cpumask)));

	idle_injection_wakeup(ii_dev);

	hrtimer_start(&ii_dev->timer,
		      ms_to_ktime(idle_duration_ms + run_duration_ms),
		      HRTIMER_MODE_REL);

	return 0;
}

/**
 * idle_injection_stop - stops the idle injections
 * @ii_dev: a pointer to an idle injection_device structure
 *
 * The function stops the idle injection and waits for the threads to
 * complete. If we are in the process of injecting an idle cycle, then
 * this will wait the end of the cycle.
 *
 * When the function returns there is no more idle injection
 * activity. The kthreads are scheduled out and the periodic timer is
 * off.
 */
void idle_injection_stop(struct idle_injection_device *ii_dev)
{
	struct idle_injection_thread *iit;
	unsigned int cpu;

	pr_debug("Stopping injecting idle cycles on CPUs '%*pbl'\n",
		 cpumask_pr_args(to_cpumask(ii_dev->cpumask)));

	hrtimer_cancel(&ii_dev->timer);

	/*
	 * We want the guarantee we have a quescient point where
	 * parked threads stay in there state while we are stopping
	 * the idle injection. After exiting the loop, if any CPU is
	 * plugged in, the 'should_run' boolean being false, the
	 * smpboot main loop schedules the task out.
	 */
	cpu_hotplug_disable();

	for_each_cpu_and(cpu, to_cpumask(ii_dev->cpumask), cpu_online_mask) {
		iit = per_cpu_ptr(&idle_injection_thread, cpu);
		iit->should_run = 0;

		wait_task_inactive(iit->tsk, 0);
	}

	cpu_hotplug_enable();
}

/**
 * idle_injection_setup - initialize the current task as a RT task
 * @cpu: the CPU number where the kthread is running on (not used)
 *
 * Called one time, this function is in charge of setting the task
 * scheduler parameters.
 */
static void idle_injection_setup(unsigned int cpu)
{
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };

	set_freezable();

	sched_setscheduler(current, SCHED_FIFO, &param);
}

/**
 * idle_injection_should_run - function helper for the smpboot API
 * @cpu: the CPU number where the kthread is running on
 *
 * Return: a boolean telling if the thread can run.
 */
static int idle_injection_should_run(unsigned int cpu)
{
	struct idle_injection_thread *iit =
		per_cpu_ptr(&idle_injection_thread, cpu);

	return iit->should_run;
}

/**
 * idle_injection_register - idle injection init routine
 * @cpumask: the list of CPUs managed by the idle injection device
 *
 * This is the initialization function in charge of creating the
 * initializing of the timer and allocate the structures. It does not
 * starts the idle injection cycles.
 *
 * Return: NULL if an allocation fails.
 */
struct idle_injection_device *idle_injection_register(struct cpumask *cpumask)
{
	struct idle_injection_device *ii_dev;
	int cpu;

	ii_dev = kzalloc(sizeof(*ii_dev) + cpumask_size(), GFP_KERNEL);
	if (!ii_dev)
		return NULL;

	cpumask_copy(to_cpumask(ii_dev->cpumask), cpumask);
	hrtimer_init(&ii_dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ii_dev->timer.function = idle_injection_wakeup_fn;

	for_each_cpu(cpu, to_cpumask(ii_dev->cpumask)) {

		if (per_cpu(idle_injection_device, cpu)) {
			pr_err("cpu%d is already registered\n", cpu);
			goto out_rollback_per_cpu;
		}

		per_cpu(idle_injection_device, cpu) = ii_dev;
	}

	return ii_dev;

out_rollback_per_cpu:
	for_each_cpu(cpu, to_cpumask(ii_dev->cpumask))
		per_cpu(idle_injection_device, cpu) = NULL;

	kfree(ii_dev);

	return NULL;
}

/**
 * idle_injection_unregister - Unregister the idle injection device
 * @ii_dev: a pointer to an idle injection device
 *
 * The function is in charge of stopping the idle injections,
 * unregister the kthreads and free the allocated memory in the
 * register function.
 */
void idle_injection_unregister(struct idle_injection_device *ii_dev)
{
	unsigned int cpu;

	idle_injection_stop(ii_dev);

	for_each_cpu(cpu, to_cpumask(ii_dev->cpumask))
		per_cpu(idle_injection_device, cpu) = NULL;

	kfree(ii_dev);
}

static struct smp_hotplug_thread idle_injection_threads = {
	.store = &idle_injection_thread.tsk,
	.setup = idle_injection_setup,
	.thread_fn = idle_injection_fn,
	.thread_comm = "idle_inject/%u",
	.thread_should_run = idle_injection_should_run,
};

static int __init idle_injection_init(void)
{
	return smpboot_register_percpu_thread(&idle_injection_threads);
}
early_initcall(idle_injection_init);
