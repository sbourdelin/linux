/*
 *  linux/drivers/thermal/cpu_idle_cooling.c
 *
 *  Copyright (C) 2017  Tao Wang <kevin.wangtao@hisilicon.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/cpu.h>
#include <linux/topology.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/cpuidle.h>
#include <linux/thermal.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/wait.h>
#include <linux/sched/rt.h>

#define MAX_TARGET_RATIO		(50U)

#define DEFAULT_WINDOW_SIZE		(1)
#define DEFAULT_DURATION_JIFFIES	(20)

struct cpu_idle_cooling_device {
	int id;
	struct thermal_cooling_device *cooling_dev;
	wait_queue_head_t wait_queue;

	/* The cpu assigned to collect stat and update
	 * control parameters. default to BSP but BSP
	 * can be offlined.
	 */
	unsigned long control_cpu;

	unsigned int set_target_ratio;
	unsigned int current_ratio;
	unsigned int control_ratio;
	unsigned int duration;
	unsigned int window_size;

	cpumask_var_t related_cpus;
	cpumask_var_t injected_cpus;
	struct list_head node;
	bool should_skip;
	bool clamping;
};

static LIST_HEAD(cpu_idle_cooling_dev_list);
static DEFINE_PER_CPU(struct task_struct *, idle_injection_thread_ptr);
static DEFINE_MUTEX(cpu_idle_cooling_lock);

unsigned long idle_time[NR_CPUS] = {0};
unsigned long time_stamp[NR_CPUS] = {0};
static enum cpuhp_state hp_state;

#define STORE_PARAM(param, min, max)			\
static ssize_t store_##param(struct device *dev,	\
	struct device_attribute *attr,			\
	const char *buf, size_t count)			\
{									\
	unsigned int new_value;						\
	struct thermal_cooling_device *cdev;				\
	struct cpu_idle_cooling_device *idle_cooling_dev;		\
									\
	if (dev == NULL || attr == NULL)				\
		return 0;						\
									\
	if (kstrtouint(buf, 10, &new_value))				\
		return -EINVAL;						\
									\
	if (new_value > max || new_value < min) {			\
		pr_err("Out of range %u, between %d-%d\n",		\
			new_value, min, max);				\
		return -EINVAL;						\
	}								\
									\
	cdev = container_of(dev, struct thermal_cooling_device, device);\
	idle_cooling_dev = cdev->devdata;				\
	idle_cooling_dev->param = new_value;				\
									\
	/* make new value visible to other cpus */			\
	smp_mb();							\
									\
	return count;							\
}

STORE_PARAM(duration, 10, 500);
STORE_PARAM(window_size, 1, 10);

#define SHOW_PARAM(param)				\
static ssize_t show_##param(struct device *dev,		\
	struct device_attribute *attr, char *buf)	\
{									\
	struct thermal_cooling_device *cdev;				\
	struct cpu_idle_cooling_device *idle_cooling_dev;		\
									\
	if (dev == NULL || attr == NULL)				\
		return 0;						\
									\
	cdev = container_of(dev, struct thermal_cooling_device, device);\
	idle_cooling_dev = cdev->devdata;				\
									\
	return snprintf(buf, 12UL, "%d\n",				\
					idle_cooling_dev->param);	\
}

SHOW_PARAM(duration);
SHOW_PARAM(window_size);

static DEVICE_ATTR(duration, 0644, show_duration, store_duration);
static DEVICE_ATTR(window_size, 0644, show_window_size, store_window_size);

static struct cpu_idle_cooling_device *
get_cpu_idle_cooling_dev(unsigned long cpu)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	list_for_each_entry(idle_cooling_dev,
		&cpu_idle_cooling_dev_list, node) {
		if (cpumask_test_cpu(cpu, idle_cooling_dev->related_cpus))
			return idle_cooling_dev;
	}

	return NULL;
}

