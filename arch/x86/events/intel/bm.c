/*
 * bm.c: support for Intel branch monitoring counters
 *
 * Copyright (c) 2017, Intel Corporation.
 *
 * Contact Information:
 * Megha Dey <megha.dey@linux.intel.com>
 * Yu-Cheng Yu <yucheng.yu@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/poll.h>

#include <asm/apic.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include <asm/nmi.h>

#include "../perf_event.h"

/* Branch Monitoring specific MSRs and mask values */
#define BR_DETECT_CONTROL_MSR	0x350
#define BR_DETECT_STATUS_MSR	0x351
#define BR_DETECT_COUNTER_CONFIG_BASE	0x354

#define MAX_WINDOW_SIZE	0x3ff
#define MAX_THRESHOLD	0x7f
#define MAX_BM_EVENTS	6
#define MAX_COUNTERS	2

#define WINDOW_SIZE_SHIFT	8
#define THRESHOLD_SHIFT	8
#define EVENT_TYPE_SHIFT	1

#define BM_ENABLE	0x3

#define THRESHOLD(cfg)	((cfg & GENMASK(14, 8)) >> 8)
#define SET_BIT0(reg)	(reg | 1)
#define CLEAR_BIT0(reg)	(reg & ~1)

/* Window size and threshold are cpu-global setting */
static int __window_size = MAX_WINDOW_SIZE;
static int __threshold	 = MAX_THRESHOLD;

atomic_t counter_used[MAX_COUNTERS] = {ATOMIC_INIT(0), ATOMIC_INIT(0)};

/* Branch monitoring counter owners */
struct perf_event       *bm_counter_owner[2];
static struct pmu intel_bm_pmu;

union bm_detect_status {
	struct {
		uint32_t Event: 1;
		uint32_t LBRsValid: 1;
		uint32_t Reserved0: 6;
		uint32_t CtrlHit0: 1;
		uint32_t CtrlHit1: 1;
		uint32_t Reserved1: 6;
		uint32_t CountWindow: 10;
		uint32_t Reserved2: 6;
		uint8_t count[2];
		uint32_t Reserved3: 16;
	} __packed;
	uint64_t raw;
};

static int intel_bm_event_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
	struct perf_event *event;
	union bm_detect_status stat;

	rdmsrl(BR_DETECT_STATUS_MSR, stat.raw);
	/* check if Branch monitoring interrupt has occurred */
	if (stat.Event) {
		wrmsrl(BR_DETECT_STATUS_MSR, 0);
		apic_write(APIC_LVTPC, APIC_DM_NMI);
		/*
		 * 2 interrupts can happen simultaneously. Issue wake-up to
		 * corrresponding polling event
		 */
		if (stat.CtrlHit0) {
			event = bm_counter_owner[0];
			atomic_set(&event->hw.bm_poll, POLLIN);
			event->pending_wakeup = 1;
			irq_work_queue(&event->pending);
		}
		if (stat.CtrlHit1) {
			event = bm_counter_owner[1];
			atomic_set(&event->hw.bm_poll, POLLIN);
			event->pending_wakeup = 1;
			irq_work_queue(&event->pending);
		}
		return NMI_HANDLED;
	}
	return NMI_DONE;
}

/* Start counting Branch monitoring events */
static void intel_bm_event_start(struct perf_event *event, int mode)
{
	if (event->id >= MAX_COUNTERS)
		return;

	wrmsrl(BR_DETECT_COUNTER_CONFIG_BASE + event->id,
	       SET_BIT0(event->hw.bm_counter_conf));
}

static int intel_bm_event_add(struct perf_event *event, int mode)
{
	union bm_detect_status cur_stat, prev_stat;

	prev_stat.raw = local64_read(&event->hw.prev_count);

	/* Start counting from previous count associated with this event */
	rdmsrl(BR_DETECT_STATUS_MSR, cur_stat.raw);

	cur_stat.count[event->id] = prev_stat.count[event->id];
	cur_stat.CountWindow = prev_stat.CountWindow;
	wrmsrl(BR_DETECT_STATUS_MSR, cur_stat.raw);

	wrmsrl(BR_DETECT_CONTROL_MSR, event->hw.bm_ctrl);

	intel_bm_event_start(event, mode);

	return 0;
}

static void intel_bm_event_update(struct perf_event *event)
{
	union bm_detect_status cur_stat;

	rdmsrl(BR_DETECT_STATUS_MSR, cur_stat.raw);
	local64_set(&event->hw.prev_count, (uint64_t)cur_stat.raw);
}

static void intel_bm_event_stop(struct perf_event *event, int mode)
{
	if (event->id >= MAX_COUNTERS)
		return;
	wrmsrl(BR_DETECT_COUNTER_CONFIG_BASE + event->id,
	       CLEAR_BIT0(event->hw.bm_counter_conf));

	intel_bm_event_update(event);
}

static void intel_bm_event_del(struct perf_event *event, int flags)
{
	intel_bm_event_stop(event, flags);
}

static void intel_bm_event_destroy(struct perf_event *event)
{
	atomic_set(&counter_used[event->id], 0);
}

