// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
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

/*
 * This driver adds support for perf events to monitor the DDR
 * bandwidth in Qualcomm Technologies chips. Each switch in the
 * interconnect is connected to tthe memory controller and contains a
 * performace monitoring unit (PMU) that the driver exposes
 * through the perf events framework.
 *
 * The PMU Event Counters
 * - Event counters, which count occurrences of a configured event.
 *
 * These resources are exposed as perf counting events, there is no
 * support for sampling based on events exposed by the driver. Event
 * counters are always accumulating.
 * Events associated with event counters are the following:
 * ddr-read-bytes: The driver scales the raw pmu count to provide the
 * number of bytes read from a specific memory controller.
 *
 * ddr-write-bytes: The driver scales the raw pmu count to provide the
 * number of bytes read from a specific memory controller.
 *
 */

#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/acpi.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include "qcom_bandwidth_perf_events.h"



/*
 * Structures representing a HW PMU and other associated resources
 */

/*
 * Represents an event counter
 *
 * This type is used to make these operations polymorphic depending on the
 * type of hardware resources an event uses. The general idea is to associate
 * a perf_event with a switch_pmu_counter via the index contained in its
 * hw_perf_event. To accomplish this, an array of switch_pmu_counters is used
 * and event counters use the  BANDWIDTH_NUM_EVENT_COUNTERS indexes, so the
 *   event counter index is found by the using the index directly:
 */
struct switch_pmu_counter {
	struct perf_event	*event;
	/* Called to start event monitoring */
	void (*start)(struct perf_event *event);
	/* Called to stop event monitoring (optional) */
	void (*stop)(struct perf_event *event, int flags);
	/* Called when the counter overflows (optional) */
	void (*wrap)(struct perf_event *event);
	/* Called to update the perf_event */
	void (*update)(struct perf_event *event);
};

/*
 * Represents the hardware PMU
 *
 * This type inherits from the core perf events struct pmu and adds data
 * to manage the PMU resources.
 */
struct switch_pmu {
	/* Base perf pmu */
	struct pmu	perf_pmu;
	/* CPU mask exported for user space tools via sysfs */
	cpumask_t       cpu;
	/* Node for the hotplug notifier hlist */
	struct hlist_node     node;
	/* Register base address */
	void __iomem	*regs;
	/* Spinlock used to protect indexed accesses to event counters */
	raw_spinlock_t	ecsel_lock;

	/* Bitmap to track counter use */
	unsigned long	used_mask[BITS_TO_LONGS(BANDWIDTH_NUM_TOTAL_COUNTERS)];
	/* Counter resources */
	struct switch_pmu_counter counters[BANDWIDTH_NUM_TOTAL_COUNTERS];
};

#define FIRST_EVENT_COUNTER 0

#define to_switch_pmu(p) (container_of(p, struct switch_pmu, perf_pmu))

static int cpuhp_state_num;

/*
 * Decoding of settings from perf_event_attr
 *
 * Common bits:
 *
 * The config format for perf events associated with event counters is:
 * - config: bits 0-3:event selector, bits 16-22:source selector
 * - config1: bits 0-21,24-30:filter config, bits 32-45,48-54:filter enable
 *
 */

#define PERF_EVENT_ATTR_EXTRACTOR(_name, _config, _size, _shift)	\
	static inline u32 get_##_name(struct perf_event *event)         \
	{								\
		return (event->attr._config >> _shift)                  \
			& GENMASK(_size - 1, 0);                            \
	}

PERF_EVENT_ATTR_EXTRACTOR(ec_event_sel, config, 4, 0);
PERF_EVENT_ATTR_EXTRACTOR(ec_event_lc, config, 1, 32);
PERF_EVENT_ATTR_EXTRACTOR(ec_source_sel, config, 7, 16);


/*
 * Implementation of global HW PMU operations
 */

static inline int event_num_counters(struct perf_event *event)
{
	return (get_ec_event_lc(event) == 0) ? 1 : 2;
}

static
bool switch_pmu__inuse(struct switch_pmu *pmu)
{
	/* Check if a given PMU is already in use by IMC */
	return readl_relaxed(pmu->regs + BANDWIDTH_EC_ENABLE_SET) == 0xF000;

}

