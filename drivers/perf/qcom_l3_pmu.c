/* Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/acpi.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>

/*
 * Driver for the L3 cache PMUs in Qualcomm Technologies chips.
 *
 * The driver supports a distributed cache architecture where the overall
 * cache for a socket is comprised of multiple slices each with its own PMU.
 * The driver aggregates counts across the whole socket to provide a global
 * picture of the metrics selected by the user.
 * Access to individual PMUs is not necessary/required since all CPUs
 * share all the slices. The particular slice used by a given address
 * is determined by a hardware hashing algorithm based on the target
 * address.
 */

/*
 * General constants
 */

#define L3_NUM_COUNTERS  8
#define L3_MAX_EVTYPE    0xFF

/*
 * Register offsets
 */

/* Perfmon registers */
#define L3_HML3_PM_CR       0x000
#define L3_HML3_PM_EVCNTR(__cntr) (0x420 + ((__cntr) & 0x7) * 8)
#define L3_HML3_PM_CNTCTL(__cntr) (0x120 + ((__cntr) & 0x7) * 8)
#define L3_HML3_PM_EVTYPE(__cntr) (0x220 + ((__cntr) & 0x7) * 8)
#define L3_HML3_PM_FILTRA   0x300
#define L3_HML3_PM_FILTRB   0x308
#define L3_HML3_PM_FILTRC   0x310
#define L3_HML3_PM_FILTRAM  0x304
#define L3_HML3_PM_FILTRBM  0x30C
#define L3_HML3_PM_FILTRCM  0x314

/* Basic counter registers */
#define L3_M_BC_CR         0x500
#define L3_M_BC_SATROLL_CR 0x504
#define L3_M_BC_CNTENSET   0x508
#define L3_M_BC_CNTENCLR   0x50C
#define L3_M_BC_INTENSET   0x510
#define L3_M_BC_INTENCLR   0x514
#define L3_M_BC_GANG       0x718
#define L3_M_BC_OVSR       0x740
#define L3_M_BC_IRQCTL     0x96C

/*
 * Bit field definitions
 */

/* L3_HML3_PM_CR */
#define PM_CR_RESET           (0)

/* L3_HML3_PM_XCNTCTL/L3_HML3_PM_CNTCTLx */
#define PMCNT_RESET           (0)

/* L3_HML3_PM_EVTYPEx */
#define EVSEL(__val)          ((u32)((__val) & 0xFF))

/* Reset value for all the filter registers */
#define PM_FLTR_RESET         (0)

/* L3_M_BC_CR */
#define BC_RESET              (((u32)1) << 1)
#define BC_ENABLE             ((u32)1)

/* L3_M_BC_SATROLL_CR */
#define BC_SATROLL_CR_RESET   (0)

/* L3_M_BC_CNTENSET */
#define PMCNTENSET(__cntr)    (((u32)1) << ((__cntr) & 0x7))

/* L3_M_BC_CNTENCLR */
#define PMCNTENCLR(__cntr)    (((u32)1) << ((__cntr) & 0x7))
#define BC_CNTENCLR_RESET     (0xFF)

/* L3_M_BC_INTENSET */
#define PMINTENSET(__cntr)    (((u32)1) << ((__cntr) & 0x7))

/* L3_M_BC_INTENCLR */
#define PMINTENCLR(__cntr)    (((u32)1) << ((__cntr) & 0x7))
#define BC_INTENCLR_RESET     (0xFF)

/* L3_M_BC_GANG */
#define GANG_EN(__cntr)       (((u32)1) << ((__cntr) & 0x7))
#define BC_GANG_RESET         (0)

/* L3_M_BC_OVSR */
#define PMOVSRCLR(__cntr)     (((u32)1) << ((__cntr) & 0x7))
#define PMOVSRCLR_RESET       (0xFF)

/* L3_M_BC_IRQCTL */
#define PMIRQONMSBEN(__cntr)  (((u32)1) << ((__cntr) & 0x7))
#define BC_IRQCTL_RESET       (0x0)

/*
 * Events
 */

#define L3_CYCLES		0x01
#define L3_READ_HIT		0x20
#define L3_READ_MISS		0x21
#define L3_READ_HIT_D		0x22
#define L3_READ_MISS_D		0x23
#define L3_WRITE_HIT		0x24
#define L3_WRITE_MISS		0x25

