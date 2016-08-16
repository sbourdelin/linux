/* Copyright (c) 2015,2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt) "l2 perfevents: " fmt

#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/perf_event_l2.h>
#include <linux/soc/qcom/l2-accessors.h>

/*
 * The cache is made-up of one or more slices, each slice has its own PMU.
 * This structure represents one of the hardware PMUs.
 */

struct hml2_pmu {
	struct perf_event *events[MAX_L2_CTRS];
	unsigned long used_mask[BITS_TO_LONGS(MAX_L2_CTRS)];
	unsigned long group_used_mask[BITS_TO_LONGS(L2_EVT_GROUP_MAX + 1)];
	int group_to_counter[L2_EVT_GROUP_MAX + 1];
	int irq;
	/* The CPU that is used for collecting events on this slice */
	int on_cpu;
	/* All the CPUs associated with this slice */
	cpumask_t slice_cpus;
	atomic64_t prev_count[MAX_L2_CTRS];
	spinlock_t pmu_lock;
};

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct l2cache_pmu {
	u32 num_pmus;
	struct pmu pmu;
	int num_counters;
	cpumask_t cpumask;
	struct notifier_block cpu_nb;
	struct platform_device *pdev;
};

#define to_l2cache_pmu(p) (container_of(p, struct l2cache_pmu, pmu))

static DEFINE_PER_CPU(struct hml2_pmu *, cpu_to_pmu);
static struct l2cache_pmu l2cache_pmu = { 0 };
static u32 l2_cycle_ctr_idx;
static u32 l2_reset_mask;

static inline u32 idx_to_reg_bit(u32 idx)
{
	u32 bit;

	if (idx == l2_cycle_ctr_idx)
		bit = BIT(L2CYCLE_CTR_BIT);
	else
		bit = BIT(idx);
	return bit;
}

static inline struct hml2_pmu *get_hml2_pmu(int cpu)
{
	return per_cpu(cpu_to_pmu, cpu);
}

static void hml2_pmu__reset_on_slice(void *x)
{
	/* Reset all ctrs */
	set_l2_indirect_reg(L2PMCR, L2PMCR_RESET_ALL);
	set_l2_indirect_reg(L2PMCNTENCLR, l2_reset_mask);
	set_l2_indirect_reg(L2PMINTENCLR, l2_reset_mask);
	set_l2_indirect_reg(L2PMOVSCLR, l2_reset_mask);
}

static inline void hml2_pmu__reset(struct hml2_pmu *slice)
{
	int cpu;

	if (cpumask_test_cpu(smp_processor_id(), &slice->slice_cpus)) {
		hml2_pmu__reset_on_slice(NULL);
		return;
	}

	/* Call each cpu in the cluster until one works */
	for_each_cpu(cpu, &slice->slice_cpus) {
		if (!smp_call_function_single(cpu, hml2_pmu__reset_on_slice,
					      NULL, 1))
			return;
	}

	dev_err(&l2cache_pmu.pdev->dev,
		"Failed to reset on cluster with cpu %d\n",
		cpumask_first(&slice->slice_cpus));
}

static inline void hml2_pmu__enable(void)
{
	set_l2_indirect_reg(L2PMCR, L2PMCR_GLOBAL_ENABLE);
}

static inline void hml2_pmu__disable(void)
{
	set_l2_indirect_reg(L2PMCR, L2PMCR_GLOBAL_DISABLE);
}

static inline void hml2_pmu__counter_set_value(u32 idx, u64 value)
{
	u32 counter_reg;

	if (idx == l2_cycle_ctr_idx) {
		set_l2_indirect_reg(L2PMCCNTR, value);
	} else {
		counter_reg = (idx * IA_L2_REG_OFFSET) + IA_L2PMXEVCNTR_BASE;
		set_l2_indirect_reg(counter_reg, (u32)(value & GENMASK(31, 0)));
	}
}

static inline u64 hml2_pmu__counter_get_value(u32 idx)
{
	u64 value;
	u32 counter_reg;

	if (idx == l2_cycle_ctr_idx) {
		value = get_l2_indirect_reg(L2PMCCNTR);
	} else {
		counter_reg = (idx * IA_L2_REG_OFFSET) + IA_L2PMXEVCNTR_BASE;
		value = get_l2_indirect_reg(counter_reg);
	}

	return value;
}