static
void switch_pmu__reset(struct switch_pmu *pmu)
{
	u32 all = GENMASK(BANDWIDTH_NUM_EVENT_COUNTERS - 1, 0);

	if (!switch_pmu__inuse(pmu)) {
		/* Enable access by writing the LAR key */
		writel_relaxed(BANDWIDTH_LAR_KEY, pmu->regs + BANDWIDTH_LAR);


		/* Disable IRQonMSB */

		writel_relaxed(0x0, pmu->regs + BANDWIDTH_EC_IRQ_CONTROL);

		/*
		 * Assert reset to the EC hardware, use writel to ensure the
		 * CLEAR commands have been seen by the device before this
		 * write.
		 */

		writel(SET(GLOBAL_RESET, 1), pmu->regs +
			BANDWIDTH_EC_GLOBAL_CONTROL);

		/*
		 * De-assert reset to the EC hardware, use writel to ensure
		 *  the reset command has been seen by the device.
		 */

		writel(SET(GLOBAL_RESET, 0), pmu->regs +
			BANDWIDTH_EC_GLOBAL_CONTROL);
		writel(SET(RETRIEVAL_MODE, 1)
			| SET(GLOBAL_ENABLE, 1) | SET(GLOBAL_TRIGOVRD, 1),
			pmu->regs + BANDWIDTH_EC_GLOBAL_CONTROL);
	}

	/* clear the interuppts and event counters */
	writel_relaxed(all, pmu->regs + BANDWIDTH_EC_ENABLE_CLEAR);
	writel_relaxed(all, pmu->regs + BANDWIDTH_EC_INTERRUPT_ENABLE_CLEAR);
};

/*
 * Event counter operations
 */

static inline
void switch_pmu__ec_set_event(struct switch_pmu *pmu, u8 cntr, u32 event)
{

	writel_relaxed(event, pmu->regs + qcom_bandwidth_ec_source_sel(cntr));
}

static inline
void switch_pmu__ec_enable(struct switch_pmu *pmu, u32 cntr)
{
	writel_relaxed(SET(ECENSET(cntr), 1), pmu->regs +
		BANDWIDTH_EC_ENABLE_SET);
}

static inline
void switch_pmu__ec_disable(struct switch_pmu *pmu, u32 cntr)
{
	writel_relaxed(SET(ECENSET(cntr), 1),
		    pmu->regs + BANDWIDTH_EC_ENABLE_CLEAR);
}

static inline
void switch_pmu__ec_enable_interrupt(struct switch_pmu *pmu, u32 cntr)
{
	u32 val = readl_relaxed(pmu->regs + BANDWIDTH_EC_IRQ_CONTROL);

	writel_relaxed(val | BIT(cntr), pmu->regs + BANDWIDTH_EC_IRQ_CONTROL);
	writel_relaxed(SET(ECINTENCLR(cntr), 1),
		    pmu->regs + BANDWIDTH_EC_INTERRUPT_ENABLE_SET);
}

static inline
void switch_pmu__ec_disable_interrupt(struct switch_pmu *pmu, u32 cntr)
{
	u32 val = readl_relaxed(pmu->regs + BANDWIDTH_EC_IRQ_CONTROL);

	writel(val & ~BIT(cntr), pmu->regs + BANDWIDTH_EC_IRQ_CONTROL);
	writel(SET(ECINTENCLR(cntr), 1),
		    pmu->regs + BANDWIDTH_EC_INTERRUPT_ENABLE_CLEAR);
}

static inline
u32 switch_pmu__ec_read_ovsr(struct switch_pmu *pmu)
{
	return readl_relaxed(pmu->regs + BANDWIDTH_EC_OVF_STATUS);
}

static inline
void switch_pmu__ec_write_ovsr(struct switch_pmu *pmu, u32 value)
{
	writel_relaxed(value, pmu->regs + BANDWIDTH_EC_OVF_STATUS);
}

static inline
bool switch_pmu__any_event_counter_overflowed(u32 ovsr)
{
	return (ovsr & GENMASK(BANDWIDTH_NUM_EVENT_COUNTERS - 1, 0)) != 0;
}

static inline
int switch_pmu__ec_has_overflowed(u32 ovsr, u8 cntr)
{
	return GET(ECOVF(cntr), ovsr) != 0;
}

