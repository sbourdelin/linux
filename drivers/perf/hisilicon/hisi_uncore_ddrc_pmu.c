/*
 * HiSilicon SoC DDRC uncore Hardware event counters support
 *
 * Copyright (C) 2017 Hisilicon Limited
 * Author: Shaokun Zhang <zhangshaokun@hisilicon.com>
 *         Anurup M <anurup.m@huawei.com>
 *
 * This code is based on the uncore PMUs like arm-cci and arm-ccn.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include "hisi_uncore_pmu.h"

/* DDRC register definition */
#define DDRC_PERF_CTRL		0x010
#define DDRC_FLUX_WR		0x380
#define DDRC_FLUX_RD		0x384
#define DDRC_FLUX_WCMD          0x388
#define DDRC_FLUX_RCMD          0x38c
#define DDRC_PRE_CMD            0x3c0
#define DDRC_ACT_CMD            0x3c4
#define DDRC_BNK_CHG            0x3c8
#define DDRC_RNK_CHG            0x3cc
#define DDRC_EVENT_CTRL         0x6C0
#define DDRC_INT_MASK		0x6c8
#define DDRC_INT_STATUS		0x6cc
#define DDRC_INT_CLEAR		0x6d0

/* DDRC supports 8-events and counter is fixed-purpose */
#define DDRC_NR_COUNTERS	0x8
#define DDRC_NR_EVENTS		DDRC_NR_COUNTERS

#define DDRC_PERF_CTRL_EN	0x2

/*
 * For DDRC PMU, there are eight-events and every event has been mapped
 * to fixed-purpose counters which register offset is not consistent.
 * Therefore there is no write event type and we assume that event
 * code (0 to 7) is equal to counter index in PMU driver.
 */
#define GET_DDRC_EVENTID(hwc)	(hwc->config_base & 0x7)

static const u32 ddrc_reg_off[] = {
	DDRC_FLUX_WR, DDRC_FLUX_RD, DDRC_FLUX_WCMD, DDRC_FLUX_RCMD,
	DDRC_PRE_CMD, DDRC_ACT_CMD, DDRC_BNK_CHG, DDRC_RNK_CHG
};

/*
 * Select the counter register offset using the counter index.
 * In DDRC there are no programmable counter, the count
 * is readed form the statistics counter register itself.
 */
static u32 get_counter_reg_off(int cntr_idx)
{
	return ddrc_reg_off[cntr_idx];
}

static u64 hisi_ddrc_pmu_read_counter(struct hisi_pmu *ddrc_pmu,
				      struct hw_perf_event *hwc)
{
	/* Use event code as counter index */
	u32 idx = GET_DDRC_EVENTID(hwc);
	u32 reg;

	if (!hisi_uncore_pmu_counter_valid(ddrc_pmu, idx)) {
		dev_err(ddrc_pmu->dev, "Unsupported event index:%d!\n", idx);
		return 0;
	}

	reg = get_counter_reg_off(idx);

	return readl(ddrc_pmu->base + reg);
}

static void hisi_ddrc_pmu_write_counter(struct hisi_pmu *ddrc_pmu,
					struct hw_perf_event *hwc, u64 val)
{
	u32 idx = GET_DDRC_EVENTID(hwc);
	u32 reg;

	if (!hisi_uncore_pmu_counter_valid(ddrc_pmu, idx)) {
		dev_err(ddrc_pmu->dev, "Unsupported event index:%d!\n", idx);
		return;
	}

	reg = get_counter_reg_off(idx);
	writel((u32)val, ddrc_pmu->base + reg);
}

static void hisi_ddrc_pmu_start_counters(struct hisi_pmu *ddrc_pmu)
{
	u32 val;

	/* Set perf_enable in DDRC_PERF_CTRL to start event counting */
	val = readl(ddrc_pmu->base + DDRC_PERF_CTRL);
	val |= DDRC_PERF_CTRL_EN;
	writel(val, ddrc_pmu->base + DDRC_PERF_CTRL);
}