static inline void hml2_pmu__counter_enable(u32 idx)
{
	u32 reg;

	reg = get_l2_indirect_reg(L2PMCNTENSET);
	reg |= idx_to_reg_bit(idx);
	set_l2_indirect_reg(L2PMCNTENSET, reg);
}

static inline void hml2_pmu__counter_disable(u32 idx)
{
	set_l2_indirect_reg(L2PMCNTENCLR, idx_to_reg_bit(idx));
}

static inline void hml2_pmu__counter_enable_interrupt(u32 idx)
{
	u32 reg;

	reg = get_l2_indirect_reg(L2PMINTENSET);
	reg |= idx_to_reg_bit(idx);
	set_l2_indirect_reg(L2PMINTENSET, reg);
}

static inline void hml2_pmu__counter_disable_interrupt(u32 idx)
{
	set_l2_indirect_reg(L2PMINTENCLR, idx_to_reg_bit(idx));
}

static inline void hml2_pmu__set_evcntcr(u32 ctr, u32 val)
{
	u32 evtcr_reg = (ctr * IA_L2_REG_OFFSET) + IA_L2PMXEVCNTCR_BASE;

	set_l2_indirect_reg(evtcr_reg, val);
}

static inline void hml2_pmu__set_evtyper(u32 ctr, u32 val)
{
	u32 evtype_reg = (ctr * IA_L2_REG_OFFSET) + IA_L2PMXEVTYPER_BASE;

	set_l2_indirect_reg(evtype_reg, val);
}

static void hml2_pmu__set_resr(struct hml2_pmu *slice,
			       u32 event_group, u32 event_cc)
{
	u64 field;
	u64 resr_val;
	u32 shift;
	unsigned long iflags;

	shift = L2PMRESR_GROUP_BITS * event_group;
	field = ((u64)(event_cc & L2PMRESR_GROUP_MASK) << shift) | L2PMRESR_EN;

	spin_lock_irqsave(&slice->pmu_lock, iflags);

	resr_val = get_l2_indirect_reg(L2PMRESR);
	resr_val &= ~(L2PMRESR_GROUP_MASK << shift);
	resr_val |= field;
	set_l2_indirect_reg(L2PMRESR, resr_val);

	spin_unlock_irqrestore(&slice->pmu_lock, iflags);
}

static inline void hml2_pmu__set_evfilter_sys_mode(u32 ctr)
{
	set_l2_indirect_reg((ctr * IA_L2_REG_OFFSET) + IA_L2PMXEVFILTER_BASE,
			    L2PMXEVFILTER_SUFILTER_ALL |
			    L2PMXEVFILTER_ORGFILTER_IDINDEP |
			    L2PMXEVFILTER_ORGFILTER_ALL);
}

static inline u32 hml2_pmu__getreset_ovsr(void)
{
	u32 result = get_l2_indirect_reg(L2PMOVSSET);

	set_l2_indirect_reg(L2PMOVSCLR, result);
	return result;
}

static inline bool hml2_pmu__has_overflowed(u32 ovsr)
{
	return !!(ovsr & l2_reset_mask);
}

static inline bool hml2_pmu__counter_has_overflowed(u32 ovsr, u32 idx)
{
	return !!(ovsr & idx_to_reg_bit(idx));
}

static void l2_cache__event_update_from_slice(struct perf_event *event,
					      struct hml2_pmu *slice)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 delta64, prev, now;
	u32 delta;
	u32 idx = hwc->idx;

	do {
		prev = atomic64_read(&slice->prev_count[idx]);
		now = hml2_pmu__counter_get_value(idx);
	} while (atomic64_cmpxchg(&slice->prev_count[idx], prev, now) != prev);

	if (idx == l2_cycle_ctr_idx) {
		/*
		 * The cycle counter is 64-bit so needs separate handling
		 * of 64-bit delta.
		 */
		delta64 = now - prev;
		local64_add(delta64, &event->count);
	} else {
		/*
		 * 32-bit counters need the unsigned 32-bit math to handle
		 * overflow and now < prev
		 */
		delta = now - prev;
		local64_add(delta, &event->count);
	}
}

static void l2_cache__slice_set_period(struct hml2_pmu *slice,
				       struct hw_perf_event *hwc)
{
	u64 value = L2_MAX_PERIOD - (L2_CNT_PERIOD - 1);
	u32 idx = hwc->idx;
	u64 prev = atomic64_read(&slice->prev_count[idx]);

