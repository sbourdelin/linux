/*
 * Copyright (C) 2013 Advanced Micro Devices, Inc.
 *
 * Author: Steven Kinney <Steven.Kinney@amd.com>
 * Author: Suravee Suthikulpanit <Suraveee.Suthikulpanit@amd.com>
 *
 * Perf: amd_iommu - AMD IOMMU Performance Counter PMU implementation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)	"perf/amd_iommu: " fmt

#include <linux/perf_event.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/slab.h>

#include "../perf_event.h"
#include "iommu.h"

#define COUNTER_SHIFT		16

#define _GET_BANK(ev)       ((u8)(ev->hw.extra_reg.reg >> 8))
#define _GET_CNTR(ev)       ((u8)(ev->hw.extra_reg.reg))

/* iommu pmu config masks */
#define _GET_CSOURCE(ev)    ((ev->hw.config & 0xFFULL))
#define _GET_DEVID(ev)      ((ev->hw.config >> 8)  & 0xFFFFULL)
#define _GET_PASID(ev)      ((ev->hw.config >> 24) & 0xFFFFULL)
#define _GET_DOMID(ev)      ((ev->hw.config >> 40) & 0xFFFFULL)
#define _GET_DEVID_MASK(ev) ((ev->hw.extra_reg.config)  & 0xFFFFULL)
#define _GET_PASID_MASK(ev) ((ev->hw.extra_reg.config >> 16) & 0xFFFFULL)
#define _GET_DOMID_MASK(ev) ((ev->hw.extra_reg.config >> 32) & 0xFFFFULL)

#define PERF_AMD_IOMMU_NAME_SZ 16

struct perf_amd_iommu {
	struct list_head list;
	struct pmu pmu;
	uint idx;
	char name[PERF_AMD_IOMMU_NAME_SZ];
	u8 max_banks;
	u8 max_counters;
	u64 cntr_assign_mask;
	raw_spinlock_t lock;
	const struct attribute_group *attr_groups[4];
};

LIST_HEAD(perf_amd_iommu_list);

#define format_group	attr_groups[0]
#define cpumask_group	attr_groups[1]
#define events_group	attr_groups[2]
#define null_group	attr_groups[3]

/*---------------------------------------------
 * sysfs format attributes
 *---------------------------------------------*/
PMU_FORMAT_ATTR(csource,    "config:0-7");
PMU_FORMAT_ATTR(devid,      "config:8-23");
PMU_FORMAT_ATTR(pasid,      "config:24-39");
PMU_FORMAT_ATTR(domid,      "config:40-55");
PMU_FORMAT_ATTR(devid_mask, "config1:0-15");
PMU_FORMAT_ATTR(pasid_mask, "config1:16-31");
PMU_FORMAT_ATTR(domid_mask, "config1:32-47");

static struct attribute *iommu_format_attrs[] = {
	&format_attr_csource.attr,
	&format_attr_devid.attr,
	&format_attr_pasid.attr,
	&format_attr_domid.attr,
	&format_attr_devid_mask.attr,
	&format_attr_pasid_mask.attr,
	&format_attr_domid_mask.attr,
	NULL,
};

static struct attribute_group amd_iommu_format_group = {
	.name = "format",
	.attrs = iommu_format_attrs,
};

/*---------------------------------------------
 * sysfs events attributes
 *---------------------------------------------*/
struct amd_iommu_event_desc {
	struct kobj_attribute attr;
	const char *event;
};

static ssize_t _iommu_event_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct amd_iommu_event_desc *event =
		container_of(attr, struct amd_iommu_event_desc, attr);
	return sprintf(buf, "%s\n", event->event);
}

#define AMD_IOMMU_EVENT_DESC(_name, _event)			\
{								\
	.attr  = __ATTR(_name, 0444, _iommu_event_show, NULL),	\
	.event = _event,					\
}