static void hisi_ddrc_pmu_stop_counters(struct hisi_pmu *ddrc_pmu)
{
	u32 val;

	/* Clear perf_enable in DDRC_PERF_CTRL to stop event counting */
	val = readl(ddrc_pmu->base + DDRC_PERF_CTRL);
	val &= ~DDRC_PERF_CTRL_EN;
	writel(val, ddrc_pmu->base + DDRC_PERF_CTRL);
}

static void hisi_ddrc_pmu_enable_counter(struct hisi_pmu *ddrc_pmu,
					 struct hw_perf_event *hwc)
{
	u32 val;

	/* Set counter index(event code) in DDRC_EVENT_CTRL register */
	val = readl(ddrc_pmu->base + DDRC_EVENT_CTRL);
	val |= (1 << GET_DDRC_EVENTID(hwc));
	writel(val, ddrc_pmu->base + DDRC_EVENT_CTRL);
}

static void hisi_ddrc_pmu_disable_counter(struct hisi_pmu *ddrc_pmu,
					  struct hw_perf_event *hwc)
{
	u32 val;

	/* Clear counter index(event code) in DDRC_EVENT_CTRL register */
	val = readl(ddrc_pmu->base + DDRC_EVENT_CTRL);
	val &= ~(1 << GET_DDRC_EVENTID(hwc));
	writel(val, ddrc_pmu->base + DDRC_EVENT_CTRL);
}

static int hisi_ddrc_pmu_get_event_idx(struct perf_event *event)
{
	struct hisi_pmu *ddrc_pmu = to_hisi_pmu(event->pmu);
	unsigned long *used_mask = ddrc_pmu->pmu_events.used_mask;
	struct hw_perf_event *hwc = &event->hw;
	/* For DDRC PMU, we use event code as counter index */
	int idx = GET_DDRC_EVENTID(hwc);

	if (test_bit(idx, used_mask))
		return -EAGAIN;

	set_bit(idx, used_mask);

	return idx;
}

static void hisi_ddrc_pmu_enable_counter_int(struct hisi_pmu *ddrc_pmu,
					     struct hw_perf_event *hwc)
{
	u32 val;

	/* Write 0 to enable interrupt */
	val = readl(ddrc_pmu->base + DDRC_INT_MASK);
	val &= ~(1 << GET_DDRC_EVENTID(hwc));
	writel(val, ddrc_pmu->base + DDRC_INT_MASK);
}

static void hisi_ddrc_pmu_disable_counter_int(struct hisi_pmu *ddrc_pmu,
					      struct hw_perf_event *hwc)
{
	u32 val;

	/* Write 1 to mask interrupt */
	val = readl(ddrc_pmu->base + DDRC_INT_MASK);
	val |= (1 << GET_DDRC_EVENTID(hwc));
	writel(val, ddrc_pmu->base + DDRC_INT_MASK);
}

static irqreturn_t hisi_ddrc_pmu_isr(int irq, void *dev_id)
{
	struct hisi_pmu *ddrc_pmu = dev_id;
	struct perf_event *event;
	unsigned long overflown;
	u32 status;
	int idx;

	/* Read the DDRC_INT_STATUS register */
	status = readl(ddrc_pmu->base + DDRC_INT_STATUS);
	if (!status)
		return IRQ_NONE;
	overflown = status;

	/*
	 * Find the counter index which overflowed if the bit was set
	 * and handle it
	 */
	for_each_set_bit(idx, &overflown, DDRC_NR_COUNTERS) {
		/* Write 1 to clear the IRQ status flag */
		writel((1 << idx), ddrc_pmu->base + DDRC_INT_CLEAR);

		/* Get the corresponding event struct */
		event = ddrc_pmu->pmu_events.hw_events[idx];
		if (!event)
			continue;

		hisi_uncore_pmu_event_update(event);
		hisi_uncore_pmu_set_event_period(event);
	}

	return IRQ_HANDLED;
}

