/*
 * IMC Performance Monitor counter support.
 *
 * Copyright (C) 2017 Madhavan Srinivasan, IBM Corporation.
 *           (C) 2017 Anju T Sudhakar, IBM Corporation.
 *           (C) 2017 Hemant K Shaw, IBM Corporation.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <asm/opal.h>
#include <asm/imc-pmu.h>
#include <asm/cputhreads.h>
#include <asm/smp.h>
#include <linux/string.h>

struct perchip_nest_info nest_perchip_info[IMC_MAX_CHIPS];
struct imc_pmu *per_nest_pmu_arr[IMC_MAX_PMUS];
static cpumask_t nest_imc_cpumask;

static atomic_t nest_events;
static atomic_t core_events;
/* Used to avoid races in calling enable/disable nest-pmu units*/
static DEFINE_MUTEX(imc_nest_reserve);
/* Used to avoid races in calling enable/disable core-pmu units */
static DEFINE_MUTEX(imc_core_reserve);

/*
 * Maintains base addresses for all the cores.
 * MAX chip and core are defined as 32. So we
 * statically allocate 8K for this structure.
 *
 * TODO -- Could be made dynamic
 */
static u64 per_core_pdbar_add[IMC_MAX_CHIPS][IMC_MAX_CORES];
static cpumask_t core_imc_cpumask;
struct imc_pmu *core_imc_pmu;

/* Maintains base address for all the cpus */
static u64 per_cpu_add[NR_CPUS];

/* Needed for sanity check */
extern u64 nest_max_offset;
extern u64 core_max_offset;
extern u64 thread_max_offset;

PMU_FORMAT_ATTR(event, "config:0-20");
static struct attribute *imc_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group imc_format_group = {
	.name = "format",
	.attrs = imc_format_attrs,
};

/* Get the cpumask printed to a buffer "buf" */
static ssize_t imc_pmu_cpumask_get_attr(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	cpumask_t *active_mask;

	if (!strncmp(pmu->name, "nest_", strlen("nest_")))
		active_mask = &nest_imc_cpumask;
	else if (!strncmp(pmu->name, "core_", strlen("core_")))
		active_mask = &core_imc_cpumask;
	else
		return 0;
	return cpumap_print_to_pagebuf(true, buf, active_mask);
}

static DEVICE_ATTR(cpumask, S_IRUGO, imc_pmu_cpumask_get_attr, NULL);

static struct attribute *imc_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group imc_pmu_cpumask_attr_group = {
	.attrs = imc_pmu_cpumask_attrs,
};

/*
 * core_imc_mem_init : Initializes memory for the current core.
 *
 * Uses alloc_pages_exact_nid() and uses the returned address as an argument to
 * an opal call to configure the pdbar. The address sent as an argument is
 * converted to physical address before the opal call is made. This is the
 * base address at which the core imc counters are populated.
 */
static int core_imc_mem_init(void)
{
	int core_id, phys_id;
	int rc = -1;

	phys_id = topology_physical_package_id(smp_processor_id());
	core_id = smp_processor_id() / threads_per_core;

	/*
	 * alloc_pages_exact_nid() will allocate memory for core in the
	 * local node only.
	 */
	per_core_pdbar_add[phys_id][core_id] = (u64) alloc_pages_exact_nid(phys_id,
			(size_t) IMC_CORE_COUNTER_MEM, GFP_KERNEL | __GFP_ZERO);
	rc = opal_core_imc_counters_control(OPAL_CORE_IMC_INIT,
				       (u64)virt_to_phys((void *)per_core_pdbar_add[phys_id][core_id]),
		 0, 0);

	return rc;
}

/*
 * Calls core_imc_mem_init and checks the return value.
 */
static void core_imc_init(int *cpu_opal_rc)
{
	int rc = 0;

	rc = core_imc_mem_init();
	if (rc)
		cpu_opal_rc[smp_processor_id()] = 1;
}

