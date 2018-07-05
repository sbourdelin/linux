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
 * - PC capture (PCC):
 *   Allows more precise PC sampling by storing the PC in a separate system
 *   register when an event counter overflow occurs. Reduces skid and allows
 *   sampling when interrupts are disabled (the PMI is a maskable interrupt
 *   in arm64). Note that there is only one PC capture register so we only
 *   allow one event at a time to use it.
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
 *   These events are encoded into perf_event_attr as:
 *     mbe      [config1:1   ]  (flag that indicates a matrix-based event)
 *     reg      [config:12-15]  (specifies the PMRESRx_EL0 instance)
 *     group    [config:0-3  ]  (specifies the event group)
 *     code     [config:4-11 ]  (specifies the event)
 *
 *   Events with the mbe flag set to zero are treated as common or raw PMUv3
 *   events and are handled by the base PMUv3 driver code.
 *
 *   The first two indexes are set combining the RESR and group number with a
 *   base number and writing it into the architected PMXEVTYPER_EL0.evtCount.
 *   The third index is set by writing the code into the bits corresponding
 *   with the group into the appropriate IMPLEMENTATION DEFINED PMRESRx_EL0
 *   register.
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/perf_event.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <asm/barrier.h>
#include <asm/sysreg.h>

#include <linux/perf/arm_pmu.h>

/*
 * Low-level PCC definitions
 */

#define PCCPTR_UNAUTH       BIT(0)
#define PCCPTR_PC_MS_SP     BIT(55)
#define PCCPTR_PC_MASK_SP   GENMASK_ULL(55,2)
#define PCCPTR_SIGN_EXT_SP  GENMASK_ULL(63,56);
#define PCC_CPT_PME0        BIT(0)
#define PCC_CPT_EVENT_EN(x) (PCC_CPT_PME0 << (x))
#define PCC_CPT_PMOVNEVT0   BIT(16)
#define PCC_CPT_EVENT_OV(x) (PCC_CPT_PMOVNEVT0 << (x))
#define QC_EVT_PCC_SHIFT    0
#define QC_EVT_PCC_MASK     GENMASK(QC_EVT_PCC_SHIFT + 1, QC_EVT_PCC_SHIFT)
#define QC_EVT_PCC(event)						\
	(((event)->attr.config1 & QC_EVT_PCC_MASK) >> QC_EVT_PCC_SHIFT)

struct pcc_ops {
	/* Retrieve the PC from the IMP DEF pmpccptr_el0 register */
	void (*read_pmpccptr_el0_pc)(u64 *pc);
	/* Read/write the IMP DEF pmpccptcr0_el0 register */
	u64 (*read_pmpccptcr0_el0)(void);
	void (*write_pmpccptcr0_el0)(u64 val);
};

/*
 * Low-level MBE definitions
 */

#define pmresr0_el0         sys_reg(3, 5, 11, 3, 0)
#define pmresr1_el0         sys_reg(3, 5, 11, 3, 2)
#define pmresr2_el0         sys_reg(3, 5, 11, 3, 4)
#define pmxevcntcr_el0      sys_reg(3, 5, 11, 0, 3)

#define QC_EVT_MBE_SHIFT    1
#define QC_EVT_REG_SHIFT    12
#define QC_EVT_CODE_SHIFT   4
#define QC_EVT_GRP_SHIFT    0
#define QC_EVT_MBE_MASK     GENMASK(QC_EVT_MBE_SHIFT + 1,  QC_EVT_MBE_SHIFT)
#define QC_EVT_REG_MASK     GENMASK(QC_EVT_REG_SHIFT + 3,  QC_EVT_REG_SHIFT)
#define QC_EVT_CODE_MASK    GENMASK(QC_EVT_CODE_SHIFT + 7, QC_EVT_CODE_SHIFT)
#define QC_EVT_GRP_MASK     GENMASK(QC_EVT_GRP_SHIFT + 3,  QC_EVT_GRP_SHIFT)
#define QC_EVT_RG_MASK      (QC_EVT_REG_MASK | QC_EVT_GRP_MASK)
#define QC_EVT_RG(event)    ((event)->attr.config & QC_EVT_RG_MASK)
#define QC_EVT_MBE(event)						\
	(((event)->attr.config1 & QC_EVT_MBE_MASK) >> QC_EVT_MBE_SHIFT)