/*
 * The cache is made-up of one or more slices, each slice has its own PMU.
 * This structure represents one of the hardware PMUs.
 */
struct hml3_pmu {
	struct list_head	entry;
	struct l3cache_pmu	*socket;
	void __iomem		*regs;
	atomic_t		prev_count[L3_NUM_COUNTERS];
};

static void hml3_pmu__reset(struct hml3_pmu *pmu)
{
	int i;

	writel_relaxed(BC_RESET, pmu->regs + L3_M_BC_CR);

	/*
	 * Use writel for the first programming command to ensure the basic
	 * counter unit is stopped before proceeding
	 */
	writel(BC_SATROLL_CR_RESET, pmu->regs + L3_M_BC_SATROLL_CR);

	writel_relaxed(BC_CNTENCLR_RESET, pmu->regs + L3_M_BC_CNTENCLR);
	writel_relaxed(BC_INTENCLR_RESET, pmu->regs + L3_M_BC_INTENCLR);
	writel_relaxed(PMOVSRCLR_RESET, pmu->regs + L3_M_BC_OVSR);
	writel_relaxed(BC_GANG_RESET, pmu->regs + L3_M_BC_GANG);
	writel_relaxed(BC_IRQCTL_RESET, pmu->regs + L3_M_BC_IRQCTL);
	writel_relaxed(PM_CR_RESET, pmu->regs + L3_HML3_PM_CR);

	for (i = 0; i < L3_NUM_COUNTERS; ++i) {
		writel_relaxed(PMCNT_RESET, pmu->regs + L3_HML3_PM_CNTCTL(i));
		writel_relaxed(EVSEL(0), pmu->regs + L3_HML3_PM_EVTYPE(i));
	}

	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRA);
	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRAM);
	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRB);
	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRBM);
	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRC);
	writel_relaxed(PM_FLTR_RESET, pmu->regs + L3_HML3_PM_FILTRCM);
}

static inline void hml3_pmu__init(struct hml3_pmu *pmu, struct l3cache_pmu *s,
				  void __iomem *regs)
{
	pmu->socket = s;
	pmu->regs = regs;
	hml3_pmu__reset(pmu);

	/*
	 * Use writel here to ensure all programming commands are done
	 *  before proceeding
	 */
	writel(BC_ENABLE, pmu->regs + L3_M_BC_CR);
}

static inline void hml3_pmu__enable(struct hml3_pmu *pmu)
{
	writel_relaxed(BC_ENABLE, pmu->regs + L3_M_BC_CR);
}

static inline void hml3_pmu__disable(struct hml3_pmu *pmu)
{
	writel_relaxed(0, pmu->regs + L3_M_BC_CR);
}

static inline void hml3_pmu__counter_set_event(struct hml3_pmu *pmu, u8 cnt,
					       u32 event)
{
	writel_relaxed(EVSEL(event), pmu->regs + L3_HML3_PM_EVTYPE(cnt));
}

static inline void hml3_pmu__counter_set_value(struct hml3_pmu *pmu, u8 cnt,
					       u32 value)
{
	writel_relaxed(value, pmu->regs + L3_HML3_PM_EVCNTR(cnt));
}

static inline u32 hml3_pmu__counter_get_value(struct hml3_pmu *pmu, u8 cnt)
{
	return readl_relaxed(pmu->regs + L3_HML3_PM_EVCNTR(cnt));
}

static inline void hml3_pmu__counter_enable(struct hml3_pmu *pmu, u8 cnt)
{
	writel_relaxed(PMCNTENSET(cnt), pmu->regs + L3_M_BC_CNTENSET);
}

static inline void hml3_pmu__counter_reset_trigger(struct hml3_pmu *pmu, u8 cnt)
{
	writel_relaxed(PMCNT_RESET, pmu->regs + L3_HML3_PM_CNTCTL(cnt));
}

static inline void hml3_pmu__counter_disable(struct hml3_pmu *pmu, u8 cnt)
{
	writel_relaxed(PMCNTENCLR(cnt), pmu->regs + L3_M_BC_CNTENCLR);
}

static inline void hml3_pmu__counter_enable_interrupt(struct hml3_pmu *pmu,
						      u8 cnt)
{
	writel_relaxed(PMINTENSET(cnt), pmu->regs + L3_M_BC_INTENSET);
}