static void core_imc_change_cpu_context(int old_cpu, int new_cpu)
{
	if (!core_imc_pmu)
		return;
	perf_pmu_migrate_context(&core_imc_pmu->pmu, old_cpu, new_cpu);
}


static int ppc_core_imc_cpu_online(unsigned int cpu)
{
	int ret;

	/* If a cpu for this core is already set, then, don't do anything */
	ret = cpumask_any_and(&core_imc_cpumask,
				 cpu_sibling_mask(cpu));
	if (ret < nr_cpu_ids)
		return 0;

	/* Else, set the cpu in the mask, and change the context */
	cpumask_set_cpu(cpu, &core_imc_cpumask);
	opal_core_imc_counters_control(OPAL_CORE_IMC_ENABLE, 0, 0, 0);
	core_imc_change_cpu_context(-1, cpu);
	return 0;
}

static int ppc_core_imc_cpu_offline(unsigned int cpu)
{
	int target;
	unsigned int ncpu;

	/*
	 * clear this cpu out of the mask, if not present in the mask,
	 * don't bother doing anything.
	 */
	if (!cpumask_test_and_clear_cpu(cpu, &core_imc_cpumask))
		return 0;

	/* Find any online cpu in that core except the current "cpu" */
	ncpu = cpumask_any_but(cpu_sibling_mask(cpu), cpu);

	if (ncpu < nr_cpu_ids) {
		target = ncpu;
		cpumask_set_cpu(target, &core_imc_cpumask);
	} else {
		opal_core_imc_counters_control(OPAL_CORE_IMC_DISABLE, 0, 0, 0);
		target = -1;
	}

	/* migrate the context */
	core_imc_change_cpu_context(cpu, target);

	return 0;
}

/*
 * nest_init : Initializes the nest imc engine for the current chip.
 * by default the nest engine is disabled.
 */
static void nest_init(int *cpu_opal_rc)
{
	int rc;

	/*
	 * OPAL figures out which CPU to start based on the CPU that is
	 * currently running when we call into OPAL
	 */
	rc = opal_nest_imc_counters_control(OPAL_NEST_IMC_PRODUCTION_MODE,
					    OPAL_NEST_IMC_STOP, 0, 0);
	if (rc)
		cpu_opal_rc[smp_processor_id()] = 1;
}

static void nest_change_cpu_context(int old_cpu, int new_cpu)
{
	int i;

	for (i = 0;
	     (per_nest_pmu_arr[i] != NULL) && (i < IMC_MAX_PMUS); i++)
		perf_pmu_migrate_context(&per_nest_pmu_arr[i]->pmu,
							old_cpu, new_cpu);
}

static int ppc_nest_imc_cpu_online(unsigned int cpu)
{
	int nid;
	const struct cpumask *l_cpumask;
	struct cpumask tmp_mask;

	/* Find the cpumask of this node */
	nid = cpu_to_node(cpu);
	l_cpumask = cpumask_of_node(nid);

	/*
	 * If any of the cpu from this node is already present in the mask,
	 * just return, if not, then set this cpu in the mask.
	 */
	if (!cpumask_and(&tmp_mask, l_cpumask, &nest_imc_cpumask)) {
		cpumask_set_cpu(cpu, &nest_imc_cpumask);
		nest_change_cpu_context(-1, cpu);
		return 0;
	}

	return 0;
}

static int ppc_nest_imc_cpu_offline(unsigned int cpu)
{
	int nid, target = -1;
	const struct cpumask *l_cpumask;

	/*
	 * Check in the designated list for this cpu. Dont bother
	 * if not one of them.
	 */
	if (!cpumask_test_and_clear_cpu(cpu, &nest_imc_cpumask))
		return 0;

	/*
	 * Now that this cpu is one of the designated,
	 * find a next cpu a) which is online and b) in same chip.
	 */
	nid = cpu_to_node(cpu);
	l_cpumask = cpumask_of_node(nid);
	target = cpumask_next(cpu, l_cpumask);

	/*
	 * Update the cpumask with the target cpu and
	 * migrate the context if needed
	 */
	if (target >= 0 && target <= nr_cpu_ids) {
		cpumask_set_cpu(target, &nest_imc_cpumask);
		nest_change_cpu_context(cpu, target);
	}
	return 0;
}