#define QC_EVT_REG(event)						\
	(((event)->attr.config & QC_EVT_REG_MASK) >> QC_EVT_REG_SHIFT)
#define QC_EVT_CODE(event)						\
	(((event)->attr.config & QC_EVT_CODE_MASK) >> QC_EVT_CODE_SHIFT)
#define QC_EVT_GROUP(event)						\
	(((event)->attr.config & QC_EVT_GRP_MASK) >> QC_EVT_GRP_SHIFT)

#define QC_MAX_GROUP        7
#define QC_MAX_RESR         2
#define QC_BITS_PER_GROUP   8
#define QC_RESR_ENABLE      BIT_ULL(63)
#define QC_RESR_EVT_BASE    0xd8

struct mbe_ops {
	/* Enable a MBE event */
	void (*enable)(struct perf_event *event);
	/* Enable a MBE event */
	void (*disable)(struct perf_event *event);
};

/*
 * Common state
 */

static struct arm_pmu *def_ops;
static const struct pcc_ops *pcc_ops;
static const struct mbe_ops *mbe_ops;

/*
 * Low-level Falkor operations
 */

static void falkor_read_pmpccptr_el0_pc(u64 *pc)
{
	u64 pcc = read_sysreg_s(sys_reg(3, 5, 11, 4, 0));

	/*
	 * Leave pc unchanged if we are not allowed to read the PC
	 *  (e.g. if the overflow occurred in secure code)
	 */
	if (pcc & PCCPTR_UNAUTH)
		return;

	*pc = pcc;
}

static void falkor_write_pmpccptcr0_el0(u64 val)
{
	write_sysreg_s(val, sys_reg(3, 5, 11, 4, 1));
}

static u64 falkor_read_pmpccptcr0_el0(void)
{
	return read_sysreg_s(sys_reg(3, 5, 11, 4, 1));
}

static inline void falkor_write_pmresr(u64 reg, u64 val)
{
	switch (reg) {
	case 0:
		write_sysreg_s(val, pmresr0_el0);
		return;
	case 1:
		write_sysreg_s(val, pmresr1_el0);
		return;
	default:
		write_sysreg_s(val, pmresr2_el0);
		return;
	}
}