static int intel_bm_event_init(struct perf_event *event)
{
	u64 cfg;
	int counter_to_use;

	local64_set(&event->hw.prev_count, 0);
	/*
	 * Type is assigned by kernel, see /sys/devices/intel_bm/type
	 */

	if (event->attr.type != intel_bm_pmu.type)
		return -ENOENT;

	event->event_caps |= BIT(PERF_EV_CAP_BM);
	/*
	 * cfg contains one of the 6 possible Branch Monitoring events
	 */
	cfg = event->attr.config;
	if (cfg < 0 || cfg > (MAX_BM_EVENTS - 1))
		return -EINVAL;

	/* only 2 counters present .. update atomically */
	if (!(atomic_cmpxchg(&counter_used[0], 0, 1))) {
		counter_to_use = 0;
		bm_counter_owner[0] = event;
	} else if (!(atomic_cmpxchg(&counter_used[1], 0, 1))) {
		counter_to_use = 1;
		bm_counter_owner[1] = event;
	} else {
		pr_err("All counters are in use\n");
		return -EINVAL;
	}

	event->hw.bm_ctrl = (__window_size << WINDOW_SIZE_SHIFT) | BM_ENABLE;
	event->hw.bm_counter_conf = (__threshold << THRESHOLD_SHIFT) |
						(cfg << EVENT_TYPE_SHIFT);

	/*
	 * Update counter_config register with event type and obtain thresholds
	 * of counter being used
	 */
	wrmsrl(BR_DETECT_COUNTER_CONFIG_BASE + counter_to_use,
						event->hw.bm_counter_conf);
	wrmsrl(BR_DETECT_STATUS_MSR, 0);
	event->id = counter_to_use;
	local64_set(&event->count, 0);

	event->destroy = intel_bm_event_destroy;

	return 0;
}

EVENT_ATTR_STR(rets, rets, "event=0x0");
EVENT_ATTR_STR(call-ret, call_ret, "event=0x01");
EVENT_ATTR_STR(ret-misp, ret_misp, "event=0x02");
EVENT_ATTR_STR(branch-misp, branch_mispredict, "event=0x03");
EVENT_ATTR_STR(indirect-branch-misp, indirect_branch_mispredict, "event=0x04");
EVENT_ATTR_STR(far-branch, far_branch, "event=0x05");

static struct attribute *intel_bm_events_attr[] = {
	EVENT_PTR(rets),
	EVENT_PTR(call_ret),
	EVENT_PTR(ret_misp),
	EVENT_PTR(branch_mispredict),
	EVENT_PTR(indirect_branch_mispredict),
	EVENT_PTR(far_branch),
	NULL,
};

static struct attribute_group intel_bm_events_group = {
	.name = "events",
	.attrs = intel_bm_events_attr,
};

PMU_FORMAT_ATTR(event, "config:0-7");
static struct attribute *intel_bm_formats_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group intel_bm_format_group = {
	.name = "format",
	.attrs = intel_bm_formats_attr,
};

/*
 * User can configure the BM MSRs using the corresponding sysfs entries
 */

static ssize_t
threshold_show(struct device *dev, struct device_attribute *attr,
			char *page)
{
	ssize_t rv;

	rv = snprintf(page, PAGE_SIZE-1, "%d\n", __threshold);

	return rv;
}

static ssize_t
threshold_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned int threshold;
	int err;

	err = kstrtouint(buf, 0, &threshold);
	if (err)
		return err;

	if (threshold > MAX_THRESHOLD) {
		pr_err("invalid threshold value\n");
		return -EINVAL;
	}

	__threshold = threshold;

	return count;
}

static DEVICE_ATTR_RW(threshold);

static ssize_t
window_size_show(struct device *dev, struct device_attribute *attr,
			char *page)
{
	ssize_t rv;

	rv = snprintf(page, PAGE_SIZE-1, "%d\n", __window_size);

	return rv;
}

static ssize_t
window_size_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned int window_size;
	int err;

	err = kstrtouint(buf, 0, &window_size);
	if (err)
		return err;

	if (atomic_read(&counter_used[0]) && atomic_read(&counter_used[1])) {
		pr_err("All counters in use. Cannot modify window size\n");
		return -EBUSY;
	}

	if (window_size > MAX_WINDOW_SIZE) {
		pr_err("illegal window size\n");
		return -EINVAL;
	}

	__window_size = window_size;

	return count;
}

static DEVICE_ATTR_RW(window_size);

static struct attribute *intel_bm_attrs[] = {
	&dev_attr_window_size.attr,
	&dev_attr_threshold.attr,
	NULL,
};

static const struct attribute_group intel_bm_group = {
	.attrs = intel_bm_attrs,
};

static const struct attribute_group *intel_bm_attr_groups[] = {
	&intel_bm_events_group,
	&intel_bm_format_group,
	&intel_bm_group,
	NULL,
};

static struct pmu intel_bm_pmu = {
	.task_ctx_nr     = perf_sw_context,
	.attr_groups     = intel_bm_attr_groups,
	.event_init      = intel_bm_event_init,
	.add             = intel_bm_event_add,
	.del             = intel_bm_event_del,
	.start           = intel_bm_event_start,
	.stop            = intel_bm_event_stop,
};

#define X86_BM_MODEL_MATCH(model)       \
	{ X86_VENDOR_INTEL, 6, model, X86_FEATURE_ANY }

static const struct x86_cpu_id bm_cpu_match[] __initconst = {
	X86_BM_MODEL_MATCH(INTEL_FAM6_CANNONLAKE_CORE),
	{},
};

MODULE_DEVICE_TABLE(x86cpu, bm_cpu_match);

static __init int intel_bm_init(void)
{
	int ret;

	/*
	 * Only CNL and ICL support branch monitoring
	 */
	if (!(x86_match_cpu(bm_cpu_match))) {
		pr_info("This system does not support branch monitoring\n");
		return -ENODEV;
	}

	register_nmi_handler(NMI_LOCAL, intel_bm_event_nmi_handler, 0, "BM");

	ret =  perf_pmu_register(&intel_bm_pmu, "intel_bm", -1);
	if (ret) {
		pr_err("Intel BM perf registration failed: %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(intel_bm_init);

static void __exit intel_bm_exit(void)
{
	perf_pmu_unregister(&intel_bm_pmu);
}
module_exit(intel_bm_exit);

MODULE_LICENSE("GPL");
