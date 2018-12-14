// SPDX-License-Identifier: GPL-2.0
/*
 * Provide performance monitoring for Hisilicon PCIe root ports
 *
 * Copyright (C) 2018 Hisilicon
 */

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/cpuhotplug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/perf_event.h>

#include "portdrv.h"

#define HISI_PP_TL_CNT_CTRL_REG 0x96C
#define   HISI_PP_TL_CNT_CTRL_TX_FLOW_CNT_EN BIT(0)
#define   HISI_PP_TL_CNT_CTRL_TX_ERR_CNT_EN BIT(1)
#define   HISI_PP_TL_CNT_CTRL_TX_NAT_CPL_CNT_EN BIT(2)
#define   HISI_PP_TL_CNT_CTRL_TX_FLOW_FUN_EN BIT(3)
#define   HISI_PP_TL_CNT_CTRL_TX_FLOW_CNT_TIME_MASK 0xFFF0

#define HISI_MAX_PERIOD(nr) (BIT_ULL(nr) - 1)

#define HISI_PP_TIMER_PERIOD_NS 10000000

enum hisi_pci_counts {
	TX_MEM_RD,
	TX_MEM_WR,
	TX_CFG_RD,
	TX_CFG_WR,
	TX_IO_RD,
	TX_IO_WR,
	TX_MSG,
	TX_CPL,
	TX_CCIX,
	TX_ATOMIC,
	TX_P2P,
	TX_TLP,
	TX_PAYLOAD,
	TX_DW,

	RX_TOTAL_TLP,
	RX_TOTAL_TR,
	RX_DROP,
	RX_POSTED,
	RX_NONPOSTED,
	RX_CPL,
	HISI_PP_EVENTS,
};

static struct hisi_pcie_cnt {
	u64 reg_offset;
	u8 bits;
} hisi_pcie_cnt_info[HISI_PP_EVENTS] = {
	[TX_MEM_RD]		= { 0x908, 16 },
	[TX_MEM_WR]		= { 0x90c, 16 },
	[TX_CFG_RD]		= { 0x910, 16 },
	[TX_CFG_WR]		= { 0x914, 16 },
	[TX_IO_RD]		= { 0x918, 16 },
	[TX_IO_WR]		= { 0x91C, 16 },
	[TX_MSG]		= { 0x924, 16 },
	[TX_CPL]		= { 0x930, 16 },
	[TX_CCIX]		= { 0x934, 16 },
	[TX_ATOMIC]		= { 0x938, 16 },
	[TX_P2P]		= { 0x93C, 16 },
	[TX_TLP]		= { 0x940, 32 },
	[TX_PAYLOAD]		= { 0x944, 32 },
	[TX_DW]			= { 0x948, 32 },

	[RX_TOTAL_TLP]		= { 0xb38, 16 },
	[RX_TOTAL_TR]		= { 0xb3c, 16 },
	[RX_DROP]		= { 0xb40, 16 },
	[RX_POSTED]		= { 0xb44, 16 },
	[RX_NONPOSTED]		= { 0xb48, 16 },
	[RX_CPL]		= { 0xb4c, 16 },
};

struct hisi_event_list {
	struct list_head list;
	struct perf_event *event;
};

struct hisi_pcie_pmu {
	struct pmu pmu;
	void __iomem *regs;
	cpumask_t associated_cpus;
	int on_cpu;
	struct hlist_node node;
	DECLARE_BITMAP(pmu_events, HISI_PP_EVENTS);
	struct hisi_event_list events[HISI_PP_EVENTS];
	struct hrtimer timer;
	struct list_head event_list;
};

#define to_hisi_pcie_pmu(p) container_of((p), struct hisi_pcie_pmu, pmu)

int hisi_pcie_pmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct hisi_pcie_pmu *dev_info = hlist_entry_safe(node,
							 struct hisi_pcie_pmu,
							 node);

	cpumask_set_cpu(cpu, &dev_info->associated_cpus);
	/* If another CPU is already managing this PMU, simply return. */
	if (dev_info->on_cpu != -1)
		return 0;

	/* Use this CPU in cpumask for event counting */
	dev_info->on_cpu = cpu;

	return 0;
}

