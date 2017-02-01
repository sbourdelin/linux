#ifndef _LINUX_SCHED_IDLE_H
#define _LINUX_SCHED_IDLE_H

enum cpu_idle_type {
	CPU_IDLE,
	CPU_NOT_IDLE,
	CPU_NEWLY_IDLE,
	CPU_MAX_IDLE_TYPES
};

#endif /* _LINUX_SCHED_IDLE_H */