static int hisi_ddrc_pmu_init_irq(struct hisi_pmu *ddrc_pmu,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int irq, ret;

	/* Read and init IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "irq init: fail map DDRC overflow interrupt\n");
		return irq;
	}

	ret = devm_request_irq(dev, irq, hisi_ddrc_pmu_isr,
			       IRQF_NOBALANCING | IRQF_NO_THREAD,
			       dev_name(dev), ddrc_pmu);
	if (ret < 0) {
		dev_err(dev, "Fail to request IRQ:%d ret:%d\n", irq, ret);
		return ret;
	}

	/* Overflow interrupt also should use the same CPU */
	WARN_ON(irq_set_affinity(irq, &ddrc_pmu->cpus));

	return 0;
}

static const struct acpi_device_id hisi_ddrc_pmu_acpi_match[] = {
	{ "HISI0233", },
	{},
};
MODULE_DEVICE_TABLE(acpi, hisi_ddrc_pmu_acpi_match);

static int hisi_ddrc_pmu_init_data(struct platform_device *pdev,
				   struct hisi_pmu *ddrc_pmu)
{
	struct device *dev = &pdev->dev;
	struct resource *res;

	/* Get the DDRC Channel ID */
	if (device_property_read_u32(dev, "hisilicon,ch-id",
				     &ddrc_pmu->ddrc_chn_id)) {
		dev_err(dev, "Can not read ddrc ch-id!\n");
		return -EINVAL;
	}

