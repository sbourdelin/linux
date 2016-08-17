/*
 * Copyright 2011,2016 Freescale Semiconductor, Inc.
 * Copyright 2011 Linaro Ltd.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

#include "common.h"

#define MMDC_MAPSR		0x404
#define BP_MMDC_MAPSR_PSD	0
#define BP_MMDC_MAPSR_PSS	4

#define MMDC_MDMISC		0x18
#define BM_MMDC_MDMISC_DDR_TYPE	0x18
#define BP_MMDC_MDMISC_DDR_TYPE	0x3

#define TOTAL_CYCLES	0x0
#define BUSY_CYCLES		0x1
#define READ_ACCESSES	0x2
#define WRITE_ACCESSES	0x3
#define READ_BYTES		0x4
#define WRITE_BYTES		0x5

/* Enables, resets, freezes, overflow profiling*/
#define DBG_DIS			0x0
#define DBG_EN			0x1
#define DBG_RST			0x2
#define PRF_FRZ			0x4
#define CYC_OVF			0x8

#define MMDC_MADPCR0	0x410
#define MMDC_MADPSR0	0x418
#define MMDC_MADPSR1	0x41C
#define MMDC_MADPSR2	0x420
#define MMDC_MADPSR3	0x424
#define MMDC_MADPSR4	0x428
#define MMDC_MADPSR5	0x42C

#define MMDC_NUM_COUNTERS	6

#define to_mmdc_pmu(p) (container_of(p, struct mmdc_pmu, pmu))

static DEFINE_IDA(mmdc_ida);

static int ddr_type;

PMU_EVENT_ATTR_STRING(total-cycles, mmdc_total_cycles, "event=0x00")
PMU_EVENT_ATTR_STRING(busy-cycles, mmdc_busy_cycles, "event=0x01")
PMU_EVENT_ATTR_STRING(read-accesses, mmdc_read_accesses, "event=0x02")
PMU_EVENT_ATTR_STRING(write-accesses, mmdc_write_accesses, "config=0x03")
PMU_EVENT_ATTR_STRING(read-bytes, mmdc_read_bytes, "event=0x04")
PMU_EVENT_ATTR_STRING(read-bytes.unit, mmdc_read_bytes_unit, "MB");
PMU_EVENT_ATTR_STRING(read-bytes.scale, mmdc_read_bytes_scale, "0.000001");
PMU_EVENT_ATTR_STRING(write-bytes, mmdc_write_bytes, "event=0x05")
PMU_EVENT_ATTR_STRING(write-bytes.unit, mmdc_write_bytes_unit, "MB");
PMU_EVENT_ATTR_STRING(write-bytes.scale, mmdc_write_bytes_scale, "0.000001");

struct mmdc_pmu {
	struct pmu pmu;
	void __iomem *mmdc_base;
	cpumask_t cpu;
	struct hrtimer hrtimer;
	unsigned int irq;
	unsigned int active_events;
	struct device *dev;
	struct perf_event *mmdc_events[MMDC_NUM_COUNTERS];
	spinlock_t mmdc_active_events_lock;
};
static struct mmdc_pmu *cpuhp_mmdc_pmu;

/* polling period is set to one second, overflow of total-cycles (the fastest
 * increasing counter) takes ten seconds so one second is safe
 */
static unsigned int mmdc_poll_period_us = 1000000;
module_param_named(pmu_poll_period_us, mmdc_poll_period_us, uint,
		S_IRUGO | S_IWUSR);

static ktime_t mmdc_timer_period(void)
{
	return ns_to_ktime((u64)mmdc_poll_period_us * 1000);
}

static ssize_t mmdc_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mmdc_pmu *pmu_mmdc = dev_get_drvdata(dev);

	return cpumap_print_to_pagebuf(true, buf, &pmu_mmdc->cpu);
}

static struct device_attribute mmdc_cpumask_attr =
__ATTR(cpumask, S_IRUGO, mmdc_cpumask_show, NULL);

static struct attribute *mmdc_cpumask_attrs[] = {
	&mmdc_cpumask_attr.attr,
	NULL,
};