#define K_P		10
#define MAX_COMP	10
static unsigned int get_compensation(unsigned int current_ratio,
		unsigned int target_ratio, unsigned int control_ratio)
{
	unsigned int comp;

	comp = abs(current_ratio - target_ratio) * K_P / 10;
	if (comp > MAX_COMP)
		comp = MAX_COMP;

	if (current_ratio > target_ratio) {
		if (control_ratio > comp)
			comp = control_ratio - comp;
		else
			comp = 1;
	} else {
		if (control_ratio + comp < MAX_TARGET_RATIO)
			comp = control_ratio + comp;
		else
			comp = MAX_TARGET_RATIO;

		if (comp > (target_ratio * 6 / 5))
			comp = target_ratio * 6 / 5;
	}

	return comp;
}

static void update_stats(struct cpu_idle_cooling_device *idle_cooling_dev)
{
	unsigned long cpu;
	u64 now, now_idle, delta_time, delta_idle;
	u64 min_idle_ratio = 100;
	u64 idle_ratio = 0;

	for_each_cpu(cpu, idle_cooling_dev->related_cpus) {
		now_idle = get_cpu_idle_time(cpu, &now, 0);
		delta_idle = now_idle - idle_time[cpu];
		delta_time = now - time_stamp[cpu];
		idle_time[cpu] = now_idle;
		time_stamp[cpu] = now;

		if (delta_idle >= delta_time || !cpu_online(cpu))
			now_idle = 100;
		else if (delta_time)
			now_idle = div64_u64(100 * delta_idle, delta_time);
		else
			return;

		if (now_idle < min_idle_ratio)
			min_idle_ratio = now_idle;

		idle_ratio += now_idle;
	}

	idle_ratio /= cpumask_weight(idle_cooling_dev->related_cpus);
	if (idle_ratio > MAX_TARGET_RATIO)
		idle_ratio = min_idle_ratio;

	if (idle_cooling_dev->should_skip)
		idle_ratio = (idle_cooling_dev->current_ratio + idle_ratio) / 2;

	idle_cooling_dev->current_ratio = (unsigned int)idle_ratio;
	idle_cooling_dev->control_ratio = get_compensation(idle_ratio,
				idle_cooling_dev->set_target_ratio,
				idle_cooling_dev->control_ratio);
	idle_cooling_dev->should_skip =
			(idle_ratio > (2 * idle_cooling_dev->set_target_ratio));
	/* make new control_ratio and should skip flag visible to other cpus */
	smp_mb();
}

static void inject_idle_fn(struct cpu_idle_cooling_device *idle_cooling_dev)
{
	long sleeptime, guard;
	unsigned int interval_ms; /* jiffies to sleep for each attempt */
	unsigned long target_jiffies;
	unsigned int duration_ms = idle_cooling_dev->duration;
	unsigned long duration_jiffies = msecs_to_jiffies(duration_ms);

	guard = DIV_ROUND_UP(duration_jiffies * (90 - MAX_TARGET_RATIO), 100);

	/* align idle time */
	target_jiffies = roundup(jiffies, duration_jiffies);
	sleeptime = target_jiffies - jiffies;
	if (sleeptime < guard)
		sleeptime += duration_jiffies;

	if (sleeptime > 0)
		schedule_timeout_interruptible(sleeptime);

	interval_ms = duration_ms * idle_cooling_dev->control_ratio / 100;

	if (idle_cooling_dev->should_skip)
		return;

	if (interval_ms)
		play_idle(interval_ms);
}

static int idle_injection_thread(void *arg)
{
	unsigned long cpunr = (unsigned long)arg;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO/2 };
	unsigned int count = 0;
	struct cpu_idle_cooling_device *idle_cooling_dev;

	set_freezable();

	sched_setscheduler(current, SCHED_FIFO, &param);

	mutex_lock(&cpu_idle_cooling_lock);
	idle_cooling_dev = get_cpu_idle_cooling_dev(cpunr);
	mutex_unlock(&cpu_idle_cooling_lock);

	while (!kthread_should_stop()) {
		wait_event_interruptible(idle_cooling_dev->wait_queue,
			(idle_cooling_dev->clamping && cpu_online(cpunr)) ||
			kthread_should_stop());

		if (kthread_should_stop())
			break;

		/* rebind thread to cpu */
		if (set_cpus_allowed_ptr(current, cpumask_of(cpunr)))
			continue;

		try_to_freeze();

		while (idle_cooling_dev->clamping &&
			cpu_online(cpunr)) {
			try_to_freeze();

			count++;
			/*
			 * only elected controlling cpu can collect stats
			 * and update control parameters.
			 */
			if (cpunr == idle_cooling_dev->control_cpu
				&& !(count % idle_cooling_dev->window_size))
				update_stats(idle_cooling_dev);

			inject_idle_fn(idle_cooling_dev);
		}
	}

	return 0;
}