static struct amd_iommu_event_desc amd_iommu_v2_event_descs[] = {
	AMD_IOMMU_EVENT_DESC(mem_pass_untrans,        "csource=0x01"),
	AMD_IOMMU_EVENT_DESC(mem_pass_pretrans,       "csource=0x02"),
	AMD_IOMMU_EVENT_DESC(mem_pass_excl,           "csource=0x03"),
	AMD_IOMMU_EVENT_DESC(mem_target_abort,        "csource=0x04"),
	AMD_IOMMU_EVENT_DESC(mem_trans_total,         "csource=0x05"),
	AMD_IOMMU_EVENT_DESC(mem_iommu_tlb_pte_hit,   "csource=0x06"),
	AMD_IOMMU_EVENT_DESC(mem_iommu_tlb_pte_mis,   "csource=0x07"),
	AMD_IOMMU_EVENT_DESC(mem_iommu_tlb_pde_hit,   "csource=0x08"),
	AMD_IOMMU_EVENT_DESC(mem_iommu_tlb_pde_mis,   "csource=0x09"),
	AMD_IOMMU_EVENT_DESC(mem_dte_hit,             "csource=0x0a"),
	AMD_IOMMU_EVENT_DESC(mem_dte_mis,             "csource=0x0b"),
	AMD_IOMMU_EVENT_DESC(page_tbl_read_tot,       "csource=0x0c"),
	AMD_IOMMU_EVENT_DESC(page_tbl_read_nst,       "csource=0x0d"),
	AMD_IOMMU_EVENT_DESC(page_tbl_read_gst,       "csource=0x0e"),
	AMD_IOMMU_EVENT_DESC(int_dte_hit,             "csource=0x0f"),
	AMD_IOMMU_EVENT_DESC(int_dte_mis,             "csource=0x10"),
	AMD_IOMMU_EVENT_DESC(cmd_processed,           "csource=0x11"),
	AMD_IOMMU_EVENT_DESC(cmd_processed_inv,       "csource=0x12"),
	AMD_IOMMU_EVENT_DESC(tlb_inv,                 "csource=0x13"),
	AMD_IOMMU_EVENT_DESC(ign_rd_wr_mmio_1ff8h,    "csource=0x14"),
	AMD_IOMMU_EVENT_DESC(vapic_int_non_guest,     "csource=0x15"),
	AMD_IOMMU_EVENT_DESC(vapic_int_guest,         "csource=0x16"),
	AMD_IOMMU_EVENT_DESC(smi_recv,                "csource=0x17"),
	AMD_IOMMU_EVENT_DESC(smi_blk,                 "csource=0x18"),
	{ /* end: all zeroes */ },
};

/*---------------------------------------------
 * sysfs cpumask attributes
 *---------------------------------------------*/
static cpumask_t iommu_cpumask;

static ssize_t _iommu_cpumask_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &iommu_cpumask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, _iommu_cpumask_show, NULL);

static struct attribute *iommu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group amd_iommu_cpumask_group = {
	.attrs = iommu_cpumask_attrs,
};

/*---------------------------------------------*/

static int get_next_avail_iommu_bnk_cntr(struct perf_amd_iommu *perf_iommu)
{
	unsigned long flags;
	int shift, bank, cntr, retval;
	int max_banks = perf_iommu->max_banks;
	int max_cntrs = perf_iommu->max_counters;

	raw_spin_lock_irqsave(&perf_iommu->lock, flags);

	for (bank = 0, shift = 0; bank < max_banks; bank++) {
		for (cntr = 0; cntr < max_cntrs; cntr++) {
			shift = bank + (bank*3) + cntr;
			if (perf_iommu->cntr_assign_mask & (1ULL<<shift)) {
				continue;
			} else {
				perf_iommu->cntr_assign_mask |= (1ULL<<shift);
				retval = ((u16)((u16)bank<<8) | (u8)(cntr));
				goto out;
			}
		}
	}
	retval = -ENOSPC;
out:
	raw_spin_unlock_irqrestore(&perf_iommu->lock, flags);
	return retval;
}

static int clear_avail_iommu_bnk_cntr(struct perf_amd_iommu *perf_iommu,
					u8 bank, u8 cntr)
{
	unsigned long flags;
	int max_banks, max_cntrs;
	int shift = 0;

	max_banks = perf_iommu->max_banks;
	max_cntrs = perf_iommu->max_counters;

	if ((bank > max_banks) || (cntr > max_cntrs))
		return -EINVAL;

	shift = bank + cntr + (bank*3);

	raw_spin_lock_irqsave(&perf_iommu->lock, flags);
	perf_iommu->cntr_assign_mask &= ~(1ULL<<shift);
	raw_spin_unlock_irqrestore(&perf_iommu->lock, flags);

	return 0;
}