static struct attribute_group mmdc_cpumask_attr_group = {
	.attrs = mmdc_cpumask_attrs,
};

static struct attribute *mmdc_events_attrs[] = {
	&mmdc_total_cycles.attr.attr,
	&mmdc_busy_cycles.attr.attr,
	&mmdc_read_accesses.attr.attr,
	&mmdc_write_accesses.attr.attr,
	&mmdc_read_bytes.attr.attr,
	&mmdc_read_bytes_unit.attr.attr,
	&mmdc_read_bytes_scale.attr.attr,
	&mmdc_write_bytes.attr.attr,
	&mmdc_write_bytes_unit.attr.attr,
	&mmdc_write_bytes_scale.attr.attr,
	NULL,
};

static struct attribute_group mmdc_events_attr_group = {
	.name = "events",
	.attrs = mmdc_events_attrs,
};

PMU_FORMAT_ATTR(event, "config:0-63");
static struct attribute *mmdc_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group mmdc_format_attr_group = {
	.name = "format",
	.attrs = mmdc_format_attrs,
};

static const struct attribute_group *attr_groups[] = {
	&mmdc_events_attr_group,
	&mmdc_format_attr_group,
	&mmdc_cpumask_attr_group,
	NULL,
};

static u32 mmdc_read_counter(struct mmdc_pmu *pmu_mmdc, int cfg, u64 prev_val)
{
	u32 val;
	void __iomem *mmdc_base, *reg;

	mmdc_base = pmu_mmdc->mmdc_base;

	switch (cfg) {
	case TOTAL_CYCLES:
		reg = mmdc_base + MMDC_MADPSR0;
		break;
	case BUSY_CYCLES:
		reg = mmdc_base + MMDC_MADPSR1;
		break;
	case READ_ACCESSES:
		reg = mmdc_base + MMDC_MADPSR2;
		break;
	case WRITE_ACCESSES:
		reg = mmdc_base + MMDC_MADPSR3;
		break;
	case READ_BYTES:
		reg = mmdc_base + MMDC_MADPSR4;
		break;
	case WRITE_BYTES:
		reg = mmdc_base + MMDC_MADPSR5;
		break;
	default:
		return WARN_ONCE(1,
			"invalid configuration %d for mmdc counter", cfg);
	}
	val = readl(reg);
	return val;
}

static int mmdc_pmu_offline_cpu(unsigned int cpu)
{
	struct mmdc_pmu *pmu_mmdc = cpuhp_mmdc_pmu;
	int target;

	if (!cpumask_test_and_clear_cpu(cpu, &pmu_mmdc->cpu))
		return 0;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;
	perf_pmu_migrate_context(&pmu_mmdc->pmu, cpu, target);
	cpumask_set_cpu(target, &pmu_mmdc->cpu);
	if (pmu_mmdc->irq)
		WARN_ON(irq_set_affinity_hint(
					pmu_mmdc->irq, &pmu_mmdc->cpu) != 0);
	return 0;
}

static int mmdc_event_init(struct perf_event *event)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);
	int cfg = event->attr.config;
	struct perf_event *sibling;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (event->cpu < 0) {
		dev_warn(pmu_mmdc->dev, "Can't provide per-task data!\n");
		return -EOPNOTSUPP;
	}

	if (event->attr.exclude_user		||
			event->attr.exclude_kernel	||
			event->attr.exclude_hv		||
			event->attr.exclude_idle	||
			event->attr.exclude_host	||
			event->attr.exclude_guest	||
			event->attr.sample_period)
		return -EINVAL;

	if (cfg < 0 || cfg >= MMDC_NUM_COUNTERS)
		return -EINVAL;

	if (event->group_leader->pmu != event->pmu &&
			!is_software_event(event->group_leader))
		return -EINVAL;

	list_for_each_entry(sibling, &event->group_leader->sibling_list,
			group_entry)
		if (sibling->pmu != event->pmu &&
				!is_software_event(sibling))
			return -EINVAL;

	event->cpu = cpumask_first(&pmu_mmdc->cpu);
	return 0;
}

