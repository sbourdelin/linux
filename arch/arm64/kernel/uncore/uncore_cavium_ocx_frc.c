/*
 * Cavium Thunder uncore PMU support, OCX FRC counters.
 *
 * Copyright 2016 Cavium Inc.
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

#ifndef PCI_DEVICE_ID_THUNDER_OCX
#define PCI_DEVICE_ID_THUNDER_OCX		0xa013
#endif

#define OCX_FRC_NR_COUNTERS			4
#define OCX_FRC_NR_UNITS			6
#define OCX_FRC_UNIT_OFFSET			0x8
#define OCX_FRC_COUNTER_OFFSET			0xfa00
#define OCX_FRC_CONTROL_OFFSET			0xff00
#define OCX_FRC_COUNTER_INC			0x80
#define OCX_FRC_EVENT_MASK			0x1fffff
#define OCX_FRC_STAT_CONTROL_BIT		37

/* OCX FRC event list */
#define OCX_FRC_EVENT_STAT0			0x0
#define OCX_FRC_EVENT_STAT1			0x1
#define OCX_FRC_EVENT_STAT2			0x2
#define OCX_FRC_EVENT_STAT3			0x3

struct thunder_uncore *thunder_uncore_ocx_frc;

static inline void __iomem *map_offset_ocx_frc(unsigned long addr,
				struct thunder_uncore *uncore, int unit)
{
	return (void __iomem *) (addr +
				 uncore->pdevs[0].map +
				 unit * OCX_FRC_UNIT_OFFSET);
}

/*
 * Summarize counters across all FRC's. Different from the other uncore
 * PMUs because all FRC's are on one PCI device.
 */
static void thunder_uncore_read_ocx_frc(struct perf_event *event)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new, sum = 0;
	s64 delta;
	int i;

	/*
	 * since we do not enable counter overflow interrupts,
	 * we do not have to worry about prev_count changing on us
	 */

	prev = local64_read(&hwc->prev_count);

	/* read counter values from all units */
	for (i = 0; i < OCX_FRC_NR_UNITS; i++) {
		new = readq(map_offset_ocx_frc(hwc->event_base, uncore, i));
		sum += new & OCX_FRC_EVENT_MASK;
	}

	local64_set(&hwc->prev_count, new);
	delta = new - prev;
	local64_add(delta, &event->count);
}

static void thunder_uncore_start(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, ctl;
	int i;

	/* restore counter value divided by units into all counters */
	if (flags & PERF_EF_RELOAD) {
		prev = local64_read(&hwc->prev_count);
		prev = (prev / uncore->nr_units) & OCX_FRC_EVENT_MASK;
		for (i = 0; i < uncore->nr_units; i++)
			writeq(prev, map_offset_ocx_frc(hwc->event_base,
							uncore, i));
	}


	hwc->state = 0;

	/* enable counters */
	ctl = readq(hwc->config_base + uncore->pdevs[0].map);
	ctl |= 1ULL << OCX_FRC_STAT_CONTROL_BIT;
	writeq(ctl, hwc->config_base + uncore->pdevs[0].map);

	perf_event_update_userpage(event);
}

static void thunder_uncore_stop(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	u64 ctl;

	/* disable counters */
	ctl = readq(hwc->config_base + uncore->pdevs[0].map);
	ctl &= ~(1ULL << OCX_FRC_STAT_CONTROL_BIT);
	writeq(ctl, hwc->config_base + uncore->pdevs[0].map);

	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunder_uncore_read_ocx_frc(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int thunder_uncore_add(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	int i;

	WARN_ON_ONCE(!uncore);

	/* are we already assigned? */
	if (hwc->idx != -1 && uncore->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < uncore->num_counters; i++) {
		if (uncore->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	/* counters are 1:1 */
	hwc->idx = -1;
	if (cmpxchg(&uncore->events[hwc->config], NULL, event) == NULL)
		hwc->idx = hwc->config;

out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = OCX_FRC_CONTROL_OFFSET - OCX_FRC_COUNTER_OFFSET;
	hwc->event_base = hwc->idx * OCX_FRC_COUNTER_INC;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		thunder_uncore_start(event, PERF_EF_RELOAD);

	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-1");

static struct attribute *thunder_ocx_frc_format_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group thunder_ocx_frc_format_group = {
	.name = "format",
	.attrs = thunder_ocx_frc_format_attr,
};

EVENT_ATTR(stat0,	OCX_FRC_EVENT_STAT0);
EVENT_ATTR(stat1,	OCX_FRC_EVENT_STAT1);
EVENT_ATTR(stat2,	OCX_FRC_EVENT_STAT2);
EVENT_ATTR(stat3,	OCX_FRC_EVENT_STAT3);

static struct attribute *thunder_ocx_frc_events_attr[] = {
	EVENT_PTR(stat0),
	EVENT_PTR(stat1),
	EVENT_PTR(stat2),
	EVENT_PTR(stat3),
	NULL,
};

static struct attribute_group thunder_ocx_frc_events_group = {
	.name = "events",
	.attrs = thunder_ocx_frc_events_attr,
};

static const struct attribute_group *thunder_ocx_frc_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_ocx_frc_format_group,
	&thunder_ocx_frc_events_group,
	NULL,
};

struct pmu thunder_ocx_frc_pmu = {
	.attr_groups	= thunder_ocx_frc_attr_groups,
	.name		= "thunder_ocx_frc",
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read_ocx_frc,
};

static int event_valid(u64 config)
{
	if (config <= OCX_FRC_EVENT_STAT3)
		return 1;
	else
		return 0;
}

int __init thunder_uncore_ocx_frc_setup(void)
{
	int ret;

	thunder_uncore_ocx_frc = kzalloc(sizeof(struct thunder_uncore),
					 GFP_KERNEL);
	if (!thunder_uncore_ocx_frc) {
		ret = -ENOMEM;
		goto fail_nomem;
	}

	ret = thunder_uncore_setup(thunder_uncore_ocx_frc,
			PCI_DEVICE_ID_THUNDER_OCX, OCX_FRC_COUNTER_OFFSET,
			OCX_FRC_CONTROL_OFFSET - OCX_FRC_COUNTER_OFFSET
				+ sizeof(unsigned long long),
			&thunder_ocx_frc_pmu);
	if (ret)
		goto fail;

	thunder_uncore_ocx_frc->type = OCX_FRC_TYPE;
	thunder_uncore_ocx_frc->num_counters = OCX_FRC_NR_COUNTERS;
	thunder_uncore_ocx_frc->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_ocx_frc);
fail_nomem:
	return ret;
}