static inline void hml3_pmu__counter_disable_interrupt(struct hml3_pmu *pmu,
						       u8 cnt)
{
	writel_relaxed(PMINTENCLR(cnt), pmu->regs + L3_M_BC_INTENCLR);
}

static inline void hml3_pmu__counter_enable_gang(struct hml3_pmu *pmu, u8 cnt)
{
	u32 value = readl_relaxed(pmu->regs + L3_M_BC_GANG);

	value |= GANG_EN(cnt);
	writel_relaxed(value, pmu->regs + L3_M_BC_GANG);
}

static inline void hml3_pmu__counter_disable_gang(struct hml3_pmu *pmu, u8 cnt)
{
	u32 value = readl_relaxed(pmu->regs + L3_M_BC_GANG);

	value &= ~(GANG_EN(cnt));
	writel_relaxed(value, pmu->regs + L3_M_BC_GANG);
}

static inline void hml3_pmu__counter_enable_irq_on_msb(struct hml3_pmu *pmu,
						       u8 cnt)
{
	u32 value = readl_relaxed(pmu->regs + L3_M_BC_IRQCTL);

	value |= PMIRQONMSBEN(cnt);
	writel_relaxed(value, pmu->regs + L3_M_BC_IRQCTL);
}

static inline void hml3_pmu__counter_disable_irq_on_msb(struct hml3_pmu *pmu,
							u8 cnt)
{
	u32 value = readl_relaxed(pmu->regs + L3_M_BC_IRQCTL);

	value &= ~(PMIRQONMSBEN(cnt));
	writel_relaxed(value, pmu->regs + L3_M_BC_IRQCTL);
}

static inline u32 hml3_pmu__getreset_ovsr(struct hml3_pmu *pmu)
{
	u32 result = readl_relaxed(pmu->regs + L3_M_BC_OVSR);

	writel_relaxed(result, pmu->regs + L3_M_BC_OVSR);
	return result;
}

static inline int hml3_pmu__has_overflowed(u32 ovsr)
{
	return (ovsr & PMOVSRCLR_RESET) != 0;
}

/*
 * Hardware counter interface.
 *
 * This interface allows operations on counters to be polymorphic.
 * The hardware supports counter chaining to allow 64 bit virtual counters.
 * We expose this capability as a config option for each event, that way
 * a user can create perf events that use 32 bit counters for events that
 * increment at a slower rate, and perf events that use 64 bit counters
 * for events that increment faster and avoid IRQs.
 */
struct l3cache_pmu_hwc {
	struct perf_event	*event;
	/* Called to start event monitoring */
	void (*start)(struct perf_event *event);
	/* Called to stop event monitoring */
	void (*stop)(struct perf_event *event, int flags);
	/* Called to update the perf_event */
	void (*update)(struct perf_event *event);
};

/*
 * Decoding of settings from perf_event_attr
 *
 * The config format for perf events is:
 * - config: bits 0-7: event type
 *           bit  32:  HW counter size requested, 0: 32 bits, 1: 64 bits
 */
static inline u32 get_event_type(struct perf_event *event)
{
	return (event->attr.config) & L3_MAX_EVTYPE;
}

static inline int get_hw_counter_size(struct perf_event *event)
{
	return event->attr.config >> 32 & 1;
}

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs, configuring each one in the same way and
 * aggregating events as needed.
 */

struct l3cache_pmu {
	struct pmu		pmu;
	struct hlist_node	node;
	struct list_head	pmus;
	struct l3cache_pmu_hwc	counters[L3_NUM_COUNTERS];
	unsigned long		used_mask[BITS_TO_LONGS(L3_NUM_COUNTERS)];
	cpumask_t		cpu;
};

#define to_l3cache_pmu(p) (container_of(p, struct l3cache_pmu, pmu))

/*
 * 64 bit counter interface implementation.
 */

static void qcom_l3_cache__64bit_counter_start(struct perf_event *event)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;
	int idx = event->hw.idx;
	u64 value = local64_read(&event->count);

	list_for_each_entry(slice, &socket->pmus, entry) {
		hml3_pmu__counter_enable_gang(slice, idx+1);

		if (value) {
			hml3_pmu__counter_set_value(slice, idx+1, value >> 32);
			hml3_pmu__counter_set_value(slice, idx, (u32)value);
			value = 0;
		} else {
			hml3_pmu__counter_set_value(slice, idx+1, 0);
			hml3_pmu__counter_set_value(slice, idx, 0);
		}

		hml3_pmu__counter_set_event(slice, idx+1, 0);
		hml3_pmu__counter_set_event(slice, idx, get_event_type(event));

		hml3_pmu__counter_enable(slice, idx+1);
		hml3_pmu__counter_enable(slice, idx);
	}
}