static int create_idle_thread(struct cpu_idle_cooling_device *idle_cooling_dev)
{
	unsigned long cpu;
	struct task_struct *thread;

	init_waitqueue_head(&idle_cooling_dev->wait_queue);

	/* start one thread per online cpu */
	for_each_cpu(cpu, idle_cooling_dev->related_cpus) {
		thread = kthread_create_on_node(idle_injection_thread,
						(void *) cpu,
						cpu_to_node(cpu),
						"kidle_inject/%lu", cpu);
		/* bind to cpu here */
		if (likely(!IS_ERR(thread))) {
			cpumask_set_cpu(cpu, idle_cooling_dev->injected_cpus);
			kthread_bind(thread, cpu);
			wake_up_process(thread);
			per_cpu(idle_injection_thread_ptr, cpu) = thread;
		} else {
			return -ENOMEM;
		}
	}

	return 0;
}

static void stop_idle_thread(struct cpu_idle_cooling_device *idle_cooling_dev)
{
	unsigned long cpu;
	struct task_struct **percpu_thread;

	idle_cooling_dev->clamping = false;
	/*
	 * make clamping visible to other cpus and give per cpu threads
	 * sometime to exit, or gets killed later.
	 */
	smp_mb();
	msleep(idle_cooling_dev->duration);
	for_each_cpu(cpu, idle_cooling_dev->injected_cpus) {
		pr_debug("idle inject thread for cpu %lu alive, kill\n", cpu);
		percpu_thread = per_cpu_ptr(&idle_injection_thread_ptr, cpu);
		if (!IS_ERR_OR_NULL(*percpu_thread)) {
			kthread_stop(*percpu_thread);
			*percpu_thread = NULL;
		}
		cpumask_clear_cpu(cpu, idle_cooling_dev->injected_cpus);
	}
}

static int idle_injection_cpu_online(unsigned int cpu)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	idle_cooling_dev = get_cpu_idle_cooling_dev(cpu);
	if (idle_cooling_dev) {
		/* prefer BSP as controlling CPU */
		if (cpu == cpumask_first(idle_cooling_dev->injected_cpus)
			|| !cpu_online(idle_cooling_dev->control_cpu)) {
			idle_cooling_dev->control_cpu = cpu;
			/* make new control_cpu visible to other cpus */
			smp_mb();
		}
		wake_up_interruptible(&idle_cooling_dev->wait_queue);
	}

	return 0;
}

static int idle_injection_cpu_predown(unsigned int cpu)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	idle_cooling_dev = get_cpu_idle_cooling_dev(cpu);
	if (idle_cooling_dev) {
		if (cpu == idle_cooling_dev->control_cpu) {
			cpu = cpumask_next_and(-1,
				idle_cooling_dev->injected_cpus,
				cpu_online_mask);

			if (cpu < nr_cpu_ids)
				idle_cooling_dev->control_cpu = cpu;
			/* make new control_cpu visible to other cpus */
			smp_mb();
		}
	}

	return 0;
}

static int idle_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	*state = MAX_TARGET_RATIO;

	return 0;
}

static int idle_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpu_idle_cooling_device *idle_cooling_dev = cdev->devdata;

	if (true == idle_cooling_dev->clamping)
		*state = (unsigned long)idle_cooling_dev->current_ratio;
	else
		*state = 0; /* indicates invalid state */

	return 0;
}