static int nest_pmu_cpumask_init(void)
{
	const struct cpumask *l_cpumask;
	int cpu, nid;
	int *cpus_opal_rc;

	if (!cpumask_empty(&nest_imc_cpumask))
		return 0;

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		goto fail;

	/*
	 * Nest PMUs are per-chip counters. So designate a cpu
	 * from each chip for counter collection.
	 */
	for_each_online_node(nid) {
		l_cpumask = cpumask_of_node(nid);

		/* designate first online cpu in this node */
		cpu = cpumask_first(l_cpumask);
		cpumask_set_cpu(cpu, &nest_imc_cpumask);
	}

	/* Initialize Nest PMUs in each node using designated cpus */
	on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_init,
						(void *)cpus_opal_rc, 1);

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &nest_imc_cpumask) {
		if (cpus_opal_rc[cpu])
			goto fail;
	}

	cpuhp_setup_state(CPUHP_AP_PERF_POWERPC_NEST_ONLINE,
			  "POWER_NEST_IMC_ONLINE",
			  ppc_nest_imc_cpu_online,
			  ppc_nest_imc_cpu_offline);

	return 0;

fail:
	if (cpus_opal_rc)
		kfree(cpus_opal_rc);
	return -ENODEV;
}

static void cleanup_core_imc_memory(void)
{
	int phys_id, core_id;
	u64 addr;

	phys_id = topology_physical_package_id(smp_processor_id());
	core_id = smp_processor_id() / threads_per_core;

	addr = per_core_pdbar_add[phys_id][core_id];

	/* Only if the address is non-zero shall, we free it */
	if (addr)
		free_pages(addr, 0);
}

static void cleanup_all_core_imc_memory(void)
{
	on_each_cpu_mask(&core_imc_cpumask,
			 (smp_call_func_t)cleanup_core_imc_memory, NULL, 1);
}

/* Enabling of Core Engine needs a scom operation */
static void core_imc_control_enable(void)
{
	opal_core_imc_counters_control(OPAL_CORE_IMC_ENABLE, 0, 0, 0);
}


/*
 * Disabling of IMC Core Engine needs a scom operation
 */
static void core_imc_control_disable(void)
{
	opal_core_imc_counters_control(OPAL_CORE_IMC_DISABLE, 0, 0, 0);
}

/*
 * Function to diable the IMC Core engine using core imc cpumask
 */
void core_imc_disable(void)
{
	on_each_cpu_mask(&core_imc_cpumask,
			 (smp_call_func_t)core_imc_control_disable, NULL, 1);
}

static int core_imc_pmu_cpumask_init(void)
{
	int cpu, *cpus_opal_rc;

	/*
	 * Get the mask of first online cpus for every core.
	 */
	core_imc_cpumask = cpu_online_cores_map();

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		goto fail;

	/*
	 * Initialize the core IMC PMU on each core using the
	 * core_imc_cpumask by calling core_imc_init().
	 */
	on_each_cpu_mask(&core_imc_cpumask, (smp_call_func_t)core_imc_init,
						(void *)cpus_opal_rc, 1);

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &core_imc_cpumask) {
		if (cpus_opal_rc[cpu]) {
			kfree(cpus_opal_rc);
			goto fail;
		}
	}

	kfree(cpus_opal_rc);

	cpuhp_setup_state(CPUHP_AP_PERF_POWERPC_COREIMC_ONLINE,
			  "POWER_CORE_IMC_ONLINE",
			  ppc_core_imc_cpu_online,
			  ppc_core_imc_cpu_offline);

	return 0;