static int perf_iommu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct perf_amd_iommu *pi = container_of(event->pmu, struct perf_amd_iommu, pmu);

	/* test the event attr type check for PMU enumeration */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * IOMMU counters are shared across all cores.
	 * Therefore, it does not support per-process mode.
	 * Also, it does not support event sampling mode.
	 */
	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EINVAL;

	/* IOMMU counters do not have usr/os/guest/host bits */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_host || event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	/* update the hw_perf_event struct with the iommu config data */
	hwc->idx = pi->idx;
	hwc->config = event->attr.config;
	hwc->extra_reg.config = event->attr.config1;

	return 0;
}

static void perf_iommu_enable_event(struct perf_event *ev)
{
	struct hw_perf_event *hwc = &ev->hw;
	u8 csource = _GET_CSOURCE(ev);
	u16 devid = _GET_DEVID(ev);
	u8 bank = _GET_BANK(ev);
	u8 cntr = _GET_CNTR(ev);
	u64 reg = 0ULL;

	reg = csource;
	amd_iommu_pc_set_reg(hwc->idx, devid, bank, cntr,
			     IOMMU_PC_COUNTER_SRC_REG, &reg);

	reg = devid | (_GET_DEVID_MASK(ev) << 32);
	if (reg)
		reg |= BIT(31);
	amd_iommu_pc_set_reg(hwc->idx, devid, bank, cntr,
			     IOMMU_PC_DEVID_MATCH_REG, &reg);

	reg = _GET_PASID(ev) | (_GET_PASID_MASK(ev) << 32);
	if (reg)
		reg |= BIT(31);
	amd_iommu_pc_set_reg(hwc->idx, devid, bank, cntr,
			     IOMMU_PC_PASID_MATCH_REG, &reg);

	reg = _GET_DOMID(ev) | (_GET_DOMID_MASK(ev) << 32);
	if (reg)
		reg |= BIT(31);
	amd_iommu_pc_set_reg(hwc->idx, devid, bank, cntr,
			     IOMMU_PC_DOMID_MATCH_REG, &reg);
}

static void perf_iommu_disable_event(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 reg = 0ULL;

	amd_iommu_pc_set_reg(hwc->idx, _GET_DEVID(event), _GET_BANK(event),
			     _GET_CNTR(event), IOMMU_PC_COUNTER_SRC_REG, &reg);
}

static void perf_iommu_start(struct perf_event *event, int flags)
{
	u64 val;
	struct hw_perf_event *hwc = &event->hw;

	if (WARN_ON_ONCE(!(hwc->state & PERF_HES_STOPPED)))
		return;

	WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));
	hwc->state = 0;

	if (!(flags & PERF_EF_RELOAD))
		goto enable;

	val = local64_read(&hwc->prev_count);

	amd_iommu_pc_set_counter(hwc->idx, _GET_BANK(event), _GET_CNTR(event), &val);
enable:
	perf_iommu_enable_event(event);
	perf_event_update_userpage(event);

}

static void perf_iommu_read(struct perf_event *event)
{
	u64 cnt, prev;
	s64 delta;
	struct hw_perf_event *hwc = &event->hw;

	if (amd_iommu_pc_get_counter(hwc->idx, _GET_BANK(event), _GET_CNTR(event), &cnt))
		return;

	/* IOMMU pc counter register is only 48 bits */
	cnt &= GENMASK_ULL(48, 0);

	prev = local64_read(&hwc->prev_count);

	/*
	 * Since we do not enable counter overflow interrupts,
	 * we do not have to worry about prev_count changing on us.
	 */
	local64_set(&hwc->prev_count, cnt);

	/* Handle 48-bit counter overflow */
	delta = (cnt << COUNTER_SHIFT) - (prev << COUNTER_SHIFT);
	delta >>= COUNTER_SHIFT;
	local64_add(delta, &event->count);
}

static void perf_iommu_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 config;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	perf_iommu_disable_event(event);
	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);
	hwc->state |= PERF_HES_STOPPED;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	config = hwc->config;
	perf_iommu_read(event);
	hwc->state |= PERF_HES_UPTODATE;
}

static int perf_iommu_add(struct perf_event *event, int flags)
{
	int retval;
	struct perf_amd_iommu *perf_iommu =
			container_of(event->pmu, struct perf_amd_iommu, pmu);

	event->hw.state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	/* request an iommu bank/counter */
	retval = get_next_avail_iommu_bnk_cntr(perf_iommu);
	if (retval != -ENOSPC)
		event->hw.extra_reg.reg = (u16)retval;
	else
		return retval;

	if (flags & PERF_EF_START)
		perf_iommu_start(event, PERF_EF_RELOAD);

	return 0;
}

