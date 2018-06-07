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
 * Qualcomm Technologies CPU PMU IMPLEMENTATION DEFINED extensions support
 *
 * Current extensions supported:
 *
 * - Matrix-based microarchitectural events support
 *
 *   Selection of these events can be envisioned as indexing them from
 *   a 3D matrix:
 *   - the first index selects a Region Event Selection Register (PMRESRx_EL0)
 *   - the second index selects a group from which only one event at a time
 *     can be selected
 *   - the third index selects the event
 *
 *   The event is encoded into perf_event_attr.config as 0xPRCCG, where:
 *     P  [config:16   ] = prefix   (flag that indicates a matrix-based event)
 *     R  [config:12-15] = register (specifies the PMRESRx_EL0 instance)
 *     G  [config:0-3  ] = group    (specifies the event group)
 *     CC [config:4-11 ] = code     (specifies the event)
 *
 *   Events with the P flag set to zero are treated as common PMUv3 events
 *   and are directly programmed into PMXEVTYPERx_EL0.
 *
 *   The first two indexes are set combining the RESR and group number with
 *   a base number and writing it into the architected PMXEVTYPER_EL0 register.
 *   The third index is set by writing the code into the bits corresponding
 *   with the group into the appropriate IMPLEMENTATION DEFINED PMRESRx_EL0
 *   register.
 */

#include <linux/acpi.h>
#include <linux/perf/arm_pmu.h>

#define pmresr0_el0         sys_reg(3, 5, 11, 3, 0)
#define pmresr1_el0         sys_reg(3, 5, 11, 3, 2)
#define pmresr2_el0         sys_reg(3, 5, 11, 3, 4)
#define pmxevcntcr_el0      sys_reg(3, 5, 11, 0, 3)

#define QC_EVT_PFX_SHIFT    16
#define QC_EVT_REG_SHIFT    12
#define QC_EVT_CODE_SHIFT   4
#define QC_EVT_GRP_SHIFT    0
#define QC_EVT_PFX_MASK     GENMASK(QC_EVT_PFX_SHIFT,  QC_EVT_PFX_SHIFT)
#define QC_EVT_REG_MASK     GENMASK(QC_EVT_REG_SHIFT + 3,  QC_EVT_REG_SHIFT)
#define QC_EVT_CODE_MASK    GENMASK(QC_EVT_CODE_SHIFT + 7, QC_EVT_CODE_SHIFT)
#define QC_EVT_GRP_MASK     GENMASK(QC_EVT_GRP_SHIFT + 3,  QC_EVT_GRP_SHIFT)
#define QC_EVT_PRG_MASK     (QC_EVT_PFX_MASK | QC_EVT_REG_MASK | QC_EVT_GRP_MASK)
#define QC_EVT_PRG(event)   ((event) & QC_EVT_PRG_MASK)
#define QC_EVT_REG(event)   (((event) & QC_EVT_REG_MASK)  >> QC_EVT_REG_SHIFT)
#define QC_EVT_CODE(event)  (((event) & QC_EVT_CODE_MASK) >> QC_EVT_CODE_SHIFT)
#define QC_EVT_GROUP(event) (((event) & QC_EVT_GRP_MASK)  >> QC_EVT_GRP_SHIFT)

#define QC_MAX_GROUP        7
#define QC_MAX_RESR         2
#define QC_BITS_PER_GROUP   8
#define QC_RESR_ENABLE      BIT_ULL(63)
#define QC_RESR_EVT_BASE    0xd8

static struct arm_pmu *def_ops;

static inline void falkor_write_pmresr(u64 reg, u64 val)
{
	if (reg == 0)
		write_sysreg_s(val, pmresr0_el0);
	else if (reg == 1)
		write_sysreg_s(val, pmresr1_el0);
	else
		write_sysreg_s(val, pmresr2_el0);
}

static inline u64 falkor_read_pmresr(u64 reg)
{
	return (reg == 0 ? read_sysreg_s(pmresr0_el0) :
		reg == 1 ? read_sysreg_s(pmresr1_el0) :
			   read_sysreg_s(pmresr2_el0));
}