fail:
	/* Free up the allocated pages */
	cleanup_all_core_imc_memory();
	return -ENODEV;
}

static int nest_imc_event_init(struct perf_event *event)
{
	int chip_id;
	u32 config = event->attr.config;
	struct perchip_nest_info *pcni;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Sampling not supported */
	if (event->hw.sample_period)
		return -EINVAL;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	/* Sanity check for config (event offset) */
	if (config > nest_max_offset)
		return -EINVAL;

	chip_id = topology_physical_package_id(event->cpu);
	pcni = &nest_perchip_info[chip_id];

	/*
	 * Memory for Nest HW counter data could be in multiple pages.
	 * Hence check and pick the right event base page for chip with
	 * "chip_id" and add "config" to it".
	 */
	event->hw.event_base = pcni->vbase[config/PAGE_SIZE] +
							(config & ~PAGE_MASK);

	return 0;
}

static int core_imc_event_init(struct perf_event *event)
{
	int core_id, phys_id;
	u64 config = event->attr.config;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Sampling not supported */
	if (event->hw.sample_period)
		return -EINVAL;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	event->hw.idx = -1;

	/* Sanity check for config (event offset) */
	if (config > core_max_offset)
		return -EINVAL;

	core_id = event->cpu / threads_per_core;
	phys_id = topology_physical_package_id(event->cpu);
	event->hw.event_base =
		per_core_pdbar_add[phys_id][core_id] + config;

	return 0;
}

static int thread_imc_event_init(struct perf_event *event)
{
	struct task_struct *target;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Sampling not supported */
	if (event->hw.sample_period)
		return -EINVAL;

	event->hw.idx = -1;

	/* Sanity check for config (event offset) */
	if (event->attr.config > thread_max_offset)
		return -EINVAL;

	target = event->hw.target;

	if (!target)
		return -EINVAL;

	event->pmu->task_ctx_nr = perf_sw_context;
	return 0;
}

static void thread_imc_read_counter(struct perf_event *event)
{
	u64 *addr, data;
	int cpu_id = smp_processor_id();

	addr = (u64 *)(per_cpu_add[cpu_id] + event->attr.config);
	data = __be64_to_cpu(READ_ONCE(*addr));
	local64_set(&event->hw.prev_count, data);
}

static void thread_imc_perf_event_update(struct perf_event *event)
{
	u64 counter_prev, counter_new, final_count, *addr;
	int cpu_id = smp_processor_id();

	addr = (u64 *)(per_cpu_add[cpu_id] + event->attr.config);
	counter_prev = local64_read(&event->hw.prev_count);
	counter_new = __be64_to_cpu(READ_ONCE(*addr));
	final_count = counter_new - counter_prev;

	local64_set(&event->hw.prev_count, counter_new);
	local64_add(final_count, &event->count);
}

static void imc_read_counter(struct perf_event *event)
{
	u64 *addr, data;

	/*
	 * In-Memory Collection (IMC) counters are free flowing counters.
	 * So we take a snapshot of the counter value on enable and save it
	 * to calculate the delta at later stage to present the event counter
	 * value.
	 */
	addr = (u64 *)event->hw.event_base;
	data = __be64_to_cpu(READ_ONCE(*addr));
	local64_set(&event->hw.prev_count, data);
}

static void imc_perf_event_update(struct perf_event *event)
{
	u64 counter_prev, counter_new, final_count, *addr;

	addr = (u64 *)event->hw.event_base;
	counter_prev = local64_read(&event->hw.prev_count);
	counter_new = __be64_to_cpu(READ_ONCE(*addr));
	final_count = counter_new - counter_prev;

	/*
	 * Need to update prev_count is that, counter could be
	 * read in a periodic interval from the tool side.
	 */
	local64_set(&event->hw.prev_count, counter_new);
	/* Update the delta to the event count */
	local64_add(final_count, &event->count);
}