	if (prev < value) {
		value += prev;
		atomic64_set(&slice->prev_count[idx], value);
	} else {
		value = prev;
	}

	hml2_pmu__counter_set_value(idx, value);
}

static int l2_cache__get_event_idx(struct hml2_pmu *slice,
				   struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	if (hwc->config_base == L2CYCLE_CTR_RAW_CODE) {
		if (test_and_set_bit(l2_cycle_ctr_idx, slice->used_mask))
			return -EAGAIN;

		return l2_cycle_ctr_idx;
	}

	for (idx = 0; idx < l2cache_pmu.num_counters - 1; idx++) {
		if (!test_and_set_bit(idx, slice->used_mask)) {
			set_bit(L2_EVT_GROUP(hwc->config_base),
				slice->group_used_mask);
			return idx;
		}
	}

	/* The counters are all in use. */
	return -EAGAIN;
}

static void l2_cache__clear_event_idx(struct hml2_pmu *slice,
				      struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	clear_bit(L2_EVT_GROUP(hwc->config_base), slice->group_used_mask);
}

static irqreturn_t l2_cache__handle_irq(int irq_num, void *data)
{
	struct hml2_pmu *slice = data;
	u32 ovsr;
	int idx;

	ovsr = hml2_pmu__getreset_ovsr();
	if (!hml2_pmu__has_overflowed(ovsr))
		return IRQ_NONE;

	for (idx = 0; idx < l2cache_pmu.num_counters; idx++) {
		struct perf_event *event = slice->events[idx];
		struct hw_perf_event *hwc;

		if (!event)
			continue;

		if (!hml2_pmu__counter_has_overflowed(ovsr, idx))
			continue;

		l2_cache__event_update_from_slice(event, slice);
		hwc = &event->hw;

		l2_cache__slice_set_period(slice, hwc);
	}

	/*
	 * Handle the pending perf events.
	 *
	 * Note: this call *must* be run with interrupts disabled. For
	 * platforms that can have the PMU interrupts raised as an NMI, this
	 * will not work.
	 */
	irq_work_run();

	return IRQ_HANDLED;
}

/*
 * Implementation of abstract pmu functionality required by
 * the core perf events code.
 */

static void l2_cache__pmu_enable(struct pmu *pmu)
{
	hml2_pmu__enable();
}

static void l2_cache__pmu_disable(struct pmu *pmu)
{
	hml2_pmu__disable();
}

static int l2_cache__event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct hml2_pmu *slice;
	struct perf_event *sibling;

	if (event->attr.type != l2cache_pmu.pmu.type)
		return -ENOENT;

	if (hwc->sample_period) {
		dev_warn(&l2cache_pmu.pdev->dev, "Sampling not supported\n");
		return -EOPNOTSUPP;
	}

	if (event->cpu < 0) {
		dev_warn(&l2cache_pmu.pdev->dev, "Per-task mode not supported\n");
		return -EOPNOTSUPP;
	}

	/* We cannot filter accurately so we just don't allow it. */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_hv || event->attr.exclude_idle) {
		dev_warn(&l2cache_pmu.pdev->dev, "Can't exclude execution levels\n");
		return -EOPNOTSUPP;
	}

	if (((L2_EVT_GROUP(event->attr.config) > L2_EVT_GROUP_MAX) ||
	    (L2_EVT_PREFIX(event->attr.config) != 0) ||
	    (L2_EVT_REG(event->attr.config) != 0)) &&
	    (event->attr.config != L2CYCLE_CTR_RAW_CODE)) {
		dev_warn(&l2cache_pmu.pdev->dev, "Invalid config %llx\n",
			 event->attr.config);
		return -EINVAL;
	}

	/* Don't allow groups with mixed PMUs, except for s/w events */
	if (event->group_leader->pmu != event->pmu &&
	    !is_software_event(event->group_leader)) {
		dev_warn(&l2cache_pmu.pdev->dev,
			 "Can't create mixed PMU group\n");
		return -EINVAL;
	}

	list_for_each_entry(sibling, &event->group_leader->sibling_list,
			    group_entry)
		if (sibling->pmu != event->pmu &&
		    !is_software_event(sibling)) {
			dev_warn(&l2cache_pmu.pdev->dev,
				 "Can't create mixed PMU group\n");
			return -EINVAL;
		}

	hwc->idx = -1;
	hwc->config_base = event->attr.config;

	/*
	 * Ensure all events are on the same cpu so all events are in the
	 * same cpu context, to avoid races on pmu_enable etc.
	 */
	slice = get_hml2_pmu(event->cpu);
	event->cpu = slice->on_cpu;

	return 0;
}