int hisi_pcie_pmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct hisi_pcie_pmu *dev_info = hlist_entry_safe(node,
							 struct hisi_pcie_pmu,
							 node);
	cpumask_t pmu_online_cpus;
	unsigned int target;

	if (!cpumask_test_and_clear_cpu(cpu, &dev_info->associated_cpus))
		return 0;

	if (dev_info->on_cpu != cpu)
		return 0;

	/* Give up ownership of the PMU */
	dev_info->on_cpu = -1;

	/* Choose a new CPU to migrate ownership of the PMU to */
	cpumask_and(&pmu_online_cpus, &dev_info->associated_cpus,
		    cpu_online_mask);
	target = cpumask_any_but(&pmu_online_cpus, cpu);
	if (target >= nr_cpu_ids)
		return 0;

	/* Use this CPU for event counting */
	dev_info->on_cpu = target;
	perf_pmu_migrate_context(&dev_info->pmu, cpu, target);

	return 0;
}

void hisi_pcie_pmu_event_update(struct perf_event *event)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 delta, prev_raw_count, new_raw_count;
	u64 offset = hisi_pcie_cnt_info[hwc->config_base].reg_offset;
	u8 period = hisi_pcie_cnt_info[hwc->config_base].bits;

	do {
		new_raw_count = readl(dev_info->regs + offset);
		prev_raw_count = local64_read(&hwc->prev_count);
	} while (local64_cmpxchg(&hwc->prev_count, prev_raw_count,
				 new_raw_count) != prev_raw_count);

	delta = (new_raw_count - prev_raw_count) & HISI_MAX_PERIOD(period);
	local64_add(delta, &event->count);
}

static enum hrtimer_restart event_read(struct hrtimer *timer)
{
	struct hisi_pcie_pmu *dev_info =
		container_of(timer, struct hisi_pcie_pmu, timer);
	struct hisi_event_list *hevent;

	list_for_each_entry(hevent, &dev_info->event_list, list)
		hisi_pcie_pmu_event_update(hevent->event);
	hrtimer_forward_now(timer, HISI_PP_TIMER_PERIOD_NS);

	return HRTIMER_RESTART;
}

int hisi_pcie_pmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(event->pmu);

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	if (event->attr.exclude_user	||
	    event->attr.exclude_kernel	||
	    event->attr.exclude_host	||
	    event->attr.exclude_guest	||
	    event->attr.exclude_hv	||
	    event->attr.exclude_idle)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	hwc->idx = -1;
	hwc->config_base = event->attr.config;

	dev_info->events[hwc->config_base].event = event;

	return 0;
}

void hisi_pcie_pmu_enable(struct pmu *pmu)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(pmu);
	u32 val;

	if (bitmap_empty(dev_info->pmu_events, HISI_PP_EVENTS))
		return;

	val = readl(dev_info->regs + HISI_PP_TL_CNT_CTRL_REG);
	/* Disable time period based flow counting */
	val &= ~HISI_PP_TL_CNT_CTRL_TX_FLOW_CNT_TIME_MASK;
	val |= HISI_PP_TL_CNT_CTRL_TX_FLOW_CNT_EN |
		HISI_PP_TL_CNT_CTRL_TX_ERR_CNT_EN;
	writel(val, dev_info->regs + HISI_PP_TL_CNT_CTRL_REG);
}

void hisi_pcie_pmu_disable(struct pmu *pmu)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(pmu);
	u32 val;
	u32 new_raw_count;

	new_raw_count = readl(dev_info->regs + 0x908);

	val = readl(dev_info->regs + HISI_PP_TL_CNT_CTRL_REG);
	val &= ~(HISI_PP_TL_CNT_CTRL_TX_FLOW_CNT_EN |
		 HISI_PP_TL_CNT_CTRL_TX_ERR_CNT_EN);
	writel(val, dev_info->regs + HISI_PP_TL_CNT_CTRL_REG);
}

void hisi_pcie_pmu_enable_event(struct perf_event *event)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 offset = hisi_pcie_cnt_info[hwc->config_base].reg_offset;

	/* Register is write to clear */
	local64_set(&hwc->prev_count, 0);
	writel(0, dev_info->regs + offset);
}

void hisi_pcie_pmu_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (WARN_ON_ONCE(!(hwc->state & PERF_HES_STOPPED)))
		return;

	WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));
	hwc->state = 0;

	hisi_pcie_pmu_enable_event(event);
	perf_event_update_userpage(event);
}

void hisi_pcie_pmu_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);
	hwc->state |= PERF_HES_STOPPED;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	/* Read hardware counter and update the perf counter statistics */
	hisi_pcie_pmu_event_update(event);
	hwc->state |= PERF_HES_UPTODATE;
}