static void nest_imc_start(int *cpu_opal_rc)
{
	int rc;

	/* Enable nest engine */
	rc = opal_nest_imc_counters_control(OPAL_NEST_IMC_PRODUCTION_MODE,
					    OPAL_NEST_IMC_START, 0, 0);
	if (rc)
		cpu_opal_rc[smp_processor_id()] = 1;

}

static int nest_imc_control(int operation)
{
	int *cpus_opal_rc, cpu;

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		return -ENOMEM;
	switch (operation) {

	case	IMC_COUNTER_ENABLE:
			/* Initialize Nest PMUs in each node using designated cpus */
			on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_imc_start,
						(void *)cpus_opal_rc, 1);
			break;
	case	IMC_COUNTER_DISABLE:
			/* Disable the counters */
			on_each_cpu_mask(&nest_imc_cpumask, (smp_call_func_t)nest_init,
						(void *)cpus_opal_rc, 1);
			break;
	default: return -EINVAL;

	}

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &nest_imc_cpumask) {
		if (cpus_opal_rc[cpu])
			return -ENODEV;
	}
	return 0;
}

static void imc_event_start(struct perf_event *event, int flags)
{
	/*
	 * In Memory Counters are free flowing counters. HW or the microcode
	 * keeps adding to the counter offset in memory. To get event
	 * counter value, we snapshot the value here and we calculate
	 * delta at later point.
	 */
	imc_read_counter(event);
}

static void imc_event_stop(struct perf_event *event, int flags)
{
	/*
	 * Take a snapshot and calculate the delta and update
	 * the event counter values.
	 */
	imc_perf_event_update(event);
}

static void nest_imc_event_start(struct perf_event *event, int flags)
{
	int rc;

	/*
	 * Nest pmu units are enabled only when it is used.
	 * See if this is triggered for the first time.
	 * If yes, take the mutex lock and enable the nest counters.
	 * If not, just increment the count in nest_events.
	 */
	if (atomic_inc_return(&nest_events) == 1) {
		mutex_lock(&imc_nest_reserve);
		rc = nest_imc_control(IMC_COUNTER_ENABLE);
		mutex_unlock(&imc_nest_reserve);
		if (rc)
			pr_err("IMC: Unbale to start the counters\n");
	}
	imc_event_start(event, flags);
}

static void nest_imc_event_stop(struct perf_event *event, int flags)
{
	int rc;

	imc_event_stop(event, flags);
	/*
	 * See if we need to disable the nest PMU.
	 * If no events are currently in use, then we have to take a
	 * mutex to ensure that we don't race with another task doing
	 * enable or disable the nest counters.
	 */
	if (atomic_dec_return(&nest_events) == 0) {
		mutex_lock(&imc_nest_reserve);
		rc = nest_imc_control(IMC_COUNTER_DISABLE);
		mutex_unlock(&imc_nest_reserve);
		if (rc)
			pr_err("IMC: Disable counters failed\n");
	}
}

static int nest_imc_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		nest_imc_event_start(event, flags);

	return 0;
}

static int core_imc_control(int operation)
{
	int cpu, *cpus_opal_rc;

	/*
	 * Memory for OPAL call return value.
	 */
	cpus_opal_rc = kzalloc((sizeof(int) * nr_cpu_ids), GFP_KERNEL);
	if (!cpus_opal_rc)
		goto fail;

	/*
	 * Initialize the core IMC PMU on each core using the
	 * core_imc_cpumask by calling core_imc_init().
	 */
	switch (operation) {

	case IMC_COUNTER_DISABLE:
		on_each_cpu_mask(&core_imc_cpumask,
				(smp_call_func_t)core_imc_control_disable,
				(void *)cpus_opal_rc, 1);
		break;
	case IMC_COUNTER_ENABLE:
		on_each_cpu_mask(&core_imc_cpumask,
				(smp_call_func_t)core_imc_control_enable,
				(void *)cpus_opal_rc, 1);
		break;
	default:
		goto fail;
	}

	/* Check return value array for any OPAL call failure */
	for_each_cpu(cpu, &core_imc_cpumask) {
		if (cpus_opal_rc[cpu])
			goto fail;
	}

	return 0;
fail:
	if (cpus_opal_rc)
		kfree(cpus_opal_rc);
	return -EINVAL;
}