static void qcom_l3_cache__64bit_counter_stop(struct perf_event *event,
					      int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;
	int idx = event->hw.idx;

	list_for_each_entry(slice, &socket->pmus, entry) {
		hml3_pmu__counter_disable_gang(slice, idx+1);

		hml3_pmu__counter_disable(slice, idx);
		hml3_pmu__counter_disable(slice, idx+1);
	}
}

static u64 qcom_l3_cache__64bit_counter_get_value(struct hml3_pmu *slice,
						  int idx)
{
	u32 hi_old, lo, hi_new;
	int i, retries = 2;

	hi_new = hml3_pmu__counter_get_value(slice, idx+1);
	for (i = 0; i < retries; i++) {
		hi_old = hi_new;
		lo = hml3_pmu__counter_get_value(slice, idx);
		hi_new = hml3_pmu__counter_get_value(slice, idx+1);
		if (hi_old == hi_new)
			break;
	}

	return ((u64)hi_new << 32) | lo;
}

static void qcom_l3_cache__64bit_counter_update(struct perf_event *event)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;
	int idx = event->hw.idx;
	u64 new = 0;

	list_for_each_entry(slice, &socket->pmus, entry)
		new += qcom_l3_cache__64bit_counter_get_value(slice, idx);

	local64_set(&event->count, new);
}

/*
 * 32 bit counter interface implementation
 */

static void qcom_l3_cache__32bit_counter_start(struct perf_event *event)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;
	struct hw_perf_event *hwc = &event->hw;

	slice = list_first_entry(&socket->pmus, struct hml3_pmu, entry);

	list_for_each_entry(slice, &socket->pmus, entry) {
		atomic_set(&slice->prev_count[hwc->idx], 0);
		hml3_pmu__counter_set_value(slice, hwc->idx, 0);
		hml3_pmu__counter_enable_irq_on_msb(slice, hwc->idx);
		hml3_pmu__counter_set_event(slice, hwc->idx,
					    get_event_type(event));
		hml3_pmu__counter_enable_interrupt(slice, hwc->idx);
		hml3_pmu__counter_enable(slice, hwc->idx);
	}
}

static void qcom_l3_cache__32bit_counter_stop(struct perf_event *event,
					      int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;
	struct hw_perf_event *hwc = &event->hw;

	list_for_each_entry(slice, &socket->pmus, entry) {
		hml3_pmu__counter_disable_irq_on_msb(slice, hwc->idx);
		hml3_pmu__counter_disable_interrupt(slice, hwc->idx);
		hml3_pmu__counter_disable(slice, hwc->idx);
	}
}

static void qcom_l3_cache__32bit_counter_update_from_slice(
		struct perf_event *event, struct hml3_pmu *slice, int idx)
{
	u32 delta, prev, now;

	do {
		prev = atomic_read(&slice->prev_count[idx]);
		now = hml3_pmu__counter_get_value(slice, idx);
	} while (atomic_cmpxchg(&slice->prev_count[idx], prev, now) != prev);

	delta = now - prev;
	local64_add(delta, &event->count);
}

static void qcom_l3_cache__32bit_counter_update(struct perf_event *event)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hml3_pmu *slice;

	list_for_each_entry(slice, &socket->pmus, entry)
		qcom_l3_cache__32bit_counter_update_from_slice(event, slice,
							       event->hw.idx);
}

/*
 * Top level PMU functions.
 */

static irqreturn_t qcom_l3_cache__handle_irq(int irq_num, void *data)
{
	struct hml3_pmu *slice = data;
	u32 status;
	int idx;

	status = hml3_pmu__getreset_ovsr(slice);
	if (!hml3_pmu__has_overflowed(status))
		return IRQ_NONE;

	while (status) {
		struct perf_event *event;

		idx = __ffs(status);
		status &= ~(1 << idx);
		event = slice->socket->counters[idx].event;
		if (!event)
			continue;

		qcom_l3_cache__32bit_counter_update_from_slice(event, slice,
							       event->hw.idx);
	}

	return IRQ_HANDLED;
}

/*
 * Implementation of abstract pmu functionality required by
 * the core perf events code.
 */