static void mmdc_event_update(struct perf_event *event)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);
	u32 val;
	u64 prev_val;

	prev_val = local64_read(&event->count);
	val = mmdc_read_counter(pmu_mmdc, (int) event->attr.config, prev_val);
	local64_add(val - (u32)(prev_val & 0xFFFFFFFF), &event->count);
}

static void mmdc_event_start(struct perf_event *event, int flags)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);
	void __iomem *mmdc_base, *reg;

	mmdc_base = pmu_mmdc->mmdc_base;
	reg = mmdc_base + MMDC_MADPCR0;
	/* hrtimer is required because mmdc does not provide an interrupt so
	 * polling is necessary
	 */
	hrtimer_start(&pmu_mmdc->hrtimer, mmdc_timer_period(),
			HRTIMER_MODE_REL_PINNED);

	writel(DBG_RST, reg);
	writel(DBG_EN, reg);
}

static int mmdc_event_add(struct perf_event *event, int flags)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);
	int cfg = (int)event->attr.config;

	if (WARN_ONCE((cfg < 0 || cfg >= MMDC_NUM_COUNTERS),
				"invalid configuration %d for mmdc", cfg))
		return -1;
	pmu_mmdc->mmdc_events[cfg] = event;
	local64_set(&event->count, 0);
	if (flags & PERF_EF_START)
		mmdc_event_start(event, flags);
	spin_lock(&pmu_mmdc->mmdc_active_events_lock);
	pmu_mmdc->active_events++;
	spin_unlock(&pmu_mmdc->mmdc_active_events_lock);
	return 0;
}

static void mmdc_event_stop(struct perf_event *event, int flags)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);
	void __iomem *mmdc_base, *reg;
	int cfg = (int)event->attr.config;

	mmdc_base = pmu_mmdc->mmdc_base;
	reg = mmdc_base + MMDC_MADPCR0;
	if (WARN_ONCE((cfg < 0 || cfg >= MMDC_NUM_COUNTERS),
				"invalid configuration %d for mmdc counter", cfg))
		return;
	spin_lock(&pmu_mmdc->mmdc_active_events_lock);
	if (pmu_mmdc->active_events <= 0)
		hrtimer_cancel(&pmu_mmdc->hrtimer);
	spin_unlock(&pmu_mmdc->mmdc_active_events_lock);
	writel(PRF_FRZ, reg);
	mmdc_event_update(event);
}

static void mmdc_event_del(struct perf_event *event, int flags)
{
	struct mmdc_pmu *pmu_mmdc = to_mmdc_pmu(event->pmu);

	spin_lock(&pmu_mmdc->mmdc_active_events_lock);
	pmu_mmdc->active_events--;
	spin_unlock(&pmu_mmdc->mmdc_active_events_lock);
	mmdc_event_stop(event, PERF_EF_UPDATE);
}

static void mmdc_overflow_handler(struct mmdc_pmu *pmu_mmdc)
{
	int i;

	for (i = 0; i < MMDC_NUM_COUNTERS; i++) {
		struct perf_event *event = pmu_mmdc->mmdc_events[i];

		if (event)
			mmdc_event_update(event);
	}
}

static enum hrtimer_restart mmdc_timer_handler(struct hrtimer *hrtimer)
{
	struct mmdc_pmu *pmu_mmdc = container_of(hrtimer, struct mmdc_pmu,
			hrtimer);

	mmdc_overflow_handler(pmu_mmdc);

	hrtimer_forward_now(hrtimer, mmdc_timer_period());
	return HRTIMER_RESTART;
}