static void falkor_set_resr(u64 reg, u64 group, u64 code)
{
	u64 shift = group * QC_BITS_PER_GROUP;
	u64 mask = GENMASK(shift + QC_BITS_PER_GROUP - 1, shift);
	u64 val;

	val = falkor_read_pmresr(reg) & ~mask;
	val |= (code << shift);
	val |= QC_RESR_ENABLE;
	falkor_write_pmresr(reg, val);
}

static void falkor_clear_resr(u64 reg, u64 group)
{
	u32 shift = group * QC_BITS_PER_GROUP;
	u64 mask = GENMASK(shift + QC_BITS_PER_GROUP - 1, shift);
	u64 val = falkor_read_pmresr(reg) & ~mask;

	falkor_write_pmresr(reg, val == QC_RESR_ENABLE ? 0 : val);
}

/*
 * Check if e1 and e2 conflict with each other
 *
 * e1 is a matrix-based microarchitectural event we are checking against e2.
 * A conflict exists if the events use the same reg, group, and a different
 * code. Events with the same code are allowed because they could be using
 * different filters (e.g. one to count user space and the other to count
 * kernel space events).
 */
static inline int events_conflict(struct perf_event *e1, struct perf_event *e2)
{
	if ((e1 != e2) &&
	    (e1->pmu == e2->pmu) &&
	    (QC_EVT_PRG(e1->attr.config) == QC_EVT_PRG(e2->attr.config)) &&
	    (QC_EVT_CODE(e1->attr.config) != QC_EVT_CODE(e2->attr.config))) {
		pr_debug_ratelimited(
			"Group exclusion: conflicting events %llx %llx\n",
			e1->attr.config,
			e2->attr.config);
		return 1;
	}
	return 0;
}

/*
 * Check if the given event is valid for the PMU and if so return the value
 * that can be used in PMXEVTYPER_EL0 to select the event
 */
static int falkor_map_event(struct perf_event *event)
{
	u64 reg = QC_EVT_REG(event->attr.config);
	u64 group = QC_EVT_GROUP(event->attr.config);
	struct perf_event *leader;
	struct perf_event *sibling;

	if (!(event->attr.config & QC_EVT_PFX_MASK))
		/* Common PMUv3 event, forward to the original op */
		return def_ops->map_event(event);

	/* Is it a valid matrix event? */
	if ((group > QC_MAX_GROUP) || (reg > QC_MAX_RESR))
		return -ENOENT;

	/* If part of an event group, check if the event can be put in it */

	leader = event->group_leader;
	if (events_conflict(event, leader))
		return -ENOENT;

	for_each_sibling_event(sibling, leader)
		if (events_conflict(event, sibling))
			return -ENOENT;

	return QC_RESR_EVT_BASE + reg*8 + group;
}

/*
 * Find a slot for the event on the current CPU
 */
static int falkor_get_event_idx(struct pmu_hw_events *cpuc, struct perf_event *event)
{
	int idx;

	if (!!(event->attr.config & QC_EVT_PFX_MASK))
		/* Matrix event, check for conflicts with existing events */
		for_each_set_bit(idx, cpuc->used_mask, ARMPMU_MAX_HWEVENTS)
			if (cpuc->events[idx] &&
			    events_conflict(event, cpuc->events[idx]))
				return -ENOENT;

	/* Let the original op handle the rest */
	idx = def_ops->get_event_idx(cpuc, event);

	/*
	 * This is called for actually allocating the events, but also with
	 * a dummy pmu_hw_events when validating groups, for that case we
	 * need to ensure that cpuc->events[idx] is NULL so we don't use
	 * an uninitialized pointer. Conflicts for matrix events in groups
	 * are checked during event mapping anyway (see falkor_event_map).
	 */
	cpuc->events[idx] = NULL;

	return idx;
}

/*
 * Reset the PMU
 */