static inline
void switch_pmu__ec_set_value(struct switch_pmu *pmu, u8 cntr, u32 value)
{
	unsigned long flags;
	bool reenable = false;

	/*
	 * Quirk: The counter needs to be disabled before updating.
	 */
	if ((readl_relaxed(pmu->regs + BANDWIDTH_EC_ENABLE_SET) &
		    SET(ECENSET(cntr), 1)) != 0) {
		switch_pmu__ec_disable(pmu, cntr);
		reenable = true;
	}

	raw_spin_lock_irqsave(&pmu->ecsel_lock, flags);
	writel_relaxed(SET(ECSEL, cntr), pmu->regs + BANDWIDTH_EC_COUNTER_SEL);

	/*
	 * Use writel because the write to BANDWIDTH_EC_COUNTER_SEL needs
	 * to be observed before the write to BANDWIDTH_EC_COUNT.
	 */

	writel(value, pmu->regs + BANDWIDTH_EC_COUNT);
	raw_spin_unlock_irqrestore(&pmu->ecsel_lock, flags);

	if (reenable)
		switch_pmu__ec_enable(pmu, cntr);
}

static inline
u32 switch_pmu__ec_get_value(struct switch_pmu *pmu, u8 cntr)
{
	u32 result;
	u32 sel;
	unsigned long flags;
	unsigned long num_attempts = 0;

	do {
		raw_spin_lock_irqsave(&pmu->ecsel_lock, flags);
		writel_relaxed(SET(ECSEL, cntr), pmu->regs +
			BANDWIDTH_EC_COUNTER_SEL);

		/*
		 * The write to BANDWIDTH_EC_COUNTER_SEL needs to be observed
		 * before the read to BANDWIDTH_EC_COUNT.
		 */
		mb();

		result = readl_relaxed(pmu->regs + BANDWIDTH_EC_COUNT);
		raw_spin_unlock_irqrestore(&pmu->ecsel_lock, flags);
		num_attempts++;
		sel = readl_relaxed(pmu->regs + BANDWIDTH_EC_COUNTER_SEL);
	} while ((sel != SET(ECSEL, cntr))
		&& (num_attempts <= DDRBW_MAX_RETRIES));

	/* Exit gracefully to avoid freeze */
	if (num_attempts >= DDRBW_MAX_RETRIES)
		return DDR_BW_READ_FAIL;

	return result;
}

static inline
bool switch_pmu__any_event_counter_active(struct switch_pmu *pmu)
{
	int idx = find_next_bit(pmu->used_mask, BANDWIDTH_NUM_TOTAL_COUNTERS,
		    FIRST_EVENT_COUNTER);

	return idx != BANDWIDTH_NUM_TOTAL_COUNTERS;
}

/*
 * Event counter switch_pmu_counter method implementation.
 */

static
void switch_pmu__32bit_event_counter_update(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;
	u32 delta, prev, now;

	do {
		prev = (u32)local64_read(&event->hw.prev_count);
		now = switch_pmu__ec_get_value(pmu, ec_idx);
	} while (local64_cmpxchg(&event->hw.prev_count, prev, now) != prev);

	delta = now - prev;
	local64_add(delta, &event->count);
}

static
void switch_pmu__64bit_event_counter_update(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	int idx = event->hw.idx - FIRST_EVENT_COUNTER;
	u32 hi, lo;
	u64 prev, now;

	do {
		prev = local64_read(&event->hw.prev_count);
		do {
			hi = switch_pmu__ec_get_value(pmu, idx + 1);
			lo = switch_pmu__ec_get_value(pmu, idx);
		} while (hi != switch_pmu__ec_get_value(pmu, idx + 1));
		now = ((u64)hi << 32) | lo;
	} while (local64_cmpxchg(&event->hw.prev_count, prev, now) != prev);

	local64_add(now - prev, &event->count);
}

static
void switch_pmu__event_counter_program(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);

	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;
	u32 ev_type = SET(ECSOURCESEL, get_ec_source_sel(event)) |
		    SET(ECEVENTSEL, get_ec_event_sel(event));

	event->hw.state = 0;

	local64_set(&event->hw.prev_count, 0);
	switch_pmu__ec_set_value(pmu, ec_idx, 0);
	switch_pmu__ec_set_event(pmu, ec_idx, ev_type);
}