static void core_imc_event_start(struct perf_event *event, int flags)
{
	int rc;

	/*
	 * Core pmu units are enabled only when it is used.
	 * See if this is triggered for the first time.
	 * If yes, take the mutex lock and enable the core counters.
	 * If not, just increment the count in core_events.
	 */
	if (atomic_inc_return(&core_events) == 1) {
		mutex_lock(&imc_core_reserve);
		rc = core_imc_control(IMC_COUNTER_ENABLE);
		mutex_unlock(&imc_core_reserve);
		if (rc)
			pr_err("IMC: Unbale to start the counters\n");
	}
	imc_event_start(event, flags);
}

static void core_imc_event_stop(struct perf_event *event, int flags)
{
	int rc;

	imc_event_stop(event, flags);
	/*
	 * See if we need to disable the IMC PMU.
	 * If no events are currently in use, then we have to take a
	 * mutex to ensure that we don't race with another task doing
	 * enable or disable the core counters.
	 */
	if (atomic_dec_return(&core_events) == 0) {
		mutex_lock(&imc_core_reserve);
		rc = core_imc_control(IMC_COUNTER_DISABLE);
		mutex_unlock(&imc_core_reserve);
		if (rc)
			pr_err("IMC: Disable counters failed\n");
	}
}

static int core_imc_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		core_imc_event_start(event, flags);

	return 0;
}


static void thread_imc_event_start(struct perf_event *event, int flags)
{
	int rc;

	/*
	 * Core pmu units are enabled only when it is used.
	 * See if this is triggered for the first time.
	 * If yes, take the mutex lock and enable the core counters.
	 * If not, just increment the count in core_events.
	 */
	if (atomic_inc_return(&core_events) == 1) {
		mutex_lock(&imc_core_reserve);
		rc = core_imc_control(IMC_COUNTER_ENABLE);
		mutex_unlock(&imc_core_reserve);
		if (rc)
			pr_err("IMC: Unbale to start the counters\n");
	}
	thread_imc_read_counter(event);
}

static void thread_imc_event_stop(struct perf_event *event, int flags)
{
	int rc;

	thread_imc_perf_event_update(event);
	/*
	 * See if we need to disable the IMC PMU.
	 * If no events are currently in use, then we have to take a
	 * mutex to ensure that we don't race with another task doing
	 * enable or disable the core counters.
	 */
	if (atomic_dec_return(&core_events) == 0) {
		mutex_lock(&imc_core_reserve);
		rc = core_imc_control(IMC_COUNTER_DISABLE);
		mutex_unlock(&imc_core_reserve);
		if (rc)
			pr_err("IMC: Disable counters failed\n");

	}
}

static void thread_imc_event_del(struct perf_event *event, int flags)
{
	thread_imc_perf_event_update(event);
}

static int thread_imc_event_add(struct perf_event *event, int flags)
{
	thread_imc_event_start(event, flags);

	return 0;
}

static void thread_imc_pmu_start_txn(struct pmu *pmu,
				     unsigned int txn_flags)
{
	if (txn_flags & ~PERF_PMU_TXN_ADD)
		return;
	perf_pmu_disable(pmu);
}

static void thread_imc_pmu_cancel_txn(struct pmu *pmu)
{
	perf_pmu_enable(pmu);
}

static int thread_imc_pmu_commit_txn(struct pmu *pmu)
{
	perf_pmu_enable(pmu);
	return 0;
}

static void thread_imc_pmu_sched_task(struct perf_event_context *ctx,
				  bool sched_in)
{
	return;
}

