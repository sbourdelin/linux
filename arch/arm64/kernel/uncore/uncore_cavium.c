/*
 * Cavium Thunder uncore PMU support. Derived from Intel and AMD uncore code.
 *
 * Copyright (C) 2015,2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/perf_event.h>
#include <linux/pci.h>

#include <asm/cpufeature.h>
#include <asm/cputype.h>

#include "uncore_cavium.h"

int thunder_uncore_version;

struct thunder_uncore *event_to_thunder_uncore(struct perf_event *event)
{
	if (event->pmu->type == thunder_l2c_tad_pmu.type)
		return thunder_uncore_l2c_tad;
	else if (event->pmu->type == thunder_l2c_cbc_pmu.type)
		return thunder_uncore_l2c_cbc;
	else
		return NULL;
}

void thunder_uncore_read(struct perf_event *event)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new = 0;
	s64 delta;
	int i;

	/*
	 * since we do not enable counter overflow interrupts,
	 * we do not have to worry about prev_count changing on us
	 */

	prev = local64_read(&hwc->prev_count);

	/* read counter values from all units */
	for (i = 0; i < uncore->nr_units; i++)
		new += readq(map_offset(hwc->event_base, uncore, i));

	local64_set(&hwc->prev_count, new);
	delta = new - prev;
	local64_add(delta, &event->count);
}

void thunder_uncore_del(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	int i;

	event->pmu->stop(event, PERF_EF_UPDATE);

	for (i = 0; i < uncore->num_counters; i++) {
		if (cmpxchg(&uncore->events[i], event, NULL) == event)
			break;
	}
	hwc->idx = -1;
}

int thunder_uncore_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore *uncore;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* we do not support sampling */
	if (is_sampling_event(event))
		return -EINVAL;

	/* counters do not have these bits */
	if (event->attr.exclude_user	||
	    event->attr.exclude_kernel	||
	    event->attr.exclude_host	||
	    event->attr.exclude_guest	||
	    event->attr.exclude_hv	||
	    event->attr.exclude_idle)
		return -EINVAL;

	/* and we do not enable counter overflow interrupts */

	uncore = event_to_thunder_uncore(event);
	if (!uncore)
		return -ENODEV;
	if (!uncore->event_valid(event->attr.config))
		return -EINVAL;

	hwc->config = event->attr.config;
	hwc->idx = -1;

	/* and we don't care about CPU */

	return 0;
}

static cpumask_t thunder_active_mask;

static ssize_t thunder_uncore_attr_show_cpumask(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	cpumask_t *active_mask = &thunder_active_mask;

	/*
	 * Thunder uncore events are independent from CPUs. Provide a cpumask
	 * nevertheless to prevent perf from adding the event per-cpu and just
	 * set the mask to one online CPU.
	 */
	cpumask_set_cpu(cpumask_first(cpu_online_mask), active_mask);

	return cpumap_print_to_pagebuf(true, buf, active_mask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, thunder_uncore_attr_show_cpumask, NULL);

static struct attribute *thunder_uncore_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

struct attribute_group thunder_uncore_attr_group = {
	.attrs = thunder_uncore_attrs,
};

ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page)
{
	struct perf_pmu_events_attr *pmu_attr =
		container_of(attr, struct perf_pmu_events_attr, attr);

	if (pmu_attr->event_str)
		return sprintf(page, "%s", pmu_attr->event_str);

	return 0;
}

int __init thunder_uncore_setup(struct thunder_uncore *uncore, int id,
			 unsigned long offset, unsigned long size,
			 struct pmu *pmu)
{
	struct pci_dev *pdev = NULL;
	pci_bus_addr_t start;
	int ret, node = 0;

	/* detect PCI devices */
	do {
		pdev = pci_get_device(PCI_VENDOR_ID_CAVIUM, id, pdev);
		if (!pdev)
			break;
		start = pci_resource_start(pdev, 0);
		uncore->pdevs[node].pdev = pdev;
		uncore->pdevs[node].base = start;
		uncore->pdevs[node].map = ioremap(start + offset, size);
		node++;
		if (node >= MAX_NR_UNCORE_PDEVS) {
			pr_err("reached pdev limit\n");
			break;
		}
	} while (1);

	if (!node)
		return -ENODEV;

	uncore->nr_units = node;

	ret = perf_pmu_register(pmu, pmu->name, -1);
	if (ret)
		goto fail;

	uncore->pmu = pmu;
	return 0;

fail:
	for (node = 0; node < MAX_NR_UNCORE_PDEVS; node++) {
		pdev = uncore->pdevs[node].pdev;
		if (!pdev)
			break;
		iounmap(uncore->pdevs[node].map);
		pci_dev_put(pdev);
	}
	return ret;
}

static int __init thunder_uncore_init(void)
{
	unsigned long implementor = read_cpuid_implementor();
	unsigned long part_number = read_cpuid_part_number();
	u32 variant;

	if (implementor != ARM_CPU_IMP_CAVIUM ||
	    part_number != CAVIUM_CPU_PART_THUNDERX)
		return -ENODEV;

	/* detect pass2 which contains different counters */
	variant = MIDR_VARIANT(read_cpuid_id());
	if (variant == 1)
		thunder_uncore_version = 1;
	pr_info("PMU version: %d\n", thunder_uncore_version);

	thunder_uncore_l2c_tad_setup();
	thunder_uncore_l2c_cbc_setup();
	return 0;
}
late_initcall(thunder_uncore_init);