static int mmdc_pmu_init(struct mmdc_pmu *pmu_mmdc,
		void __iomem *mmdc_base, struct device *dev)
{
	int mmdc_num;

	*pmu_mmdc = (struct mmdc_pmu) {
		.pmu = (struct pmu) {
			.task_ctx_nr    = perf_invalid_context,
			.attr_groups    = attr_groups,
			.event_init     = mmdc_event_init,
			.add            = mmdc_event_add,
			.del            = mmdc_event_del,
			.start          = mmdc_event_start,
			.stop           = mmdc_event_stop,
			.read           = mmdc_event_update,
		},
		.mmdc_base = mmdc_base,
	};

	mmdc_num = ida_simple_get(&mmdc_ida, 0, 0, GFP_KERNEL);

	cpumask_set_cpu(smp_processor_id(), &pmu_mmdc->cpu);

	pmu_mmdc->dev = dev;
	pmu_mmdc->active_events = 0;
	spin_lock_init(&pmu_mmdc->mmdc_active_events_lock);

	cpuhp_mmdc_pmu = pmu_mmdc;
	cpuhp_setup_state(CPUHP_ONLINE,
			"PERF_MMDC_ONLINE", NULL,
			mmdc_pmu_offline_cpu);

	return mmdc_num;
}

static int imx_mmdc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	void __iomem *mmdc_base, *reg;
	struct mmdc_pmu *pmu_mmdc;
	char *name;
	u32 val;
	int timeout = 0x400;
	int mmdc_num;

	mmdc_base = of_iomap(np, 0);
	WARN_ON(!mmdc_base);

	reg = mmdc_base + MMDC_MDMISC;
	/* Get ddr type */
	val = readl_relaxed(reg);
	ddr_type = (val & BM_MMDC_MDMISC_DDR_TYPE) >>
		 BP_MMDC_MDMISC_DDR_TYPE;

	reg = mmdc_base + MMDC_MAPSR;

	/* Enable automatic power saving */
	val = readl_relaxed(reg);
	val &= ~(1 << BP_MMDC_MAPSR_PSD);
	writel_relaxed(val, reg);

	/* Ensure it's successfully enabled */
	while (!(readl_relaxed(reg) & 1 << BP_MMDC_MAPSR_PSS) && --timeout)
		cpu_relax();

	if (unlikely(!timeout)) {
		pr_warn("%s: failed to enable automatic power saving\n",
			__func__);
		return -EBUSY;
	}
	pmu_mmdc = kzalloc(sizeof(*pmu_mmdc), GFP_KERNEL);

	if (!pmu_mmdc) {
		pr_err("failed to allocate PMU device!\n");
		return -ENOMEM;
	}
	mmdc_num = mmdc_pmu_init(pmu_mmdc, mmdc_base, &pdev->dev);
	hrtimer_init(&pmu_mmdc->hrtimer, CLOCK_MONOTONIC,
			HRTIMER_MODE_REL);
	pmu_mmdc->hrtimer.function = mmdc_timer_handler;
	if (mmdc_num == 0)
		name = "mmdc";
	else
		name = devm_kasprintf(&pdev->dev,
				GFP_KERNEL, "mmdc_%d", mmdc_num);
	platform_set_drvdata(pdev, pmu_mmdc);
	perf_pmu_register(&(pmu_mmdc->pmu), name, -1);
	return 0;
}

static int imx_mmdc_remove(struct platform_device *pdev)
{
	struct mmdc_pmu *pmu_mmdc = platform_get_drvdata(pdev);

	perf_pmu_unregister(&pmu_mmdc->pmu);
	cpuhp_remove_state_nocalls(CPUHP_ONLINE);
	cpuhp_mmdc_pmu = NULL;
	kfree(pmu_mmdc);
	return 0;
}

int imx_mmdc_get_ddr_type(void)
{
	return ddr_type;
}

static const struct of_device_id imx_mmdc_dt_ids[] = {
	{ .compatible = "fsl,imx6q-mmdc", },
	{ /* sentinel */ }
};

static struct platform_driver imx_mmdc_driver = {
	.driver		= {
		.name	= "imx-mmdc",
		.of_match_table = imx_mmdc_dt_ids,
	},
	.probe		= imx_mmdc_probe,
	.remove		= imx_mmdc_remove,
};

static int __init imx_mmdc_init(void)
{
	return platform_driver_register(&imx_mmdc_driver);
}
postcore_initcall(imx_mmdc_init);