static void qcom_l3_cache__pmu_enable(struct pmu *pmu)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(pmu);
	struct hml3_pmu *slice;
	int idx;

	/*
	 * Re-write CNTCTL for all existing events to re-assert
	 * the start trigger.
	 */
	for (idx = 0; idx < L3_NUM_COUNTERS; idx++)
		if (socket->counters[idx].event)
			list_for_each_entry(slice, &socket->pmus, entry)
				hml3_pmu__counter_reset_trigger(slice, idx);

	/* Ensure all programming commands are done before proceeding */
	wmb();
	list_for_each_entry(slice, &socket->pmus, entry)
		hml3_pmu__enable(slice);
}

static void qcom_l3_cache__pmu_disable(struct pmu *pmu)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(pmu);
	struct hml3_pmu *slice;

	list_for_each_entry(slice, &socket->pmus, entry)
		hml3_pmu__disable(slice);

	/* Ensure the basic counter unit is stopped before proceeding */
	wmb();
}

static int qcom_l3_cache__event_init(struct perf_event *event)
{
	struct l3cache_pmu *socket;
	struct hw_perf_event *hwc = &event->hw;

	/*
	 * Is the event for this PMU?
	 */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * There are no per-counter mode filters in the PMU.
	 */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_hv || event->attr.exclude_idle)
		return -EINVAL;

	hwc->idx = -1;

	/*
	 * Sampling not supported since these events are not core-attributable.
	 */
	if (hwc->sample_period)
		return -EINVAL;

	/*
	 * Task mode not available, we run the counters as socket counters,
	 * not attributable to any CPU and therefore cannot attribute per-task.
	 */
	if (event->cpu < 0)
		return -EINVAL;

	/*
	 * Many perf core operations (eg. events rotation) operate on a
	 * single CPU context. This is obvious for CPU PMUs, where one
	 * expects the same sets of events being observed on all CPUs,
	 * but can lead to issues for off-core PMUs, like this one, where
	 * each event could be theoretically assigned to a different CPU.
	 * To mitigate this, we enforce CPU assignment to one designated
	 * processor (the one described in the "cpumask" attribute exported
	 * by the PMU device). perf user space tools honor this and avoid
	 * opening more than one copy of the events.
	 */
	socket = to_l3cache_pmu(event->pmu);
	event->cpu = cpumask_first(&socket->cpu);

	return 0;
}

static void qcom_l3_cache__event_start(struct perf_event *event, int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	hwc->state = 0;

	socket->counters[hwc->idx].start(event);
}