static
void enable_64bit_ganging(struct perf_event *event, u32 idx)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);

	/* according to errata doc, this needs to be done
	 * for the odd counter
	 */
	u16 gang_regs;
	u32 ev_type = SET(ECSOURCESEL, 0x0) | SET(ECEVENTSEL, 0xf);

	switch_pmu__ec_set_event(pmu, idx, ev_type);

	/* enable ganging RMW */
	gang_regs = readl_relaxed(pmu->regs + BANDWIDTH_EC_GANG);
	gang_regs |= BIT(idx);
	writel_relaxed(gang_regs, pmu->regs + BANDWIDTH_EC_GANG);

}

static
void disable_64bit_ganging(struct perf_event *event, u32 idx)
{
	u16 gang_regs;
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);

	gang_regs = readl_relaxed(pmu->regs + BANDWIDTH_EC_GANG);
	gang_regs = gang_regs & ~BIT(idx);
	writel_relaxed(gang_regs, pmu->regs + BANDWIDTH_EC_GANG);

}
static
void switch_pmu_event_32bit_counter_start(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;

	switch_pmu__event_counter_program(pmu->counters[ec_idx].event);
	switch_pmu__ec_enable_interrupt(pmu, ec_idx);
	switch_pmu__ec_enable(pmu, ec_idx);
}

static
void switch_pmu_event_64bit_counter_start(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;

	switch_pmu__event_counter_program(pmu->counters[ec_idx].event);
	enable_64bit_ganging(event, ec_idx + 1);
	switch_pmu__ec_enable(pmu, ec_idx);
	switch_pmu__ec_enable(pmu, ec_idx + 1);
}

static
void switch_pmu_event_32bit_counter_stop(struct perf_event *event, int flags)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;

	switch_pmu__ec_disable_interrupt(pmu, ec_idx);
	switch_pmu__ec_disable(pmu, ec_idx);
}

static
void switch_pmu_event_64bit_counter_stop(struct perf_event *event, int flags)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	u32 ec_idx = event->hw.idx - FIRST_EVENT_COUNTER;

	switch_pmu__ec_disable_interrupt(pmu, ec_idx);
	switch_pmu__ec_disable(pmu, ec_idx);
	switch_pmu__ec_disable_interrupt(pmu, ec_idx + 1);
	switch_pmu__ec_disable(pmu, ec_idx + 1);
	disable_64bit_ganging(event, ec_idx + 1);
}

static
void switch_pmu_event_32bit_counter_wrap(struct perf_event *event)
{
	switch_pmu__32bit_event_counter_update(event);
}

/*
 * Core abstract PMU functions and management of the software counters.
 */

static
void switch_pmu__nop(struct pmu *perf_pmu)
{
}

static
int switch_pmu__reserve_event_counter(struct switch_pmu *pmu,
		    struct perf_event *event, int sz)
{
	int idx;

	idx = bitmap_find_free_region(pmu->used_mask,
	    BANDWIDTH_NUM_TOTAL_COUNTERS, sz);
	if (idx < 0)
		return -EAGAIN;
	return idx;
}

/*
 * We must NOT create groups containing events from multiple hardware PMUs,
 * although mixing different software and hardware PMUs is allowed.
 */
static bool switch_pmu__validate_event_group(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader;
	struct perf_event *sibling;
	int counters = 0;

	if (leader->pmu != event->pmu && !is_software_event(leader))
		return false;

	counters = event_num_counters(event);
	counters += event_num_counters(leader);

	for_each_sibling_event(sibling, leader) {
		if (is_software_event(sibling))
			continue;
		if (sibling->pmu != event->pmu)
			return false;
		counters += event_num_counters(sibling);
	}

	/*
	 * If the group requires more counters than the HW has, it
	 * cannot ever be scheduled.
	 */
	return counters <= BANDWIDTH_NUM_TOTAL_COUNTERS;
}
static
int switch_pmu__event_init(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	/*
	 * Is the event for this PMU?
	 */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * We cannot filter accurately so we just don't allow it at all.
	 */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
		    event->attr.exclude_hv || event->attr.exclude_idle)
		return -EINVAL;

	hwc->idx = -1;

	/* Sampling not supported these are system counters/events */
	if (hwc->sample_period)
		return -EINVAL;

	/*
	 * Task mode not available, these are system counters not attributable
	 * to any CPU and therefore cannot attribute per-task.
	 */
	if (event->cpu < 0)
		return -EINVAL;
	/* set event cpu to the cpumask*/
	event->cpu = cpumask_first(&pmu->cpu);

	/* Validate the group */
	if (!switch_pmu__validate_event_group(event))
		return -EINVAL;

	return 0;
}