static void perf_iommu_del(struct perf_event *event, int flags)
{
	struct perf_amd_iommu *perf_iommu =
			container_of(event->pmu, struct perf_amd_iommu, pmu);

	perf_iommu_stop(event, PERF_EF_UPDATE);

	/* clear the assigned iommu bank/counter */
	clear_avail_iommu_bnk_cntr(perf_iommu,
				     _GET_BANK(event),
				     _GET_CNTR(event));

	perf_event_update_userpage(event);
}

static __init int _init_events_attrs(struct perf_amd_iommu *perf_iommu)
{
	struct attribute **attrs;
	struct attribute_group *attr_group;
	int i = 0, j;

	while (amd_iommu_v2_event_descs[i].attr.attr.name)
		i++;

	attr_group = kzalloc(sizeof(struct attribute *)
		* (i + 1) + sizeof(*attr_group), GFP_KERNEL);
	if (!attr_group)
		return -ENOMEM;

	attrs = (struct attribute **)(attr_group + 1);
	for (j = 0; j < i; j++)
		attrs[j] = &amd_iommu_v2_event_descs[j].attr.attr;

	attr_group->name = "events";
	attr_group->attrs = attrs;
	perf_iommu->events_group = attr_group;

	return 0;
}

static __init void amd_iommu_pc_exit(void)
{
	struct perf_amd_iommu *pi, *next;

	list_for_each_entry_safe(pi, next, &perf_amd_iommu_list, list) {
		list_del(&pi->list);

		kfree(pi->events_group);
		pi->events_group = NULL;

		kfree(pi);
	}
}

static __init int
init_one_perf_amd_iommu(struct perf_amd_iommu *perf_iommu, uint idx)
{
	int ret;

	raw_spin_lock_init(&perf_iommu->lock);

	/* Init format attributes */
	perf_iommu->format_group = &amd_iommu_format_group;

	/* Init cpumask attributes to only core 0 */
	cpumask_set_cpu(0, &iommu_cpumask);
	perf_iommu->cpumask_group = &amd_iommu_cpumask_group;

	/* Init events attributes */
	if (_init_events_attrs(perf_iommu) != 0)
		pr_err("Only support raw events.\n");

	snprintf(perf_iommu->name, PERF_AMD_IOMMU_NAME_SZ, "amd_iommu_%u", idx);
	perf_iommu->idx = idx;
	perf_iommu->max_banks = amd_iommu_pc_get_max_banks(idx);
	perf_iommu->max_counters = amd_iommu_pc_get_max_counters(idx);
	if (!perf_iommu->max_banks || !perf_iommu->max_counters)
		return -EINVAL;

	/* Init null attributes */
	perf_iommu->null_group = NULL;

	/* Setting up PMU */
	perf_iommu->pmu.event_init = perf_iommu_event_init,
	perf_iommu->pmu.add = perf_iommu_add,
	perf_iommu->pmu.del = perf_iommu_del,
	perf_iommu->pmu.start = perf_iommu_start,
	perf_iommu->pmu.stop = perf_iommu_stop,
	perf_iommu->pmu.read = perf_iommu_read,
	perf_iommu->pmu.task_ctx_nr = perf_invalid_context;
	perf_iommu->pmu.attr_groups = perf_iommu->attr_groups;

	ret = perf_pmu_register(&perf_iommu->pmu, perf_iommu->name, -1);
	if (ret) {
		pr_err("Error initializing AMD IOMMU perf counters.\n");
		amd_iommu_pc_exit();
	} else {
		pr_info("Detected %s, w/ %d banks, %d counters/bank\n",
			perf_iommu->name,
			amd_iommu_pc_get_max_banks(idx),
			amd_iommu_pc_get_max_counters(idx));

		list_add_tail(&perf_iommu->list, &perf_amd_iommu_list);
	}

	return ret;
}

static __init int amd_iommu_pc_init(void)
{
	uint i;

	/* Make sure the IOMMU PC resource is available */
	if (!amd_iommu_pc_supported())
		return -ENODEV;

	for (i = 0 ; i < amd_iommu_get_num_iommus(); i++) {
		int ret;
		struct perf_amd_iommu *pi;

		pi = kzalloc(sizeof(struct perf_amd_iommu), GFP_KERNEL);
		if (!pi)
			return -ENOMEM;

		ret = init_one_perf_amd_iommu(pi, i);
		if (ret)
			return ret;
	}

	return 0;
}

device_initcall(amd_iommu_pc_init);