static void l2_cache__event_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (hwc->idx < 0)
		return;

	l2_cache__event_update_from_slice(event, get_hml2_pmu(event->cpu));
}

static void l2_cache__event_start(struct perf_event *event, int flags)
{
	struct hml2_pmu *slice;
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;
	u32 config;
	u32 evt_prefix, event_cc, event_group;

	if (idx < 0)
		return;

	hwc->state = 0;

	slice = get_hml2_pmu(event->cpu);
	l2_cache__slice_set_period(slice, hwc);

	if (hwc->config_base == L2CYCLE_CTR_RAW_CODE)
		goto out;

	config = hwc->config_base;
	evt_prefix  = L2_EVT_PREFIX(config);
	event_cc    = L2_EVT_CODE(config);
	event_group = L2_EVT_GROUP(config);

	hml2_pmu__set_evcntcr(idx, 0x0);
	hml2_pmu__set_evtyper(idx, event_group);
	hml2_pmu__set_resr(slice, event_group, event_cc);
	hml2_pmu__set_evfilter_sys_mode(idx);
out:
	hml2_pmu__counter_enable_interrupt(idx);
	hml2_pmu__counter_enable(idx);
}

static void l2_cache__event_stop(struct perf_event *event, int flags)
{
	struct hml2_pmu *slice;
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	if (idx < 0)
		return;

	if (!(hwc->state & PERF_HES_STOPPED)) {
		slice = get_hml2_pmu(event->cpu);
		hml2_pmu__counter_disable_interrupt(idx);
		hml2_pmu__counter_disable(idx);

		if (flags & PERF_EF_UPDATE)
			l2_cache__event_update(event);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static int l2_cache__event_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx;
	int err = 0;
	struct hml2_pmu *slice;

	slice = get_hml2_pmu(event->cpu);

	idx = l2_cache__get_event_idx(slice, event);
	if (idx < 0) {
		err = idx;
		goto out;
	}

	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	slice->events[idx] = event;
	slice->group_to_counter[L2_EVT_GROUP(hwc->config_base)] = idx;
	atomic64_set(&slice->prev_count[idx], 0ULL);

	if (flags & PERF_EF_START)
		l2_cache__event_start(event, flags);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);

out:
	return err;
}

static void l2_cache__event_del(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct hml2_pmu *slice;
	int idx = hwc->idx;

	if (idx < 0)
		return;

	slice = get_hml2_pmu(event->cpu);
	l2_cache__event_stop(event, flags | PERF_EF_UPDATE);
	slice->events[idx] = NULL;
	clear_bit(idx, slice->used_mask);
	l2_cache__clear_event_idx(slice, event);

	perf_event_update_userpage(event);
}

static void l2_cache__event_read(struct perf_event *event)
{
	l2_cache__event_update(event);
}

static int l2_cache_filter_match(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct hml2_pmu *slice = get_hml2_pmu(event->cpu);
	unsigned int group = L2_EVT_GROUP(hwc->config_base);

	/* check for column exclusion: group already in use by another event */
	if (test_bit(group, slice->group_used_mask) &&
	    slice->events[slice->group_to_counter[group]] != event)
		return 0;

	return 1;
}

static ssize_t l2_cache_pmu_cpumask_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &l2cache_pmu.cpumask);
}

static struct device_attribute l2_cache_pmu_cpumask_attr =
		__ATTR(cpumask, S_IRUGO, l2_cache_pmu_cpumask_show, NULL);

static struct attribute *l2_cache_pmu_cpumask_attrs[] = {
	&l2_cache_pmu_cpumask_attr.attr,
	NULL,
};

static struct attribute_group l2_cache_pmu_cpumask_group = {
	.attrs = l2_cache_pmu_cpumask_attrs,
};