static
int switch_pmu__event_add(struct perf_event *event, int flags)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx;
	int err = 0;
	int sz;

	sz = get_ec_event_lc(event);

	/* Try to find a hardware resource for this event */
	idx = switch_pmu__reserve_event_counter(pmu, event, sz);
	if (idx < 0) {
		err = idx;
		goto out;
	}

	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;

	if (sz == 0) {
		pmu->counters[idx] = (struct switch_pmu_counter) {
			.event = event,
			.start = switch_pmu_event_32bit_counter_start,
			.stop = switch_pmu_event_32bit_counter_stop,
			.wrap = switch_pmu_event_32bit_counter_wrap,
			.update = switch_pmu__32bit_event_counter_update,
		};

	} else {
		pmu->counters[idx] = (struct switch_pmu_counter) {
			.event = event,
			.start = switch_pmu_event_64bit_counter_start,
			.stop = switch_pmu_event_64bit_counter_stop,
			.update = switch_pmu__64bit_event_counter_update,
			.wrap = NULL
		};
		pmu->counters[idx + 1] = pmu->counters[idx];
	}

	if (flags & PERF_EF_START)
		pmu->counters[idx].start(pmu->counters[idx].event);

out:
	return err;
}

static
void switch_pmu__event_start(struct perf_event *event, int flags)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	pmu->counters[hwc->idx].start(pmu->counters[hwc->idx].event);
}

static
void switch_pmu__event_stop(struct perf_event *event, int flags)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct switch_pmu_counter *c = &pmu->counters[hwc->idx];


	if (!(hwc->state & PERF_HES_STOPPED)) {
		if (c->stop)
			c->stop(c->event, flags);

		if (flags & PERF_EF_UPDATE)
			c->update(c->event);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static
void switch_pmu__event_del(struct perf_event *event, int flags)
{

	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct switch_pmu_counter *c = &pmu->counters[hwc->idx];
	struct switch_pmu_counter *cl = &pmu->counters[hwc->idx + 1];
	int sz;

	sz = get_ec_event_lc(event);

	if (c->stop)
		c->stop(c->event, flags | PERF_EF_UPDATE);
	c->update(c->event);
	c->event = NULL;
	bitmap_release_region(pmu->used_mask, hwc->idx, sz);

	/* Null set the upper counter when the long counter was enabled*/
	if (sz)
		cl->event = NULL;
}


static
void switch_pmu__event_read(struct perf_event *event)
{
	struct switch_pmu *pmu = to_switch_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	pmu->counters[hwc->idx].update(pmu->counters[hwc->idx].event);
}

static
int dummy_event_idx(struct perf_event *event)
{
	return 0;
}

static
bool switch_pmu__ec_handle_irq(struct switch_pmu *pmu)
{
	bool handled = false;
	u32 ovs = switch_pmu__ec_read_ovsr(pmu);
	int idx;

	switch_pmu__ec_write_ovsr(pmu, ovs);

	if (!switch_pmu__any_event_counter_overflowed(ovs))
		return handled;

	for (idx = 0; idx < BANDWIDTH_NUM_EVENT_COUNTERS; ++idx) {
		struct switch_pmu_counter *counter;

		if (!switch_pmu__ec_has_overflowed(ovs, idx))
			continue;
		counter = &pmu->counters[idx + FIRST_EVENT_COUNTER];
		if (!counter->event)
			continue;
		counter->wrap(counter->event);
		handled = true;
	}

	return handled;
}


static
irqreturn_t switch_pmu__handle_irq(int irq_num, void *data)
{
	bool handled = false;
	struct switch_pmu *pmu = data;

	if (switch_pmu__any_event_counter_active(pmu))
		handled = switch_pmu__ec_handle_irq(pmu);

	/*
	 * Handle the pending perf events.
	 *
	 * Note: this call *must* be run with interrupts disabled. For
	 * platforms that can have the PMU interrupts raised as an NMI, this
	 * will not work.
	 */

	irq_work_run();

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/*
 * Fixed attribute groups exposed for perf in the format group.
 *
 * The config format for perf events associated with event counters is:
 * - config: bits 0-3:event selector, bits 16-22:source selector
 * - config1: bits 0-21,24-30:filter config, bits 32-45,48-54:filter enable
 *
 */

/* Event counters */

#define DDRBW_ATTR(_name, _str)					\
	(&((struct perf_pmu_events_attr[]){				\
	   {.attr = __ATTR(_name, 0444, perf_event_sysfs_show, NULL),\
	.id = 0,						\
	.event_str = _str }					\
	})[0].attr.attr)


static struct attribute *qcom_bandwidth_pmu_formats[] = {
	DDRBW_ATTR(ecsourcesel, "config:16-22"),
	DDRBW_ATTR(eceventsel, "config:0-3"),
	DDRBW_ATTR(lc, "config:32"),
	NULL,
};

static struct attribute_group qcom_bandwidth_pmu_format_group = {
	.name = "format",
	.attrs = qcom_bandwidth_pmu_formats,
};

static ssize_t qcom_bandwidth_pmu_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct switch_pmu *pmu = to_switch_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &pmu->cpu);
}