static inline u64 falkor_read_pmresr(u64 reg)
{
	switch (reg) {
	case 0:
		return read_sysreg_s(pmresr0_el0);
	case 1:
		return read_sysreg_s(pmresr1_el0);
	default:
		return read_sysreg_s(pmresr2_el0);
	}
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

static void falkor_mbe_enable(struct perf_event *event)
{
	/* Program the appropriate PMRESRx_EL0 */
	u64 reg = QC_EVT_REG(event);
	u64 code = QC_EVT_CODE(event);
	u64 group = QC_EVT_GROUP(event);

	falkor_set_resr(reg, group, code);
}

static void falkor_mbe_disable(struct perf_event *event)
{
	/* De-program the appropriate PMRESRx_EL0 */
	u64 reg = QC_EVT_REG(event);
	u64 group = QC_EVT_GROUP(event);

	falkor_clear_resr(reg, group);
}

static const struct pcc_ops falkor_pcc_ops = {
	.read_pmpccptr_el0_pc = falkor_read_pmpccptr_el0_pc,
	.read_pmpccptcr0_el0 = falkor_read_pmpccptcr0_el0,
	.write_pmpccptcr0_el0 = falkor_write_pmpccptcr0_el0
};

static const struct mbe_ops falkor_mbe_ops = {
	.enable = falkor_mbe_enable,
	.disable = falkor_mbe_disable
};

/*
 * Low-level Saphira operations
 */

static void saphira_read_pmpccptr_el0_pc(u64 *pc)
{
	u64 pcc = read_sysreg_s(sys_reg(3, 5, 11, 5, 0));

	/*
	 * Leave pc unchanged if we are not allowed to read the PC
	 *  (e.g. if the overflow occurred in secure code)
	 */
	if (pcc & PCCPTR_UNAUTH)
		return;

	*pc = pcc & PCCPTR_PC_MASK_SP;
	/* In Saphira we need to sign extend */
	if (pcc & PCCPTR_PC_MS_SP)
		*pc |= PCCPTR_SIGN_EXT_SP;
}

static void saphira_write_pmpccptcr0_el0(u64 val)
{
	write_sysreg_s(val, sys_reg(3, 5, 11, 5, 1));
}

static u64 saphira_read_pmpccptcr0_el0(void)
{
	return read_sysreg_s(sys_reg(3, 5, 11, 5, 1));
}

static const struct pcc_ops saphira_pcc_ops = {
	.read_pmpccptr_el0_pc = saphira_read_pmpccptr_el0_pc,
	.read_pmpccptcr0_el0 = saphira_read_pmpccptcr0_el0,
	.write_pmpccptcr0_el0 = saphira_write_pmpccptcr0_el0
};

/*
 * Check if the given event uses PCC
 */
static bool has_pcc(struct perf_event *event)
{
	/* PCC not enabled */
	if (!pcc_ops)
		return false;

	/* PCC only used for sampling events */
	if (!is_sampling_event(event))
		return false;

	/*
	 * PCC only used without callchain because software callchain might
	 * provide misleading entries
	 */
	if (event->attr.sample_type & PERF_SAMPLE_CALLCHAIN)
		return false;

	return QC_EVT_PCC(event);
}

/*
 * Check if the given event uses MBE
 */
static bool has_mbe(struct perf_event *event)
{
	/* MBE not enabled */
	if (!mbe_ops)
		return false;

	return QC_EVT_MBE(event);
}

/*
 * Check if the given event is for the raw or dynamic PMU type
 */
static inline bool is_raw_or_dynamic(struct perf_event *event)
{
	int type = event->attr.type;

	return (type == PERF_TYPE_RAW) || (type == event->pmu->type);
}

/*
 * Check if e1 and e2 have conflicting PCC settings
 */
static inline bool pcc_conflict(struct perf_event *e1, struct perf_event *e2)
{
	bool pcc1 = has_pcc(e1), pcc2= has_pcc(e2);

	/* No conflict if none of the events is using PCC */
	if (!pcc1 && !pcc2)
		return false;

	/* No conflict if one of the events is not using PCC */
	if (pcc1 != pcc2)
		return false;

	pr_warn_ratelimited("PCC exclusion: conflicting events %llx %llx\n",
			     e1->attr.config,
			     e2->attr.config);
	return true;
}

/*
 * Check if e1 and e2 have conflicting MBE settings
 */
static inline bool mbe_conflict(struct perf_event *e1, struct perf_event *e2)
{
	bool mbe1 = has_mbe(e1), mbe2= has_mbe(e2);

	/* No conflict if none of the events is using MBE */
	if (!mbe1 && !mbe2)
		return false;

	/* No conflict if one of the events is not using MBE */
	if (mbe1 != mbe2)
		return false;

	/* No conflict if using different reg or group */
	if (QC_EVT_RG(e1) != QC_EVT_RG(e2))
		return false;

	/* Same mbe, reg and group is fine so long as code matches */
	if (QC_EVT_CODE(e1) == QC_EVT_CODE(e2))
		return false;

	pr_warn_ratelimited("Group exclusion: conflicting events %llx %llx\n",
			     e1->attr.config,
			     e2->attr.config);
	return true;
}

/*
 * Check if e1 and e2 conflict with each other
 *
 * e1 is an event that has extensions and we are checking against e2.
 */
static inline bool events_conflict(struct perf_event *e1, struct perf_event *e2)
{
	int type = e2->attr.type;
	int dynamic = e1->pmu->type;

	/* Same event? */
	if (e1 == e2)
		return false;

	/* Other PMU that is not the RAW or this PMU's dynamic type? */
	if ((e1->pmu != e2->pmu) && (type != PERF_TYPE_RAW) && (type != dynamic))
		return false;

	return pcc_conflict(e1, e2) || mbe_conflict(e1, e2);;
}

/*
 * Handle a PCC event overflow
 *
 * No extra checks needed here since we do all of that during map, event_idx,
 * and enable. We only let one PCC event per-CPU pass-through to this.
 */
static void pcc_overflow_handler(struct perf_event *event,
				 struct perf_sample_data *data,
				 struct pt_regs *regs)
{
	u64 irq_pc = regs->pc;

	/* Override with hardware PC */
	pcc_ops->read_pmpccptr_el0_pc(&regs->pc);

	/* Let the original handler finish the operation */
	event->orig_overflow_handler(event, data, regs);

	/* Restore */
	regs->pc = irq_pc;
}

/*
 * Check if the given event is valid for the PMU and if so return the value
 * that can be used in PMXEVTYPER_EL0 to select the event
 */
static int qcom_arm_pmu_map_event(struct perf_event *event)
{
	if (!is_raw_or_dynamic(event))
		goto done;

	if (has_pcc(event) || has_mbe(event)) {
		struct perf_event *leader;
		struct perf_event *sibling;

		/* Check if the event is compatible with its group */
		leader = event->group_leader;
		if (events_conflict(event, leader))
			return -ENOENT;

		for_each_sibling_event(sibling, leader)
			if (events_conflict(event, sibling))
				return -ENOENT;
	}

	if (has_mbe(event)) {
		u64 reg = QC_EVT_REG(event);
		u64 group = QC_EVT_GROUP(event);

		if ((group > QC_MAX_GROUP) || (reg > QC_MAX_RESR))
			return -ENOENT;
		return QC_RESR_EVT_BASE + reg * 8 + group;
	}

done:
	return def_ops->map_event(event);
}

/*
 * Find a slot for the event on the current CPU
 */
static int qcom_arm_pmu_get_event_idx(struct pmu_hw_events *cpuc, struct perf_event *event)
{
	int idx;

	if (!is_raw_or_dynamic(event))
		goto done;

	if (has_pcc(event) || has_mbe(event)) {
		/* Check for conflicts with existing events */
		for_each_set_bit(idx, cpuc->used_mask, ARMPMU_MAX_HWEVENTS)
			if (cpuc->events[idx] &&
			    events_conflict(event, cpuc->events[idx]))
				return -ENOENT;
	}

	if (has_pcc(event)) {
		struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
		int idx;

		/*
		 * PCC is requested for this event so we need to use an event
		 * counter even for the cycle counter (PCC does not work with
		 * the dedicated cycle counter).
		 */
		for (idx = ARMV8_IDX_COUNTER0; idx < cpu_pmu->num_events; ++idx) {
			if (!test_and_set_bit(idx, cpuc->used_mask))
				return idx;
		}

		/* The counters are all in use. */
		return -EAGAIN;
	}

done:
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
 * Enable the given event
 */
static void qcom_arm_pmu_enable(struct perf_event *event)
{
	if (has_pcc(event)) {
		int idx = event->hw.idx;
		u32 pcc = PCC_CPT_EVENT_EN(ARMV8_IDX_TO_COUNTER(idx)) |
			  PCC_CPT_EVENT_OV(ARMV8_IDX_TO_COUNTER(idx));

		pcc_ops->write_pmpccptcr0_el0(pcc);
		event->orig_overflow_handler = READ_ONCE(event->overflow_handler);
		WRITE_ONCE(event->overflow_handler, pcc_overflow_handler);
	}

	if (has_mbe(event))
		mbe_ops->enable(event);

	/* Let the original op handle the rest */
	def_ops->enable(event);
}

/*
 * Disable the given event
 */
static void qcom_arm_pmu_disable(struct perf_event *event)
{
	/* Use the original op to disable the counter and interrupt  */
	def_ops->enable(event);

	if (has_pcc(event)) {
		int idx = event->hw.idx;
		u32 pcc = pcc_ops->read_pmpccptcr0_el0();

		pcc &= ~(PCC_CPT_EVENT_EN(ARMV8_IDX_TO_COUNTER(idx)) |
			 PCC_CPT_EVENT_OV(ARMV8_IDX_TO_COUNTER(idx)));
		pcc_ops->write_pmpccptcr0_el0(pcc);
		if (event->orig_overflow_handler)
			WRITE_ONCE(event->overflow_handler, event->orig_overflow_handler);
	}

	if (has_mbe(event))
		mbe_ops->disable(event);
}

/*
 * Reset the PMU
 */
static void qcom_arm_pmu_falkor_reset(void *info)
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

PMU_FORMAT_ATTR(event, "config:0-15");
PMU_FORMAT_ATTR(pcc,   "config1:0");
PMU_FORMAT_ATTR(mbe,   "config1:1");
PMU_FORMAT_ATTR(reg,   "config:12-15");
PMU_FORMAT_ATTR(code,  "config:4-11");
PMU_FORMAT_ATTR(group, "config:0-3");

static struct attribute *falkor_pmu_formats[] = {
	&format_attr_pcc.attr,
	&format_attr_event.attr,
	&format_attr_mbe.attr,
	&format_attr_reg.attr,
	&format_attr_code.attr,
	&format_attr_group.attr,
	NULL,
};

static struct attribute *saphira_pmu_formats[] = {
	&format_attr_pcc.attr,
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group pmu_format_attr_group = {
	.name = "format",
};

static inline bool pcc_supported(struct device *dev)
{
	u8 pcc = 0;

	acpi_node_prop_read(dev->fwnode, "qcom,pmu-pcc-support",
			    DEV_PROP_U8, &pcc, 1);
	return pcc != 0;
}

static int qcom_pmu_init(struct arm_pmu *pmu, struct device *dev)
{
	/* Save base arm_pmu so we can invoke its ops when appropriate */
	def_ops = devm_kmemdup(dev, pmu, sizeof(*def_ops), GFP_KERNEL);
	if (!def_ops) {
		pr_warn("Failed to allocate arm_pmu for QCOM extensions");
		return -ENODEV;
	}

	pmu->name = "qcom_pmuv3";

	/* Override the necessary ops */
	pmu->map_event     = qcom_arm_pmu_map_event;
	pmu->get_event_idx = qcom_arm_pmu_get_event_idx;
	pmu->enable        = qcom_arm_pmu_enable;
	pmu->disable       = qcom_arm_pmu_disable;

	/* Override the necessary attributes */
	pmu->pmu.attr_groups[ARMPMU_ATTR_GROUP_FORMATS] =
		&pmu_format_attr_group;

	return 1;
}

static int qcom_falkor_pmu_init(struct arm_pmu *pmu, struct device *dev)
{
	int result;

	if (pcc_supported(dev)) {
		pmu_format_attr_group.attrs = falkor_pmu_formats;
		pcc_ops = &falkor_pcc_ops;
	} else {
		pmu_format_attr_group.attrs = &falkor_pmu_formats[1];
	}

	mbe_ops = &falkor_mbe_ops;

	result = qcom_pmu_init(pmu, dev);
	pmu->reset = qcom_arm_pmu_falkor_reset;

	return result;
}

static int qcom_saphira_pmu_init(struct arm_pmu *pmu, struct device *dev)
{
	if (pcc_supported(dev))
		pcc_ops = &saphira_pcc_ops;
	else
		return -ENODEV;

	pmu_format_attr_group.attrs = saphira_pmu_formats;

	return qcom_pmu_init(pmu, dev);
}

ACPI_DECLARE_PMU_VARIANT(qcom_falkor,  "QCOM8150", qcom_falkor_pmu_init);
ACPI_DECLARE_PMU_VARIANT(qcom_saphira, "QCOM8151", qcom_saphira_pmu_init);