/* NRCCG format for perf RAW codes. */
PMU_FORMAT_ATTR(l2_prefix, "config:16-19");
PMU_FORMAT_ATTR(l2_reg,    "config:12-15");
PMU_FORMAT_ATTR(l2_code,   "config:4-11");
PMU_FORMAT_ATTR(l2_grp,    "config:0-3");
static struct attribute *l2_cache_pmu_formats[] = {
	&format_attr_l2_prefix.attr,
	&format_attr_l2_reg.attr,
	&format_attr_l2_code.attr,
	&format_attr_l2_grp.attr,
	NULL,
};

static struct attribute_group l2_cache_pmu_format_group = {
	.name = "format",
	.attrs = l2_cache_pmu_formats,
};

static const struct attribute_group *l2_cache_pmu_attr_grps[] = {
	&l2_cache_pmu_format_group,
	&l2_cache_pmu_cpumask_group,
	NULL,
};

/*
 * Generic device handlers
 */

static const struct acpi_device_id l2_cache_pmu_acpi_match[] = {
	{ "QCOM8130", },
	{ }
};

static int get_num_counters(void)
{
	int val;

	val = get_l2_indirect_reg(L2PMCR);

	/*
	 * Read number of counters from L2PMCR and add 1
	 * for the cycle counter.
	 */
	return ((val >> L2PMCR_NUM_EV_SHIFT) & L2PMCR_NUM_EV_MASK) + 1;
}

static int l2cache_pmu_cpu_notifier(struct notifier_block *nb,
				    unsigned long action, void *hcpu)
{
	struct l2cache_pmu *l2cache =
		container_of(nb, struct l2cache_pmu, cpu_nb);
	unsigned int cpu = (long)hcpu;
	unsigned int target;
	struct hml2_pmu *slice;
	cpumask_t slice_online_cpus;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DOWN_PREPARE:
		if (!cpumask_test_and_clear_cpu(cpu, &l2cache->cpumask))
			break;
		slice = get_hml2_pmu(cpu);
		cpumask_and(&slice_online_cpus, &slice->slice_cpus,
			    cpu_online_mask);
		/* Any other CPU for this slice which is still online */
		target = cpumask_any_but(&slice_online_cpus, cpu);
		if (target >= nr_cpu_ids)
			break;
		perf_pmu_migrate_context(&l2cache->pmu, cpu, target);
		slice->on_cpu = target;
		cpumask_set_cpu(target, &l2cache->cpumask);
		WARN_ON(irq_set_affinity(slice->irq, cpumask_of(target)));
		break;
	case CPU_ONLINE:
		slice = get_hml2_pmu(cpu);
		cpumask_and(&slice_online_cpus, &slice->slice_cpus,
			    cpu_online_mask);
		if (cpumask_weight(&slice_online_cpus) == 1) {
			/* all CPUs on this slice were down, use this one */
			slice->on_cpu = cpu;
			cpumask_set_cpu(cpu, &l2cache->cpumask);
			WARN_ON(irq_set_affinity(slice->irq, cpumask_of(cpu)));
		}
		break;
	}

	return NOTIFY_OK;
}

static int l2_cache_pmu_probe_slice(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev->parent);
	struct platform_device *sdev = to_platform_device(dev);
	struct l2cache_pmu *l2cache = data;
	struct hml2_pmu *slice;
	struct acpi_device *device;
	int irq;
	int err;
	int logical_cpu;
	unsigned long fw_slice_id;

	if (acpi_bus_get_device(ACPI_HANDLE(dev), &device))
		return -ENODEV;

	if (kstrtol(device->pnp.unique_id, 10, &fw_slice_id) < 0) {
		dev_err(&pdev->dev, "unable to read ACPI uid\n");
		return -ENODEV;
	}

	irq = platform_get_irq(sdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev,
			"Failed to get valid irq for slice %ld\n", fw_slice_id);
		return irq;
	}

	slice = devm_kzalloc(&pdev->dev, sizeof(*slice), GFP_KERNEL);
	if (!slice)
		return -ENOMEM;

	for_each_present_cpu(logical_cpu) {
		if (topology_physical_package_id(logical_cpu) == fw_slice_id) {
			cpumask_set_cpu(logical_cpu, &slice->slice_cpus);
			per_cpu(cpu_to_pmu, logical_cpu) = slice;
		}
	}
	slice->irq = irq;

	if (cpumask_empty(&slice->slice_cpus)) {
		dev_err(&pdev->dev, "No CPUs found for L2 cache instance %ld\n",
			fw_slice_id);
		return -ENODEV;
	}

	/* Pick one CPU to be the preferred one to use in the slice */
	slice->on_cpu = cpumask_first(&slice->slice_cpus);

	if (irq_set_affinity(irq, cpumask_of(slice->on_cpu))) {
		dev_err(&pdev->dev,
			"Unable to set irq affinity (irq=%d, cpu=%d)\n",
			irq, slice->on_cpu);
		return -ENODEV;
	}

	err = devm_request_irq(
		&pdev->dev, irq, l2_cache__handle_irq,
		IRQF_NOBALANCING, "l2-cache-pmu", slice);
	if (err) {
		dev_err(&pdev->dev,
			"Unable to request IRQ%d for L2 PMU counters\n",
			irq);
		return err;
	}

	dev_info(&pdev->dev,
		 "Registered L2 cache PMU instance %ld with %d CPUs\n",
		 fw_slice_id, cpumask_weight(&slice->slice_cpus));

	slice->pmu_lock = __SPIN_LOCK_UNLOCKED(slice->pmu_lock);
	cpumask_set_cpu(slice->on_cpu, &l2cache->cpumask);

	hml2_pmu__reset(slice);
	l2cache->num_pmus++;

	return 0;
}