static struct device_attribute qcom_bandwidth_pmu_cpumask_attr =
	__ATTR(cpumask, 0444, qcom_bandwidth_pmu_cpumask_show, NULL);

static struct attribute *qcom_bandwidth_pmu_cpumask_attrs[] = {
	&qcom_bandwidth_pmu_cpumask_attr.attr,
	NULL,
};

static struct attribute_group qcom_bandwidth_pmu_cpumask_attr_group = {
	.attrs = qcom_bandwidth_pmu_cpumask_attrs,
};


static struct attribute *qcom_ddrbw_pmu_events[] = {
	DDRBW_ATTR(ddr-read-beats, "ecsourcesel=0x14, eceventsel=0"),
	DDRBW_ATTR(ddr-read-beats.unit, "Bytes"),
	DDRBW_ATTR(ddr-read-beats.scale, "32"),
	DDRBW_ATTR(ddr-write-beats, "ecsourcesel=0x15, eceventsel=0"),
	DDRBW_ATTR(ddr-write-beats.unit, "Bytes"),
	DDRBW_ATTR(ddr-write-beats.scale, "32"),
	NULL
};

static struct attribute_group qcom_bandwidth_pmu_events_group = {
	.name = "events",
	.attrs = qcom_ddrbw_pmu_events,
};

static const struct attribute_group **init_attribute_groups(void)
{
	static const struct attribute_group *result[4];

	result[0] = &qcom_bandwidth_pmu_format_group;
	result[1] = &qcom_bandwidth_pmu_cpumask_attr_group;
	result[2] = &qcom_bandwidth_pmu_events_group;
	result[3] = NULL;
	return result;
}

static const struct attribute_group **attr_groups;

/*
 * Device probing and initialization.
 */

static int qcom_bandwidth_pmu_offline_cpu(unsigned int cpu,
	struct hlist_node *node)
{
	struct switch_pmu *pmu = hlist_entry_safe(node,
		struct switch_pmu, node);
	unsigned int target;

	if (!cpumask_test_and_clear_cpu(cpu, &pmu->cpu))
		return 0;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;
	perf_pmu_migrate_context(&pmu->perf_pmu, cpu, target);
	cpumask_set_cpu(target, &pmu->cpu);
	return 0;
}

static const struct acpi_device_id qcom_bandwidth_pmu_acpi_match[] = {
	{ "QCOM80C1", },
	{ }
};

MODULE_DEVICE_TABLE(acpi, qcom_bandwidth_pmu_acpi_match);