static void falkor_reset(void *info)
{
	struct arm_pmu *pmu = (struct arm_pmu *)info;
	u32 i, ctrs = pmu->num_events;

	/* PMRESRx_EL0 regs are unknown at reset, except for the EN field */
	for (i = 0; i <= QC_MAX_RESR; i++)
		falkor_write_pmresr(i, 0);

	/* PMXEVCNTCRx_EL0 regs are unknown at reset */
	for (i = 0; i <= ctrs; i++) {
		write_sysreg(i, pmselr_el0);
		isb();
		write_sysreg_s(0, pmxevcntcr_el0);
	}

	/* Let the original op handle the rest */
	def_ops->reset(info);
}

/*
 * Enable the given event
 */
static void falkor_enable(struct perf_event *event)
{
	if (!!(event->attr.config & QC_EVT_PFX_MASK)) {
		/* Matrix event, program the appropriate PMRESRx_EL0 */
		struct arm_pmu *pmu = to_arm_pmu(event->pmu);
		struct pmu_hw_events *events = this_cpu_ptr(pmu->hw_events);
		u64 reg = QC_EVT_REG(event->attr.config);
		u64 code = QC_EVT_CODE(event->attr.config);
		u64 group = QC_EVT_GROUP(event->attr.config);
		unsigned long flags;

		raw_spin_lock_irqsave(&events->pmu_lock, flags);
		falkor_set_resr(reg, group, code);
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
	}

	/* Let the original op handle the rest */
	def_ops->enable(event);
}

/*
 * Disable the given event
 */
static void falkor_disable(struct perf_event *event)
{
	/* Use the original op to disable the counter and interrupt  */
	def_ops->enable(event);

	if (!!(event->attr.config & QC_EVT_PFX_MASK)) {
		/* Matrix event, de-program the appropriate PMRESRx_EL0 */
		struct arm_pmu *pmu = to_arm_pmu(event->pmu);
		struct pmu_hw_events *events = this_cpu_ptr(pmu->hw_events);
		u64 reg = QC_EVT_REG(event->attr.config);
		u64 group = QC_EVT_GROUP(event->attr.config);
		unsigned long flags;

		raw_spin_lock_irqsave(&events->pmu_lock, flags);
		falkor_clear_resr(reg, group);
		raw_spin_unlock_irqrestore(&events->pmu_lock, flags);
	}
}

PMU_FORMAT_ATTR(event,  "config:0-15");
PMU_FORMAT_ATTR(prefix, "config:16");
PMU_FORMAT_ATTR(reg,    "config:12-15");
PMU_FORMAT_ATTR(code,   "config:4-11");
PMU_FORMAT_ATTR(group,  "config:0-3");

static struct attribute *falkor_pmu_formats[] = {
	&format_attr_event.attr,
	&format_attr_prefix.attr,
	&format_attr_reg.attr,
	&format_attr_code.attr,
	&format_attr_group.attr,
	NULL,
};

static struct attribute_group falkor_pmu_format_attr_group = {
	.name = "format",
	.attrs = falkor_pmu_formats,
};

static int qcom_falkor_pmu_init(struct arm_pmu *pmu, struct device *dev)
{
	/* Save base arm_pmu so we can invoke its ops when appropriate */
	def_ops = devm_kmemdup(dev, pmu, sizeof(*def_ops), GFP_KERNEL);
	if (!def_ops) {
		pr_warn("Failed to allocate arm_pmu for QCOM extensions");
		return -ENODEV;
	}

	pmu->name = "qcom_pmuv3";

	/* Override the necessary ops */
	pmu->map_event     = falkor_map_event;
	pmu->get_event_idx = falkor_get_event_idx;
	pmu->reset         = falkor_reset;
	pmu->enable        = falkor_enable;
	pmu->disable       = falkor_disable;

	/* Override the necessary attributes */
	pmu->pmu.attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&falkor_pmu_format_attr_group;

	return 1;
}

ACPI_DECLARE_PMU_VARIANT(qcom_falkor, "QCOM8150", qcom_falkor_pmu_init);