	/* Get the DDRC SCCL ID */
	if (device_property_read_u32(dev, "hisilicon,scl-id",
				     &ddrc_pmu->scl_id)) {
		dev_err(dev, "Can not read ddrc scl-id!\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ddrc_pmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ddrc_pmu->base))
		return PTR_ERR(ddrc_pmu->base);

	return 0;
}

static struct attribute *hisi_ddrc_pmu_format_attr[] = {
	HISI_PMU_FORMAT_ATTR(event, "config:0-4"),
	NULL,
};

static const struct attribute_group hisi_ddrc_pmu_format_group = {
	.name = "format",
	.attrs = hisi_ddrc_pmu_format_attr,
};

static struct attribute *hisi_ddrc_pmu_events_attr[] = {
	HISI_PMU_EVENT_ATTR(flux_wr,		0x00),
	HISI_PMU_EVENT_ATTR(flux_rd,		0x01),
	HISI_PMU_EVENT_ATTR(flux_wcmd,		0x02),
	HISI_PMU_EVENT_ATTR(flux_rcmd,		0x03),
	HISI_PMU_EVENT_ATTR(pre_cmd,		0x04),
	HISI_PMU_EVENT_ATTR(act_cmd,		0x05),
	HISI_PMU_EVENT_ATTR(rnk_chg,		0x06),
	HISI_PMU_EVENT_ATTR(rw_chg,		0x07),
	NULL,
};

static const struct attribute_group hisi_ddrc_pmu_events_group = {
	.name = "events",
	.attrs = hisi_ddrc_pmu_events_attr,
};

static struct attribute *hisi_ddrc_pmu_attrs[] = {
	NULL,
};

static const struct attribute_group hisi_ddrc_pmu_attr_group = {
	.attrs = hisi_ddrc_pmu_attrs,
};

static DEVICE_ATTR(cpumask, 0444, hisi_cpumask_sysfs_show, NULL);

static struct attribute *hisi_ddrc_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group hisi_ddrc_pmu_cpumask_attr_group = {
	.attrs = hisi_ddrc_pmu_cpumask_attrs,
};

static const struct attribute_group *hisi_ddrc_pmu_attr_groups[] = {
	&hisi_ddrc_pmu_attr_group,
	&hisi_ddrc_pmu_format_group,
	&hisi_ddrc_pmu_events_group,
	&hisi_ddrc_pmu_cpumask_attr_group,
	NULL,
};

static const struct hisi_uncore_ops hisi_uncore_ddrc_ops = {
	.get_event_idx		= hisi_ddrc_pmu_get_event_idx,
	.start_counters		= hisi_ddrc_pmu_start_counters,
	.stop_counters		= hisi_ddrc_pmu_stop_counters,
	.enable_counter		= hisi_ddrc_pmu_enable_counter,
	.disable_counter	= hisi_ddrc_pmu_disable_counter,
	.enable_counter_int	= hisi_ddrc_pmu_enable_counter_int,
	.disable_counter_int	= hisi_ddrc_pmu_disable_counter_int,
	.write_counter		= hisi_ddrc_pmu_write_counter,
	.read_counter		= hisi_ddrc_pmu_read_counter,
};

static int hisi_ddrc_pmu_dev_probe(struct platform_device *pdev,
				   struct hisi_pmu *ddrc_pmu)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = hisi_ddrc_pmu_init_data(pdev, ddrc_pmu);
	if (ret)
		return ret;

	/* Pick one core to use for cpumask attributes */
	cpumask_set_cpu(smp_processor_id(), &ddrc_pmu->cpus);

	ret = hisi_ddrc_pmu_init_irq(ddrc_pmu, pdev);
	if (ret)
		return ret;

	ddrc_pmu->name = devm_kasprintf(dev, GFP_KERNEL, "hisi_ddrc%u_%u",
					ddrc_pmu->ddrc_chn_id,
					ddrc_pmu->scl_id);
	ddrc_pmu->num_events = DDRC_NR_EVENTS;
	ddrc_pmu->num_counters = DDRC_NR_COUNTERS;
	ddrc_pmu->counter_bits = 32;
	ddrc_pmu->ops = &hisi_uncore_ddrc_ops;
	ddrc_pmu->dev = dev;

	return 0;
}

static int hisi_ddrc_pmu_probe(struct platform_device *pdev)
{
	struct hisi_pmu *ddrc_pmu;
	struct device *dev = &pdev->dev;
	int ret;

	ddrc_pmu = hisi_pmu_alloc(dev, DDRC_NR_COUNTERS);
	if (!ddrc_pmu)
		return -ENOMEM;

	ret = hisi_ddrc_pmu_dev_probe(pdev, ddrc_pmu);
	if (ret)
		return ret;

	ddrc_pmu->pmu = (struct pmu) {
		.name		= ddrc_pmu->name,
		.task_ctx_nr	= perf_invalid_context,
		.event_init	= hisi_uncore_pmu_event_init,
		.pmu_enable	= hisi_uncore_pmu_enable,
		.pmu_disable	= hisi_uncore_pmu_disable,
		.add		= hisi_uncore_pmu_add,
		.del		= hisi_uncore_pmu_del,
		.start		= hisi_uncore_pmu_start,
		.stop		= hisi_uncore_pmu_stop,
		.read		= hisi_uncore_pmu_read,
		.attr_groups	= hisi_ddrc_pmu_attr_groups,
	};

	ret = hisi_uncore_pmu_setup(ddrc_pmu, ddrc_pmu->name);
	if (ret) {
		dev_err(ddrc_pmu->dev, "hisi_uncore_pmu_setup failed!\n");
		return ret;
	}

	platform_set_drvdata(pdev, ddrc_pmu);

	return 0;
}

static int hisi_ddrc_pmu_remove(struct platform_device *pdev)
{
	struct hisi_pmu *ddrc_pmu = platform_get_drvdata(pdev);

	perf_pmu_unregister(&ddrc_pmu->pmu);

	return 0;
}

static struct platform_driver hisi_ddrc_pmu_driver = {
	.driver = {
		.name = "hisi_ddrc_pmu",
		.acpi_match_table = ACPI_PTR(hisi_ddrc_pmu_acpi_match),
	},
	.probe = hisi_ddrc_pmu_probe,
	.remove = hisi_ddrc_pmu_remove,
};
module_platform_driver(hisi_ddrc_pmu_driver);

MODULE_DESCRIPTION("HiSilicon SoC DDRC uncore PMU driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Shaokun Zhang, Anurup M");