static int qcom_bandwidth_pmu_probe(struct platform_device *pdev)
{
	int result, irq, err;
	struct resource *regs_rc;
	struct switch_pmu *pmu;
	unsigned long uid;
	struct acpi_device *device;
	char *name;

	regs_rc = platform_get_resource(pdev, IORESOURCE_MEM, RES_BW);

	name = devm_kzalloc(&pdev->dev, DDRBW_PMU_NAME_LEN, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	pmu = devm_kzalloc(&pdev->dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;

	*pmu = (struct switch_pmu) {
		.perf_pmu = {
			/* Tag this as a SW context to disable multiplexing */
			.task_ctx_nr	= perf_invalid_context,

			.pmu_enable	= switch_pmu__nop,
			.pmu_disable	= switch_pmu__nop,
			.event_init	= switch_pmu__event_init,
			.add		= switch_pmu__event_add,
			.del		= switch_pmu__event_del,
			.start		= switch_pmu__event_start,
			.stop		= switch_pmu__event_stop,
			.read		= switch_pmu__event_read,

			.event_idx	= dummy_event_idx,

			.attr_groups	= attr_groups
		},
		.counters = {
			[0 ... BANDWIDTH_NUM_TOTAL_COUNTERS - 1] {}
		}
	};

	raw_spin_lock_init(&pmu->ecsel_lock);

	pmu->regs = devm_ioremap_resource(&pdev->dev, regs_rc);
	if (IS_ERR(pmu->regs)) {
		dev_err(&pdev->dev, "Can't map regs @%pa!\n",
		    &regs_rc->start);
		return PTR_ERR(pmu->regs);
	}

	irq = platform_get_irq(pdev, IRQ_BW);
	if (irq <= 0) {
		dev_err(&pdev->dev, "Failed to get valid irq\n");
		return -ENODEV;
	}

	if (acpi_bus_get_device(ACPI_HANDLE(&pdev->dev), &device))
		return -ENODEV;

	if (kstrtol(device->pnp.unique_id, 10, &uid) < 0) {
		dev_err(&pdev->dev, "unable to read ACPI uid\n");
		return -ENODEV;
	}

	snprintf(name, DDRBW_PMU_NAME_LEN, DDRBW_PMU_NAME_FORMAT, uid);
	pmu->perf_pmu.name = name;

	err = devm_request_irq(&pdev->dev, irq, switch_pmu__handle_irq,
		    IRQF_NOBALANCING, pmu->perf_pmu.name, pmu);
	if (err) {
		dev_err(&pdev->dev, "Unable to request IRQ%d\n", irq);
		return err;
	}

	/* Designate the probing CPU as the context for the PMU */
	cpumask_set_cpu(smp_processor_id(), &pmu->cpu);

	switch_pmu__reset(pmu);
	result = perf_pmu_register(&pmu->perf_pmu, pmu->perf_pmu.name, -1);

	if (result < 0) {
		dev_err(&pdev->dev, "Failed to register(%d)\n", result);
		return result;
	}
	dev_info(&pdev->dev, "Registered %s PMU, type: %d\n",
		pmu->perf_pmu.name, pmu->perf_pmu.type);

	/* Add this instance to the list used by the offline callback */
	cpuhp_state_add_instance_nocalls(cpuhp_state_num, &pmu->node);

	platform_set_drvdata(pdev, pmu);

	return result;
}

static int qcom_bandwidth_pmu_remove(struct platform_device *pdev)
{
	struct switch_pmu *pmu = platform_get_drvdata(pdev);

	cpuhp_state_remove_instance_nocalls(cpuhp_state_num, &pmu->node);
	perf_pmu_unregister(&pmu->perf_pmu);
	return 0;
}

static struct platform_driver qcom_bandwidth_pmu_driver = {
	.driver = {
		.name = "qcom-bandwidth-pmu-v1",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(qcom_bandwidth_pmu_acpi_match),
	},
	.probe = qcom_bandwidth_pmu_probe,
	.remove = qcom_bandwidth_pmu_remove,
};

static int __init register_qcom_bandwidth_pmu_driver(void)
{
	if (attr_groups == NULL)
		attr_groups = init_attribute_groups();

	/* Install a hook to update the context CPU in case it goes offline */
	cpuhp_state_num = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
		"perf/qcom/msw:online", NULL, qcom_bandwidth_pmu_offline_cpu);
	if (cpuhp_state_num < 0)
		return cpuhp_state_num;

	return platform_driver_register(&qcom_bandwidth_pmu_driver);
}

static void __exit unregister_qcom_bandwidth_pmu_driver(void)
{
	cpuhp_remove_multi_state(cpuhp_state_num);
	platform_driver_unregister(&qcom_bandwidth_pmu_driver);
}

module_init(register_qcom_bandwidth_pmu_driver);
module_exit(unregister_qcom_bandwidth_pmu_driver);
MODULE_LICENSE("GPL v2");