static int idle_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long new_target_ratio)
{
	struct cpu_idle_cooling_device *idle_cooling_dev = cdev->devdata;
	int ret = 0;

	mutex_lock(&cdev->lock);

	new_target_ratio = clamp(new_target_ratio, 0UL,
				(unsigned long) MAX_TARGET_RATIO);
	if (idle_cooling_dev->set_target_ratio == 0
		&& new_target_ratio > 0) {
		idle_cooling_dev->set_target_ratio =
			(unsigned int) new_target_ratio;
		idle_cooling_dev->control_ratio =
			idle_cooling_dev->set_target_ratio;
		idle_cooling_dev->current_ratio =
			idle_cooling_dev->set_target_ratio;
		idle_cooling_dev->clamping = true;
		wake_up_interruptible(&idle_cooling_dev->wait_queue);
	} else if (idle_cooling_dev->set_target_ratio > 0) {
		if (new_target_ratio == 0) {
			idle_cooling_dev->set_target_ratio = 0;
			idle_cooling_dev->clamping = false;
			/* make clamping visible to other cpus */
			smp_mb();
		} else	/* adjust currently running */ {
			idle_cooling_dev->set_target_ratio =
				(unsigned int) new_target_ratio;
			/* make new set_target_ratio visible to other cpus */
			smp_mb();
		}
	}

	mutex_unlock(&cdev->lock);

	return ret;
}

static struct thermal_cooling_device_ops cpu_idle_injection_cooling_ops = {
	.get_max_state = idle_get_max_state,
	.get_cur_state = idle_get_cur_state,
	.set_cur_state = idle_set_cur_state,
};

unsigned long get_max_idle_state(const struct cpumask *clip_cpus)
{
	return MAX_TARGET_RATIO;
}
EXPORT_SYMBOL_GPL(get_max_idle_state);

void set_idle_state(const struct cpumask *clip_cpus, unsigned long idle_ratio)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	mutex_lock(&cpu_idle_cooling_lock);
	list_for_each_entry(idle_cooling_dev,
		&cpu_idle_cooling_dev_list, node) {
		if (cpumask_subset(idle_cooling_dev->related_cpus, clip_cpus))
			idle_set_cur_state(idle_cooling_dev->cooling_dev,
					idle_ratio);
	}
	mutex_unlock(&cpu_idle_cooling_lock);
}
EXPORT_SYMBOL_GPL(set_idle_state);

struct thermal_cooling_device * __init
cpu_idle_cooling_register(const struct cpumask *clip_cpus)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;
	struct thermal_cooling_device *ret;
	unsigned long cpu;
	char dev_name[THERMAL_NAME_LENGTH];

	if (cpumask_empty(clip_cpus))
		return ERR_PTR(-ENOMEM);

	mutex_lock(&cpu_idle_cooling_lock);
	get_online_cpus();
	list_for_each_entry(idle_cooling_dev,
		&cpu_idle_cooling_dev_list, node) {
		if (cpumask_intersects(idle_cooling_dev->related_cpus,
			clip_cpus)) {
			ret = ERR_PTR(-EINVAL);
			goto exit_unlock;
		}
	}

	idle_cooling_dev = kzalloc(sizeof(*idle_cooling_dev), GFP_KERNEL);
	if (!idle_cooling_dev) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_unlock;
	}

	if (!zalloc_cpumask_var(&idle_cooling_dev->related_cpus, GFP_KERNEL)) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_free_dev;
	}

	if (!zalloc_cpumask_var(&idle_cooling_dev->injected_cpus, GFP_KERNEL)) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_free_related_cpus;
	}

	cpumask_copy(idle_cooling_dev->related_cpus, clip_cpus);
	cpu = cpumask_first(clip_cpus);
	idle_cooling_dev->control_cpu = cpu;
	idle_cooling_dev->id = topology_physical_package_id(cpu);
	idle_cooling_dev->window_size = DEFAULT_WINDOW_SIZE;
	idle_cooling_dev->duration = jiffies_to_msecs(DEFAULT_DURATION_JIFFIES);

	if (create_idle_thread(idle_cooling_dev)) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_free_injected_cpus;
	}

	snprintf(dev_name, sizeof(dev_name), "thermal-cpuidle-%d",
		 idle_cooling_dev->id);
	ret = thermal_cooling_device_register(dev_name,
					idle_cooling_dev,
					&cpu_idle_injection_cooling_ops);
	if (IS_ERR(ret))
		goto exit_stop_thread;

	idle_cooling_dev->cooling_dev = ret;

	if (device_create_file(&idle_cooling_dev->cooling_dev->device,
		&dev_attr_duration)) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_unregister_cdev;
	}

	if (device_create_file(&idle_cooling_dev->cooling_dev->device,
		&dev_attr_window_size)) {
		ret = ERR_PTR(-ENOMEM);
		goto exit_remove_duration_attr;
	}

	list_add(&idle_cooling_dev->node, &cpu_idle_cooling_dev_list);

	goto exit_unlock;