/* update_pmu_ops : Populate the appropriate operations for "pmu" */
static int update_pmu_ops(struct imc_pmu *pmu)
{
	if (!pmu)
		return -EINVAL;

	pmu->pmu.task_ctx_nr = perf_invalid_context;
	if (pmu->domain == IMC_DOMAIN_NEST) {
		pmu->pmu.event_init = nest_imc_event_init;
		pmu->pmu.add = nest_imc_event_add;
		pmu->pmu.del = nest_imc_event_stop;
		pmu->pmu.start = nest_imc_event_start;
		pmu->pmu.stop = nest_imc_event_stop;
		pmu->attr_groups[IMC_CPUMASK_ATTR] = &imc_pmu_cpumask_attr_group;
	} else if (pmu->domain == IMC_DOMAIN_CORE) {
		pmu->pmu.event_init = core_imc_event_init;
		pmu->pmu.add = core_imc_event_add;
		pmu->pmu.del = core_imc_event_stop;
		pmu->pmu.start = core_imc_event_start;
		pmu->pmu.stop = core_imc_event_stop;
		pmu->attr_groups[IMC_CPUMASK_ATTR] = &imc_pmu_cpumask_attr_group;
	}
	pmu->pmu.read = imc_perf_event_update;
	pmu->attr_groups[IMC_FORMAT_ATTR] = &imc_format_group;
	pmu->pmu.attr_groups = pmu->attr_groups;

	if (pmu->domain == IMC_DOMAIN_THREAD) {
		pmu->pmu.event_init = thread_imc_event_init;
		pmu->pmu.start = thread_imc_event_start;
		pmu->pmu.add = thread_imc_event_add;
		pmu->pmu.del = thread_imc_event_del;
		pmu->pmu.stop = thread_imc_event_stop;
		pmu->pmu.read = thread_imc_perf_event_update;
		pmu->pmu.start_txn = thread_imc_pmu_start_txn;
		pmu->pmu.cancel_txn = thread_imc_pmu_cancel_txn;
		pmu->pmu.commit_txn = thread_imc_pmu_commit_txn;
		pmu->pmu.sched_task = thread_imc_pmu_sched_task;
	}

	return 0;
}

/* dev_str_attr : Populate event "name" and string "str" in attribute */
static struct attribute *dev_str_attr(const char *name, const char *str)
{
	struct perf_pmu_events_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;
	sysfs_attr_init(&attr->attr.attr);

	attr->event_str = str;
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = perf_event_sysfs_show;

	return &attr->attr.attr;
}

/*
 * update_events_in_group: Update the "events" information in an attr_group
 *                         and assign the attr_group to the pmu "pmu".
 */
static int update_events_in_group(struct imc_events *events,
				  int idx, struct imc_pmu *pmu)
{
	struct attribute_group *attr_group;
	struct attribute **attrs;
	int i;

	/* If there is no events for this pmu, just return zero */
	if (!events)
		return 0;

	/* Allocate memory for attribute group */
	attr_group = kzalloc(sizeof(*attr_group), GFP_KERNEL);
	if (!attr_group)
		return -ENOMEM;

	/* Allocate memory for attributes */
	attrs = kzalloc((sizeof(struct attribute *) * (idx + 1)), GFP_KERNEL);
	if (!attrs) {
		kfree(attr_group);
		return -ENOMEM;
	}

	attr_group->name = "events";
	attr_group->attrs = attrs;
	for (i = 0; i < idx; i++, events++) {
		attrs[i] = dev_str_attr((char *)events->ev_name,
					(char *)events->ev_value);
	}

	/* Save the event attribute */
	pmu->attr_groups[IMC_EVENT_ATTR] = attr_group;
	return 0;
}

static void thread_imc_ldbar_disable(void *dummy)
{
	/* LDBAR spr is a per-thread */
	mtspr(SPRN_LDBAR, 0);
}