static void qcom_l3_cache__event_stop(struct perf_event *event, int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (!(hwc->state & PERF_HES_STOPPED)) {
		socket->counters[hwc->idx].stop(event, flags);

		if (flags & PERF_EF_UPDATE)
			socket->counters[hwc->idx].update(event);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static int qcom_l3_cache__event_add(struct perf_event *event, int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int idx;
	int sz;

	/*
	 * Try to allocate a counter.
	 */
	sz = get_hw_counter_size(event);
	idx = bitmap_find_free_region(socket->used_mask, L3_NUM_COUNTERS, sz);
	if (idx < 0)
		/* The counters are all in use. */
		return -EAGAIN;

	hwc->idx = idx;
	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;

	if (sz == 0)
		socket->counters[idx] = (struct l3cache_pmu_hwc) {
			.event = event,
			.start = qcom_l3_cache__32bit_counter_start,
			.stop = qcom_l3_cache__32bit_counter_stop,
			.update = qcom_l3_cache__32bit_counter_update
		};
	else {
		socket->counters[idx] = (struct l3cache_pmu_hwc) {
			.event = event,
			.start = qcom_l3_cache__64bit_counter_start,
			.stop = qcom_l3_cache__64bit_counter_stop,
			.update = qcom_l3_cache__64bit_counter_update
		};
		socket->counters[idx+1] = socket->counters[idx];
	}

	if (flags & PERF_EF_START)
		qcom_l3_cache__event_start(event, 0);

	/* Propagate changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void qcom_l3_cache__event_del(struct perf_event *event, int flags)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int sz;

	qcom_l3_cache__event_stop(event,  flags | PERF_EF_UPDATE);
	sz = get_hw_counter_size(event);
	socket->counters[hwc->idx].event = NULL;
	if (sz)
		socket->counters[hwc->idx+1].event = NULL;
	bitmap_release_region(socket->used_mask, hwc->idx, sz);

	perf_event_update_userpage(event);
}

static void qcom_l3_cache__event_read(struct perf_event *event)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	socket->counters[hwc->idx].update(event);
}

/*
 * Add support for creating events symbolically when using the perf
 * user space tools command line. E.g.:
 *   perf stat -a -e l3cache/event=read-miss/ ls
 *   perf stat -a -e l3cache/event=0x21/ ls
 */

ssize_t l3cache_pmu_event_sysfs_show(struct device *dev,
				     struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);
	return sprintf(page, "event=0x%02llx,name=%s\n",
		       pmu_attr->id, attr->attr.name);
}

#define L3CACHE_EVENT_VAR(__id)	pmu_event_attr_##__id
#define L3CACHE_EVENT_PTR(__id)	(&L3CACHE_EVENT_VAR(__id).attr.attr)

#define L3CACHE_EVENT_ATTR(__name, __id)			\
	PMU_EVENT_ATTR(__name, L3CACHE_EVENT_VAR(__id), __id,	\
		       l3cache_pmu_event_sysfs_show)


L3CACHE_EVENT_ATTR(cycles, L3_CYCLES);
L3CACHE_EVENT_ATTR(read-hit, L3_READ_HIT);
L3CACHE_EVENT_ATTR(read-miss, L3_READ_MISS);
L3CACHE_EVENT_ATTR(read-hit-d-side, L3_READ_HIT_D);
L3CACHE_EVENT_ATTR(read-miss-d-side, L3_READ_MISS_D);
L3CACHE_EVENT_ATTR(write-hit, L3_WRITE_HIT);
L3CACHE_EVENT_ATTR(write-miss, L3_WRITE_MISS);

static struct attribute *qcom_l3_cache_pmu_events[] = {
	L3CACHE_EVENT_PTR(L3_CYCLES),
	L3CACHE_EVENT_PTR(L3_READ_HIT),
	L3CACHE_EVENT_PTR(L3_READ_MISS),
	L3CACHE_EVENT_PTR(L3_READ_HIT_D),
	L3CACHE_EVENT_PTR(L3_READ_MISS_D),
	L3CACHE_EVENT_PTR(L3_WRITE_HIT),
	L3CACHE_EVENT_PTR(L3_WRITE_MISS),
	NULL
};

static struct attribute_group qcom_l3_cache_pmu_events_group = {
	.name = "events",
	.attrs = qcom_l3_cache_pmu_events,
};

PMU_FORMAT_ATTR(event, "config:0-7");
PMU_FORMAT_ATTR(lc, "config:32");

static struct attribute *qcom_l3_cache_pmu_formats[] = {
	&format_attr_event.attr,
	&format_attr_lc.attr,
	NULL,
};

static struct attribute_group qcom_l3_cache_pmu_format_group = {
	.name = "format",
	.attrs = qcom_l3_cache_pmu_formats,
};

static ssize_t qcom_l3_cache_pmu_cpumask_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct l3cache_pmu *socket = to_l3cache_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &socket->cpu);
}

static struct device_attribute qcom_l3_cache_pmu_cpumask_attr =
		__ATTR(cpumask, 0444, qcom_l3_cache_pmu_cpumask_show, NULL);

static struct attribute *qcom_l3_cache_pmu_cpumask_attrs[] = {
	&qcom_l3_cache_pmu_cpumask_attr.attr,
	NULL,
};

static struct attribute_group qcom_l3_cache_pmu_cpumask_attr_group = {
	.attrs = qcom_l3_cache_pmu_cpumask_attrs,
};


static const struct attribute_group *qcom_l3_cache_pmu_attr_grps[] = {
	&qcom_l3_cache_pmu_format_group,
	&qcom_l3_cache_pmu_events_group,
	&qcom_l3_cache_pmu_cpumask_attr_group,
	NULL,
};

/*
 * Probing functions and data.
 */

static int qcom_l3_cache_pmu_offline_cpu(unsigned int cpu, struct hlist_node *n)
{
	struct l3cache_pmu *socket = hlist_entry_safe(n, struct l3cache_pmu, node);
	unsigned int target;

	if (!cpumask_test_and_clear_cpu(cpu, &socket->cpu))
		return 0;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;
	/*
	 * TODO: migrate context once core races on event->ctx have
	 * been fixed.
	 */
	cpumask_set_cpu(target, &socket->cpu);
	return 0;
}