static int l2_cache_pmu_probe(struct platform_device *pdev)
{
	int err;

	l2cache_pmu.pmu = (struct pmu) {
		/* suffix is instance id for future use with multiple sockets */
		.name		= "l2cache_0",
		.task_ctx_nr    = perf_invalid_context,
		.pmu_enable	= l2_cache__pmu_enable,
		.pmu_disable	= l2_cache__pmu_disable,
		.event_init	= l2_cache__event_init,
		.add		= l2_cache__event_add,
		.del		= l2_cache__event_del,
		.start		= l2_cache__event_start,
		.stop		= l2_cache__event_stop,
		.read		= l2_cache__event_read,
		.attr_groups	= l2_cache_pmu_attr_grps,
		.filter_match   = l2_cache_filter_match,
	};

	l2cache_pmu.num_counters = get_num_counters();
	l2cache_pmu.pdev = pdev;
	l2_cycle_ctr_idx = l2cache_pmu.num_counters - 1;
	l2_reset_mask = GENMASK(l2cache_pmu.num_counters - 2, 0) |
		L2PM_CC_ENABLE;

	cpumask_clear(&l2cache_pmu.cpumask);

	/* Read slice info and initialize each slice */
	err = device_for_each_child(&pdev->dev, &l2cache_pmu,
				    l2_cache_pmu_probe_slice);
	if (err < 0)
		return err;

	if (l2cache_pmu.num_pmus == 0) {
		dev_err(&pdev->dev, "No hardware L2 PMUs found\n");
		return -ENODEV;
	}

	l2cache_pmu.cpu_nb.notifier_call = l2cache_pmu_cpu_notifier;
	l2cache_pmu.cpu_nb.priority = CPU_PRI_PERF + 1;
	err = register_cpu_notifier(&l2cache_pmu.cpu_nb);
	if (err)
		return err;

	err = perf_pmu_register(&l2cache_pmu.pmu, l2cache_pmu.pmu.name, -1);
	if (err < 0)
		goto probe_err;
	else
		dev_info(&pdev->dev,
			 "Registered L2 cache PMU using %d HW PMUs\n",
			 l2cache_pmu.num_pmus);

	return err;

probe_err:
	dev_err(&pdev->dev, "Failed to register L2 cache PMU (%d)\n", err);
	unregister_cpu_notifier(&l2cache_pmu.cpu_nb);
	return err;
}

static int l2_cache_pmu_remove(struct platform_device *pdev)
{
	unregister_cpu_notifier(&l2cache_pmu.cpu_nb);
	perf_pmu_unregister(&l2cache_pmu.pmu);
	return 0;
}

static struct platform_driver l2_cache_pmu_driver = {
	.driver = {
		.name = "qcom-l2cache-pmu",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(l2_cache_pmu_acpi_match),
	},
	.probe = l2_cache_pmu_probe,
	.remove = l2_cache_pmu_remove,
};

static int __init register_l2_cache_pmu_driver(void)
{
	return platform_driver_register(&l2_cache_pmu_driver);
}
device_initcall(register_l2_cache_pmu_driver);