void thread_imc_disable(void)
{
	on_each_cpu(thread_imc_ldbar_disable, NULL, 1);
}

static void cleanup_thread_imc_memory(void *dummy)
{
	int cpu_id = smp_processor_id();
	u64 addr = per_cpu_add[cpu_id];

	/* Only if the address is non-zero, shall we free it */
	if (addr)
		free_pages(addr, 0);
}

static void cleanup_all_thread_imc_memory(void)
{
	on_each_cpu(cleanup_thread_imc_memory, NULL, 1);
}

/*
 * Allocates a page of memory for each of the online cpus, and, writes the
 * physical base address of that page to the LDBAR for that cpu. This starts
 * the thread IMC counters.
 */
static void thread_imc_mem_alloc(void *dummy)
{
	u64 ldbar_addr, ldbar_value;
	int cpu_id = smp_processor_id();
	int phys_id = topology_physical_package_id(smp_processor_id());

	per_cpu_add[cpu_id] = (u64)alloc_pages_exact_nid(phys_id,
			(size_t)IMC_THREAD_COUNTER_MEM, GFP_KERNEL | __GFP_ZERO);
	ldbar_addr = (u64)virt_to_phys((void *)per_cpu_add[cpu_id]);
	ldbar_value = (ldbar_addr & (u64)THREAD_IMC_LDBAR_MASK) |
		(u64)THREAD_IMC_ENABLE;
	mtspr(SPRN_LDBAR, ldbar_value);
}

void thread_imc_cpu_init(void)
{
	on_each_cpu(thread_imc_mem_alloc, NULL, 1);
}

/*
 * init_imc_pmu : Setup and register the IMC pmu device.
 *
 * @events:	events memory for this pmu.
 * @idx:	number of event entries created.
 * @pmu_ptr:	memory allocated for this pmu.
 *
 * init_imc_pmu() setup the cpu mask information for these pmus and setup
 * the state machine hotplug notifiers as well.
 */
int __init init_imc_pmu(struct imc_events *events, int idx,
		 struct imc_pmu *pmu_ptr)
{
	int ret = -ENODEV;

	/* Add cpumask and register for hotplug notification */
	switch (pmu_ptr->domain) {
	case IMC_DOMAIN_NEST:
		ret = nest_pmu_cpumask_init();
		if (ret)
			return ret;
		break;
	case IMC_DOMAIN_CORE:
		ret = core_imc_pmu_cpumask_init();
		if (ret)
			return ret;
		break;
	case IMC_DOMAIN_THREAD:
		thread_imc_cpu_init();
		break;
	default:
		return -1;  /* Unknown domain */
	}

	ret = update_events_in_group(events, idx, pmu_ptr);
	if (ret)
		goto err_free;

	ret = update_pmu_ops(pmu_ptr);
	if (ret)
		goto err_free;

	ret = perf_pmu_register(&pmu_ptr->pmu, pmu_ptr->pmu.name, -1);
	if (ret)
		goto err_free;

	pr_info("%s performance monitor hardware support registered\n",
		pmu_ptr->pmu.name);

	return 0;

err_free:
	/* Only free the attr_groups which are dynamically allocated  */
	if (pmu_ptr->attr_groups[IMC_EVENT_ATTR]) {
		if (pmu_ptr->attr_groups[IMC_EVENT_ATTR]->attrs)
			kfree(pmu_ptr->attr_groups[IMC_EVENT_ATTR]->attrs);
		kfree(pmu_ptr->attr_groups[IMC_EVENT_ATTR]);
	}
	/* For core_imc, we have allocated memory, we need to free it */
	if (pmu_ptr->domain == IMC_DOMAIN_CORE)
		cleanup_all_core_imc_memory();

	/* For thread_imc, we have allocated memory, we need to free it */
	if (pmu_ptr->domain == IMC_DOMAIN_THREAD)
		cleanup_all_thread_imc_memory();

	return ret;
}
