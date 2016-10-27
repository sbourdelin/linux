/*
 * Scheduler Tunability (SchedTune) Extensions for CFS
 *
 * Copyright (C) 2016 ARM Ltd, Patrick Bellasi <patrick.bellasi@arm.com>
 */

#ifdef CONFIG_SCHED_TUNE

#include <linux/reciprocal_div.h>

extern struct reciprocal_value schedtune_spc_rdiv;

#ifdef CONFIG_CGROUP_SCHED_TUNE

int schedtune_cpu_boost(int cpu);

void schedtune_exit_task(struct task_struct *tsk);

void schedtune_enqueue_task(struct task_struct *p, int cpu);
void schedtune_dequeue_task(struct task_struct *p, int cpu);

#else /* CONFIG_CGROUP_SCHED_TUNE */

#define schedtune_cpu_boost(cpu)  get_sysctl_sched_cfs_boost()

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)
#define schedtune_exit_task(task) do { } while (0)

#endif /* CONFIG_CGROUP_SCHED_TUNE */

#else /* CONFIG_SCHED_TUNE */

#define schedtune_cpu_boost(cpu)  0

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)
#define schedtune_exit_task(task) do { } while (0)

#endif /* CONFIG_SCHED_TUNE */