int hisi_pcie_pmu_add(struct perf_event *event, int flags)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	bool prev_empty;

	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	set_bit(hwc->config_base, dev_info->pmu_events);
	if (flags & PERF_EF_START)
		hisi_pcie_pmu_start(event, PERF_EF_RELOAD);
	prev_empty = list_empty(&dev_info->event_list);
	list_add(&dev_info->events[hwc->config_base].list,
		 &dev_info->event_list);
	if (prev_empty)
		hrtimer_start(&dev_info->timer, HISI_PP_TIMER_PERIOD_NS,
			      HRTIMER_MODE_REL);

	return 0;
}


void hisi_pcie_pmu_del(struct perf_event *event, int flags)
{
	struct hisi_pcie_pmu *dev_info = to_hisi_pcie_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	list_del(&dev_info->events[hwc->config_base].list);
	if (list_empty(&dev_info->event_list))
		hrtimer_cancel(&dev_info->timer);
	hisi_pcie_pmu_stop(event, PERF_EF_UPDATE);
	clear_bit(hwc->config_base, dev_info->pmu_events);
	perf_event_update_userpage(event);
}

ssize_t hisi_event_sysfs_show(struct device *dev,
			      struct device_attribute *attr, char *page)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);

	return sprintf(page, "config=0x%lx\n", (unsigned long)eattr->var);
}

void hisi_pcie_pmu_read(struct perf_event *event)
{
	hisi_pcie_pmu_event_update(event);
}

ssize_t hisi_cpumask_sysfs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct hisi_pcie_pmu *dev_info =
		container_of(pmu, struct hisi_pcie_pmu, pmu);

	return sprintf(buf, "%d\n", dev_info->on_cpu);
}

static DEVICE_ATTR(cpumask, 0444, hisi_cpumask_sysfs_show, NULL);

static struct attribute *hisi_pcie_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group hisi_pcie_pmu_cpumask_attr_group = {
	.attrs = hisi_pcie_pmu_cpumask_attrs,
};

#define HISI_PP_ATTR(_name, _func, _config)			\
	(&((struct dev_ext_attribute[]) {				\
		{ __ATTR(_name, 0444, _func, NULL), (void *)_config }   \
	})[0].attr.attr)

#define HISI_PP_EVENT_ATTR(_name, _config)		\
	HISI_PP_ATTR(_name, hisi_event_sysfs_show, (unsigned long)_config)

static struct attribute *hisi_pcie_pmu_events_attr[] = {
	HISI_PP_EVENT_ATTR(tx_mem_rd, TX_MEM_RD),
	HISI_PP_EVENT_ATTR(tx_mem_wr, TX_MEM_WR),
	HISI_PP_EVENT_ATTR(tx_cfg_rd, TX_CFG_RD),
	HISI_PP_EVENT_ATTR(tx_cfg_wr, TX_CFG_WR),
	HISI_PP_EVENT_ATTR(tx_io_rd, TX_IO_RD),
	HISI_PP_EVENT_ATTR(tx_io_wr, TX_IO_WR),
	HISI_PP_EVENT_ATTR(tx_msg, TX_MSG),

	HISI_PP_EVENT_ATTR(tx_tlp, TX_TLP),
	HISI_PP_EVENT_ATTR(tx_payload, TX_PAYLOAD),
	HISI_PP_EVENT_ATTR(tx_dw, TX_DW),

	HISI_PP_EVENT_ATTR(tx_cpl, TX_CPL),
	HISI_PP_EVENT_ATTR(tx_ccix_tlp, TX_CCIX),
	HISI_PP_EVENT_ATTR(tx_atomic_tlp, TX_ATOMIC),
	HISI_PP_EVENT_ATTR(tx_p2p_tlp, TX_P2P),

	HISI_PP_EVENT_ATTR(rx_tlp, RX_TOTAL_TLP),
	HISI_PP_EVENT_ATTR(rx_tr_tlp, RX_TOTAL_TR),
	HISI_PP_EVENT_ATTR(rx_posted_tlp, RX_POSTED),
	HISI_PP_EVENT_ATTR(rx_nonposted_tlp, RX_NONPOSTED),
	HISI_PP_EVENT_ATTR(rx_cpl_tlp, RX_CPL),
	NULL,
};

static struct attribute_group hisi_pcie_pmu_events_group = {
	.name = "events",
	.attrs = hisi_pcie_pmu_events_attr,
};

static const struct attribute_group *hisi_pcie_pmu_attr_groups[] = {
	&hisi_pcie_pmu_events_group,
	&hisi_pcie_pmu_cpumask_attr_group,
	NULL,
};