static int qcom_l3_cache_pmu_probe_slice(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev->parent);
	struct platform_device *sdev = to_platform_device(dev);
	struct l3cache_pmu *socket = data;
	struct resource *memrc;
	void __iomem *regs;
	struct hml3_pmu *slice;
	int irq, err;

	memrc = platform_get_resource(sdev, IORESOURCE_MEM, 0);
	slice = devm_kzalloc(&pdev->dev, sizeof(*slice), GFP_KERNEL);
	if (!slice)
		return -ENOMEM;

	regs = devm_ioremap_resource(&pdev->dev, memrc);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Can't map slice @%pa\n", &memrc->start);
		return PTR_ERR(regs);
	}

	irq = platform_get_irq(sdev, 0);
	if (irq <= 0) {
		dev_err(&pdev->dev, "Failed to get valid irq for slice @%pa\n",
			&memrc->start);
		return irq;
	}

	err = devm_request_irq(&pdev->dev, irq, qcom_l3_cache__handle_irq, 0,
			       "qcom-l3-cache-pmu", slice);
	if (err) {
		dev_err(&pdev->dev, "Request for IRQ failed for slice @%pa\n",
			&memrc->start);
		return err;
	}

	hml3_pmu__init(slice, socket, regs);
	list_add(&slice->entry, &socket->pmus);
	return 0;
}

static int qcom_l3_cache_pmu_probe(struct platform_device *pdev)
{
	struct l3cache_pmu *socket = devm_kzalloc(&pdev->dev, sizeof(*socket),
						  GFP_KERNEL);
	struct list_head *slice;
	int err, num_pmus = 0;

	if (!socket)
		return -ENOMEM;

	INIT_LIST_HEAD(&socket->pmus);

	socket->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,

		.pmu_enable	= qcom_l3_cache__pmu_enable,
		.pmu_disable	= qcom_l3_cache__pmu_disable,
		.event_init	= qcom_l3_cache__event_init,
		.add		= qcom_l3_cache__event_add,
		.del		= qcom_l3_cache__event_del,
		.start		= qcom_l3_cache__event_start,
		.stop		= qcom_l3_cache__event_stop,
		.read		= qcom_l3_cache__event_read,

		.attr_groups	= qcom_l3_cache_pmu_attr_grps,
	};

	/* Designate the probing CPU as the reader */
	cpumask_set_cpu(smp_processor_id(), &socket->cpu);

	/* Iterate through slices and add */
	err = device_for_each_child(&pdev->dev, socket,
				    qcom_l3_cache_pmu_probe_slice);

	list_for_each(slice, &socket->pmus)
		++num_pmus;

	if (num_pmus == 0) {
		dev_err(&pdev->dev, "No hardware HML3 PMUs found\n");
		return -ENODEV;
	}

	err = perf_pmu_register(&socket->pmu, "l3cache", -1);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to register L3 cache PMU (%d)\n", err);
		return err;
	}

	/* Add this instance to the list used by the offline callback */
	cpuhp_state_add_instance_nocalls(CPUHP_AP_PERF_QCOM_L3CACHE_ONLINE,
					 &socket->node);
	dev_info(&pdev->dev,
		 "Registered L3 cache PMU, type: %d, using %d HW PMUs\n",
		 socket->pmu.type, num_pmus);

	return 0;
}

static const struct acpi_device_id qcom_l3_cache_pmu_acpi_match[] = {
	{ "QCOM8080", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, qcom_l3_cache_pmu_acpi_match);

static struct platform_driver qcom_l3_cache_pmu_driver = {
	.driver = {
		.name = "qcom-l3cache-pmu",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(qcom_l3_cache_pmu_acpi_match),
	},
	.probe = qcom_l3_cache_pmu_probe,
};

static int __init register_qcom_l3_cache_pmu_driver(void)
{
	int ret;

	/* Install a hook to update the reader CPU in case it goes offline */
	ret = cpuhp_setup_state_multi(CPUHP_AP_PERF_QCOM_L3CACHE_ONLINE,
				      "perf/qcom/l3cache:online", NULL,
				      qcom_l3_cache_pmu_offline_cpu);
	if (ret)
		return ret;

	return platform_driver_register(&qcom_l3_cache_pmu_driver);
}
device_initcall(register_qcom_l3_cache_pmu_driver);
