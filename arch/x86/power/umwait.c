// SPDX-License-Identifier: GPL-2.0
/*
 * umwait.c - control user wait
 *
 * Copyright (c) 2018, Intel Corporation.
 * Fenghua Yu <fenghua.yu@intel.com>
 */
/*
 * umwait.c adds control of user wait states that user enters through user wait
 * instructions umwait or tpause.
 */
#include <linux/cpu.h>
#include <asm/msr.h>

static int umwait_disable_c0_2;
static DEFINE_MUTEX(umwait_lock);

static ssize_t umwait_disable_c0_2_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", umwait_disable_c0_2);
}

static ssize_t umwait_disable_c0_2_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	int disable_c0_2, cpu, ret;
	u32 msr_val;

	ret = kstrtou32(buf, 10, &disable_c0_2);
	if (ret)
		return ret;
	if (disable_c0_2 != 1 && disable_c0_2 != 0)
		return -EINVAL;

	mutex_lock(&umwait_lock);
	umwait_disable_c0_2 = disable_c0_2;
	/*
	 * No global umwait maximum time limit (0 in bits 31-0).
	 * Enable or disable C0.2 based on global setting (bit 0) on all CPUs.
	 */
	msr_val = umwait_disable_c0_2 & UMWAIT_CONTROL_C02_MASK;
	for_each_online_cpu(cpu)
		wrmsr_on_cpu(cpu, MSR_IA32_UMWAIT_CONTROL, msr_val, 0);
	mutex_unlock(&umwait_lock);

	return count;
}

static DEVICE_ATTR_RW(umwait_disable_c0_2);

static struct attribute *umwait_attrs[] = {
	&dev_attr_umwait_disable_c0_2.attr,
	NULL
};

static struct attribute_group umwait_attr_group = {
	.attrs = umwait_attrs,
	.name = "user_wait",
};

/* Keep the umwait control MSR on this CPU with the current global setting. */
static int umwait_cpu_online(unsigned int cpu)
{
	u32 msr_val;

	mutex_lock(&umwait_lock);
	/*
	 * No global umwait maximum time limit (0 in bits 31-0).
	 * Enable or disable C0.2 based on global setting (bit 0) on this CPU.
	 */
	msr_val = umwait_disable_c0_2 & UMWAIT_CONTROL_C02_MASK;
	wrmsr(MSR_IA32_UMWAIT_CONTROL, umwait_disable_c0_2, 0);
	mutex_unlock(&umwait_lock);

	return 0;
}

static int __init umwait_init(void)
{
	struct device *dev;
	int ret;

	if (!boot_cpu_has(X86_FEATURE_WAITPKG))
		return -ENODEV;

	/* Add CPU global user wait interface to control umwait C0.2. */
	dev = cpu_subsys.dev_root;
	ret = sysfs_create_group(&dev->kobj, &umwait_attr_group);
	if (ret)
		return ret;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "umwait/intel:online",
				umwait_cpu_online, NULL);
	if (ret < 0)
		goto out_group;

	return 0;
out_group:
	sysfs_remove_group(&dev->kobj, &umwait_attr_group);

	return ret;
}
device_initcall(umwait_init);