static int hisi_pcie_pmu_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	struct acpi_device *acpi_dev;
	struct resource_entry *entry;
	resource_size_t regs_start, regs_size;
	struct hisi_pcie_pmu *dev_info;
	struct list_head list;
	char *name;
	int ret;

	INIT_LIST_HEAD(&list);

	if (pdev->vendor != PCI_VENDOR_ID_HUAWEI)
		return -ENODEV;

	acpi_dev = ACPI_COMPANION(&pdev->dev);
	if (!acpi_dev)
		return -ENODEV;

	ret = acpi_dev_get_resources(acpi_dev, &list, NULL, NULL);
	if (ret < 0) {
		pr_err("Failed to get PMU resources, ret=%d\n", ret);
		return ret;
	}
	entry = list_first_entry(&list, struct resource_entry, node);
	if (entry) {
		regs_start = entry->res->start;
		regs_size = resource_size(entry->res);
		ret = 0;
	} else
		ret = -EINVAL;
	acpi_dev_free_resource_list(&list);
	if (ret)
		return ret;

	dev_info = devm_kzalloc(&pdev->dev, sizeof(*dev_info), GFP_KERNEL);
	if (!dev_info)
		return -ENOMEM;

	hrtimer_init(&dev_info->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_info->timer.function = event_read;

	INIT_LIST_HEAD(&dev_info->event_list);
	dev_info->regs = devm_ioremap_nocache(&pdev->dev, regs_start,
					      regs_size);
	if (!dev_info->regs)
		return -EINVAL;
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "hisi_pcie_port%04x_%02x",
			      pdev->bus->number,
			      PCI_SLOT(pdev->devfn));
	if (!name)
		return -ENOMEM;

	dev_info->pmu = (struct pmu) {
		.name = name,
		.task_ctx_nr = perf_invalid_context,
		.event_init = hisi_pcie_pmu_event_init,
		.pmu_enable = hisi_pcie_pmu_enable,
		.pmu_disable = hisi_pcie_pmu_disable,
		.add = hisi_pcie_pmu_add,
		.del = hisi_pcie_pmu_del,
		.start = hisi_pcie_pmu_start,
		.stop = hisi_pcie_pmu_stop,
		.read = hisi_pcie_pmu_read,
		.attr_groups = hisi_pcie_pmu_attr_groups,
		.capabilities = PERF_PMU_CAP_NO_INTERRUPT,
	};


	ret = cpuhp_state_add_instance(CPUHP_AP_PERF_ARM_HISI_PCIE_PMU_ONLINE,
				       &dev_info->node);
	if (ret) {
		dev_err(&pdev->dev, "Error %d registering hotplug;\n", ret);
		return ret;
	}

	ret = perf_pmu_register(&dev_info->pmu, name, -1);
	if (ret) {
		cpuhp_state_remove_instance(CPUHP_AP_PERF_ARM_HISI_PCIE_PMU_ONLINE,
					    &dev_info->node);

		return ret;
	}

	dev_set_drvdata(&pdev->dev, dev_info);

	return 0;
}

static void hisi_pcie_pmu_remove(struct pcie_device *dev)
{
	struct hisi_pcie_pmu *dev_info = dev_get_drvdata(&dev->port->dev);

	perf_pmu_unregister(&dev_info->pmu);
	cpuhp_state_remove_instance(CPUHP_AP_PERF_ARM_HISI_PCIE_PMU_ONLINE,
				    &dev_info->node);
}

static struct pcie_port_service_driver hisi_pcie_pmu = {
	.name = "hisi_pcie_root_port_pmu",
	.port_type = PCIE_ANY_PORT,
	.service = PCIE_PORT_SERVICE_PMU,
	.probe = hisi_pcie_pmu_probe,
	.remove = hisi_pcie_pmu_remove,
};

static int __init hisi_pcie_service_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_PERF_ARM_HISI_PCIE_PMU_ONLINE,
				      "AP_PERF_ARM_HISI_PCIE_ONLINE",
				      hisi_pcie_pmu_online_cpu,
				      hisi_pcie_pmu_offline_cpu);
	if (ret) {
		pr_err("PCIE PMU: setup hotplug, ret = %d\n", ret);
		return ret;
	}

	pcie_port_service_register(&hisi_pcie_pmu);

	return 0;
}

static void hisi_pcie_service_remove(void)
{
	pcie_port_service_unregister(&hisi_pcie_pmu);
	cpuhp_remove_multi_state(CPUHP_AP_PERF_ARM_HISI_PCIE_PMU_ONLINE);
}
module_init(hisi_pcie_service_init);
module_exit(hisi_pcie_service_remove);
MODULE_LICENSE("GPL");
