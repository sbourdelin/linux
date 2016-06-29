/*
 * cpuidle.h - The internal header file
 */

#ifndef __DRIVER_CPUIDLE_H
#define __DRIVER_CPUIDLE_H

/* For internal use only */
extern struct cpuidle_governor *cpuidle_curr_governor;
extern struct list_head cpuidle_governors;
extern struct list_head cpuidle_detected_devices;
extern struct mutex cpuidle_lock;
extern spinlock_t cpuidle_driver_lock;
extern int cpuidle_disabled(void);
extern int cpuidle_enter_state(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state);

/* idle loop */
extern void cpuidle_install_idle_handler(void);
extern void cpuidle_uninstall_idle_handler(void);

/* governors */
extern int cpuidle_switch_governor(struct cpuidle_governor *gov);

/* sysfs */

struct device;

extern int cpuidle_add_interface(struct device *dev);
extern void cpuidle_remove_interface(struct device *dev);
extern int cpuidle_add_device_sysfs(struct cpuidle_device *device);
extern void cpuidle_remove_device_sysfs(struct cpuidle_device *device);
extern int cpuidle_add_sysfs(struct cpuidle_device *dev);
extern void cpuidle_remove_sysfs(struct cpuidle_device *dev);

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
bool cpuidle_state_is_coupled(struct cpuidle_driver *drv, int state);
int cpuidle_coupled_state_verify(struct cpuidle_driver *drv);
int cpuidle_enter_state_coupled(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state);
int cpuidle_coupled_register_device(struct cpuidle_device *dev);
void cpuidle_coupled_unregister_device(struct cpuidle_device *dev);
#else
static inline
bool cpuidle_state_is_coupled(struct cpuidle_driver *drv, int state)
{
	return false;
}

static inline int cpuidle_coupled_state_verify(struct cpuidle_driver *drv)
{
	return 0;
}

static inline int cpuidle_enter_state_coupled(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int next_state)
{
	return -1;
}

static inline int cpuidle_coupled_register_device(struct cpuidle_device *dev)
{
	return 0;
}

static inline void cpuidle_coupled_unregister_device(struct cpuidle_device *dev)
{
}
#endif

/*
 * Used for calculating last_residency in usec. Optimized for case
 * where last_residency in nsecs is < INT_MAX/2 by using faster
 * approximation. Approximated value has less than 1% error.
 */
static inline int convert_nsec_to_usec(u64 nsec)
{
	if (likely(nsec < INT_MAX / 2)) {
		int usec = (int)nsec;

		usec += usec >> 5;
		usec = usec >> 10;
		return usec;
	} else {
		u64 usec = div_u64(nsec, 1000);

		if (usec > INT_MAX)
			usec = INT_MAX;
		return (int)usec;
	}
}


#endif /* __DRIVER_CPUIDLE_H */