exit_remove_duration_attr:
	device_remove_file(&idle_cooling_dev->cooling_dev->device,
			&dev_attr_duration);
exit_unregister_cdev:
	thermal_cooling_device_unregister(idle_cooling_dev->cooling_dev);
exit_stop_thread:
	stop_idle_thread(idle_cooling_dev);
exit_free_injected_cpus:
	free_cpumask_var(idle_cooling_dev->injected_cpus);
exit_free_related_cpus:
	free_cpumask_var(idle_cooling_dev->related_cpus);
exit_free_dev:
	kfree(idle_cooling_dev);
exit_unlock:
	put_online_cpus();
	mutex_unlock(&cpu_idle_cooling_lock);
	return ret;
}

void cpu_idle_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	if (IS_ERR_OR_NULL(cdev))
		return;

	idle_cooling_dev = cdev->devdata;

	mutex_lock(&cpu_idle_cooling_lock);
	get_online_cpus();
	list_del(&idle_cooling_dev->node);
	put_online_cpus();
	mutex_unlock(&cpu_idle_cooling_lock);

	device_remove_file(&cdev->device, &dev_attr_window_size);
	device_remove_file(&cdev->device, &dev_attr_duration);
	thermal_cooling_device_unregister(idle_cooling_dev->cooling_dev);

	stop_idle_thread(idle_cooling_dev);
	free_cpumask_var(idle_cooling_dev->injected_cpus);
	free_cpumask_var(idle_cooling_dev->related_cpus);
	kfree(idle_cooling_dev);
}

static void __cpu_idle_cooling_exit(void)
{
	struct cpu_idle_cooling_device *idle_cooling_dev;

	while (!list_empty(&cpu_idle_cooling_dev_list)) {
		idle_cooling_dev = list_first_entry(&cpu_idle_cooling_dev_list,
				struct cpu_idle_cooling_device, node);
		cpu_idle_cooling_unregister(idle_cooling_dev->cooling_dev);
	}

	if (hp_state > 0)
		cpuhp_remove_state_nocalls(hp_state);
}

static int __init cpu_idle_cooling_init(void)
{
	struct thermal_cooling_device *ret;
	cpumask_t rest_cpu_mask = CPU_MASK_ALL;
	const struct cpumask *register_cpu_mask;

	hp_state = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"thermal/cpu_idle_cooling:online",
			idle_injection_cpu_online,
			idle_injection_cpu_predown);
	if (hp_state < 0)
		return hp_state;

	do {
		register_cpu_mask =
			topology_core_cpumask(cpumask_first(&rest_cpu_mask));

		if (cpumask_empty(register_cpu_mask))
			break;

		ret = cpu_idle_cooling_register(register_cpu_mask);
		if (IS_ERR(ret)) {
			__cpu_idle_cooling_exit();
			return -ENOMEM;
		}
	} while (cpumask_andnot(&rest_cpu_mask,
				&rest_cpu_mask,
				register_cpu_mask));

	return 0;
}
module_init(cpu_idle_cooling_init);

static void __exit cpu_idle_cooling_exit(void)
{
	__cpu_idle_cooling_exit();
}
module_exit(cpu_idle_cooling_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tao Wang <kevin.wangtao@hisilicon.com>");
MODULE_DESCRIPTION("CPU Idle Cooling Driver for ARM Platform");
