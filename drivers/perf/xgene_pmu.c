/*
 * APM X-Gene SoC PMU (Performance Monitor Unit)
 *
 * Copyright (c) 2016, Applied Micro Circuits Corporation
 * Author: Hoan Tran <hotran@apm.com>
 *         Tai Nguyen <ttnguyen@apm.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
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
#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/efi.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define CSW_CSWCR                       0x0000
#define  CSW_CSWCR_DUALMCB_MASK         BIT(0)
#define MCBADDRMR                       0x0000
#define  MCBADDRMR_DUALMCU_MODE_MASK    BIT(2)

#define PCPPMU_INTSTATUS_REG	0x000
#define PCPPMU_INTMASK_REG	0x004
#define  PCPPMU_INTMASK		0x0000000F
#define  PCPPMU_INTENMASK	0xFFFFFFFF
#define  PCPPMU_INTCLRMASK	0xFFFFFFF0
#define  PCPPMU_INT_MCU		BIT(0)
#define  PCPPMU_INT_MCB		BIT(1)
#define  PCPPMU_INT_L3C		BIT(2)
#define  PCPPMU_INT_IOB		BIT(3)

#define PMU_MAX_COUNTERS	4
#define PMU_CNT_MAX_VAL		0x100000000ULL
#define PMU_OVERFLOW_MASK	0xF
#define PMU_PMCR_E		BIT(0)
#define PMU_PMCR_P		BIT(1)

#define PMU_PMEVCNTR0		0x000
#define PMU_PMEVCNTR1		0x004
#define PMU_PMEVCNTR2		0x008
#define PMU_PMEVCNTR3		0x00C
#define PMU_PMEVTYPER0		0x400
#define PMU_PMEVTYPER1		0x404
#define PMU_PMEVTYPER2		0x408
#define PMU_PMEVTYPER3		0x40C
#define PMU_PMAMR0		0xA00
#define PMU_PMAMR1		0xA04
#define PMU_PMCNTENSET		0xC00
#define PMU_PMCNTENCLR		0xC20
#define PMU_PMINTENSET		0xC40
#define PMU_PMINTENCLR		0xC60
#define PMU_PMOVSR		0xC80
#define PMU_PMCR		0xE04

#define to_pmu_dev(p)     container_of(p, struct xgene_pmu_dev, pmu)
#define _GET_CNTR(ev)     (ev->hw.extra_reg.reg)
#define _GET_EVENTID(ev)  (ev->hw.config & 0xFFULL)
#define _GET_AGENTID(ev)  (ev->hw.extra_reg.config & 0xFFFFFFFFULL)
#define _GET_AGENT1ID(ev) ((ev->hw.extra_reg.config >> 32) & 0xFFFFFFFFULL)

struct hw_pmu_info {
	u32 type;
	u32 enable_mask;
	void __iomem *csr;
};

struct xgene_pmu_dev {
	struct hw_pmu_info *inf;
	struct xgene_pmu *parent;
	struct pmu pmu;
	u8 max_counters;
	DECLARE_BITMAP(cntr_assign_mask, PMU_MAX_COUNTERS);
	raw_spinlock_t lock;
	u64 max_period;
	const struct attribute_group **attr_groups;
	struct perf_event *pmu_counter_event[4];
};

struct xgene_pmu {
	struct device *dev;
	int version;
	void __iomem *pcppmu_csr;
	u32 mcb_active_mask;
	u32 mc_active_mask;
	cpumask_t cpu;
	struct list_head l3cpmus;
	struct list_head iobpmus;
	struct list_head mcbpmus;
	struct list_head mcpmus;
};

struct xgene_pmu_dev_ctx {
	char *name;
	struct list_head next;
	struct xgene_pmu_dev *pmu_dev;
	struct hw_pmu_info inf;
};

struct xgene_pmu_event {
	struct device_attribute attr;
	u32 config;
};

struct xgene_pmu_data {
	int id;
	u32 data;
};

enum xgene_pmu_version {
	PCP_PMU_V1 = 1,
	PCP_PMU_V2,
};

enum xgene_pmu_dev_type {
	PMU_TYPE_L3C = 0,
	PMU_TYPE_IOB,
	PMU_TYPE_MCB,
	PMU_TYPE_MC,
};

/*
 * sysfs format attributes
 */
static ssize_t xgene_pmu_format_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sprintf(buf, "%s\n", (char *) eattr->var);
}

#define XGENE_PMU_FORMAT_ATTR(_name, _config) \
	struct dev_ext_attribute pmu_format_attr_##_name = \
		{ __ATTR(_name, S_IRUGO, xgene_pmu_format_show, NULL), _config }

static XGENE_PMU_FORMAT_ATTR(l3c_eventid, "config:0-7");
static XGENE_PMU_FORMAT_ATTR(l3c_agentid, "config1:0-9");
static XGENE_PMU_FORMAT_ATTR(iob_eventid, "config:0-7");
static XGENE_PMU_FORMAT_ATTR(iob_agentid, "config1:0-63");
static XGENE_PMU_FORMAT_ATTR(mcb_eventid, "config:0-5");
static XGENE_PMU_FORMAT_ATTR(mcb_agentid, "config1:0-9");
static XGENE_PMU_FORMAT_ATTR(mc_eventid, "config:0-28");

static const struct attribute *l3c_pmu_format_attrs[] = {
	&pmu_format_attr_l3c_eventid.attr.attr,
	&pmu_format_attr_l3c_agentid.attr.attr,
	NULL,
};

static const struct attribute *iob_pmu_format_attrs[] = {
	&pmu_format_attr_iob_eventid.attr.attr,
	&pmu_format_attr_iob_agentid.attr.attr,
	NULL,
};

static const struct attribute *mcb_pmu_format_attrs[] = {
	&pmu_format_attr_mcb_eventid.attr.attr,
	&pmu_format_attr_mcb_agentid.attr.attr,
	NULL,
};

static const struct attribute *mc_pmu_format_attrs[] = {
	&pmu_format_attr_mc_eventid.attr.attr,
	NULL,
};

static const struct attribute_group l3c_pmu_format_attr_group = {
	.name = "format",
	.attrs = (struct attribute **) l3c_pmu_format_attrs,
};

static const struct attribute_group iob_pmu_format_attr_group = {
	.name = "format",
	.attrs = (struct attribute **) iob_pmu_format_attrs,
};

static const struct attribute_group mcb_pmu_format_attr_group = {
	.name = "format",
	.attrs = (struct attribute **) mcb_pmu_format_attrs,
};

static const struct attribute_group mc_pmu_format_attr_group = {
	.name = "format",
	.attrs = (struct attribute **) mc_pmu_format_attrs,
};

/*
 * sysfs event attributes
 */
static ssize_t xgene_pmu_event_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xgene_pmu_event *event = container_of(attr,
			struct xgene_pmu_event, attr);

	return sprintf(buf, "config=0x%x\n", event->config);
}

#define XGENE_PMU_EVENT_ATTR(_name) \
		__ATTR(_name, S_IRUGO, xgene_pmu_event_show, NULL)
#define XGENE_PMU_EVENT(_name, _config) { \
		.attr = XGENE_PMU_EVENT_ATTR(_name), \
		.config = _config, }

static const struct xgene_pmu_event l3c_pmu_events[] = {
	XGENE_PMU_EVENT(cycle-count,			0x00),
	XGENE_PMU_EVENT(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT(read-hit,			0x02),
	XGENE_PMU_EVENT(read-miss,			0x03),
	XGENE_PMU_EVENT(write-need-replacement,		0x06),
	XGENE_PMU_EVENT(write-not-need-replacement,	0x07),
	XGENE_PMU_EVENT(tq-full,			0x08),
	XGENE_PMU_EVENT(ackq-full,			0x09),
	XGENE_PMU_EVENT(wdb-full,			0x0a),
	XGENE_PMU_EVENT(bank-fifo-full,			0x0b),
	XGENE_PMU_EVENT(odb-full,			0x0c),
	XGENE_PMU_EVENT(wbq-full,			0x0d),
	XGENE_PMU_EVENT(bank-conflict-fifo-issue,	0x0e),
	XGENE_PMU_EVENT(bank-fifo-issue,		0x0f),
};

static const struct xgene_pmu_event iob_pmu_events[] = {
	XGENE_PMU_EVENT(cycle-count,			0x00),
	XGENE_PMU_EVENT(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT(axi0-read,			0x02),
	XGENE_PMU_EVENT(axi0-read-partial,		0x03),
	XGENE_PMU_EVENT(axi1-read,			0x04),
	XGENE_PMU_EVENT(axi1-read-partial,		0x05),
	XGENE_PMU_EVENT(csw-read-block,			0x06),
	XGENE_PMU_EVENT(csw-read-partial,		0x07),
	XGENE_PMU_EVENT(axi0-write,			0x10),
	XGENE_PMU_EVENT(axi0-write-partial,		0x11),
	XGENE_PMU_EVENT(axi1-write,			0x13),
	XGENE_PMU_EVENT(axi1-write-partial,		0x14),
	XGENE_PMU_EVENT(csw-inbound-dirty,		0x16),
};

static const struct xgene_pmu_event mcb_pmu_events[] = {
	XGENE_PMU_EVENT(cycle-count,			0x00),
	XGENE_PMU_EVENT(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT(csw-read,			0x02),
	XGENE_PMU_EVENT(csw-write-request,		0x03),
	XGENE_PMU_EVENT(mcb-csw-stall,			0x04),
	XGENE_PMU_EVENT(cancel-read-gack,		0x05),
};

static const struct xgene_pmu_event mc_pmu_events[] = {
	XGENE_PMU_EVENT(cycle-count,				0x00),
	XGENE_PMU_EVENT(cycle-count-div-64,			0x01),
	XGENE_PMU_EVENT(act-cmd-sent,				0x02),
	XGENE_PMU_EVENT(pre-cmd-sent,				0x03),
	XGENE_PMU_EVENT(rd-cmd-sent,				0x04),
	XGENE_PMU_EVENT(rda-cmd-sent,				0x05),
	XGENE_PMU_EVENT(wr-cmd-sent,				0x06),
	XGENE_PMU_EVENT(wra-cmd-sent,				0x07),
	XGENE_PMU_EVENT(pde-cmd-sent,				0x08),
	XGENE_PMU_EVENT(sre-cmd-sent,				0x09),
	XGENE_PMU_EVENT(prea-cmd-sent,				0x0a),
	XGENE_PMU_EVENT(ref-cmd-sent,				0x0b),
	XGENE_PMU_EVENT(rd-rda-cmd-sent,			0x0c),
	XGENE_PMU_EVENT(wr-wra-cmd-sent,			0x0d),
	XGENE_PMU_EVENT(in-rd-collision,			0x0e),
	XGENE_PMU_EVENT(in-wr-collision,			0x0f),
	XGENE_PMU_EVENT(collision-queue-not-empty,		0x10),
	XGENE_PMU_EVENT(collision-queue-full,			0x11),
	XGENE_PMU_EVENT(mcu-request,				0x12),
	XGENE_PMU_EVENT(mcu-rd-request,				0x13),
	XGENE_PMU_EVENT(mcu-hp-rd-request,			0x14),
	XGENE_PMU_EVENT(mcu-wr-request,				0x15),
	XGENE_PMU_EVENT(mcu-rd-proceed-all,			0x16),
	XGENE_PMU_EVENT(mcu-rd-proceed-cancel,			0x17),
	XGENE_PMU_EVENT(mcu-rd-response,			0x18),
	XGENE_PMU_EVENT(mcu-rd-proceed-speculative-all,		0x19),
	XGENE_PMU_EVENT(mcu-rd-proceed-speculative-cancel,	0x1a),
	XGENE_PMU_EVENT(mcu-wr-proceed-all,			0x1b),
	XGENE_PMU_EVENT(mcu-wr-proceed-cancel,			0x1c),
};

/* Populated in xgene_pmu_probe */
static struct attribute *l3c_pmu_events_attrs[ARRAY_SIZE(l3c_pmu_events) + 1];
static struct attribute *iob_pmu_events_attrs[ARRAY_SIZE(iob_pmu_events) + 1];
static struct attribute *mcb_pmu_events_attrs[ARRAY_SIZE(mcb_pmu_events) + 1];
static struct attribute *mc_pmu_events_attrs[ARRAY_SIZE(mc_pmu_events) + 1];

static const struct attribute_group l3c_pmu_events_attr_group = {
	.name = "events",
	.attrs = l3c_pmu_events_attrs,
};

static const struct attribute_group iob_pmu_events_attr_group = {
	.name = "events",
	.attrs = iob_pmu_events_attrs,
};

static const struct attribute_group mcb_pmu_events_attr_group = {
	.name = "events",
	.attrs = mcb_pmu_events_attrs,
};

static const struct attribute_group mc_pmu_events_attr_group = {
	.name = "events",
	.attrs = mc_pmu_events_attrs,
};

/*
 * sysfs cpumask attributes
 */
static ssize_t xgene_pmu_cpumask_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &pmu_dev->parent->cpu);
}

static DEVICE_ATTR(cpumask, S_IRUGO, xgene_pmu_cpumask_show, NULL);

static const struct attribute *xgene_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group pmu_cpumask_attr_group = {
	.attrs = (struct attribute **) xgene_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups
 */
static const struct attribute_group *l3c_pmu_attr_groups[] = {
	&l3c_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&l3c_pmu_events_attr_group,
	NULL
};

static const struct attribute_group *iob_pmu_attr_groups[] = {
	&iob_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&iob_pmu_events_attr_group,
	NULL
};

static const struct attribute_group *mcb_pmu_attr_groups[] = {
	&mcb_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&mcb_pmu_events_attr_group,
	NULL
};

static const struct attribute_group *mc_pmu_attr_groups[] = {
	&mc_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&mc_pmu_events_attr_group,
	NULL
};

static int get_next_avail_cntr(struct xgene_pmu_dev *pmu_dev)
{
	int cntr;

	cntr = find_first_zero_bit(pmu_dev->cntr_assign_mask,
				pmu_dev->max_counters);
	if (cntr == pmu_dev->max_counters)
		return -ENOSPC;
	set_bit(cntr, pmu_dev->cntr_assign_mask);

	return cntr;
}

static void clear_avail_cntr(struct xgene_pmu_dev *pmu_dev, int cntr)
{
	clear_bit(cntr, pmu_dev->cntr_assign_mask);
}

static inline void xgene_pmu_mask_int(struct xgene_pmu *xgene_pmu)
{
	writel(PCPPMU_INTENMASK, xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
}

static inline void xgene_pmu_unmask_int(struct xgene_pmu *xgene_pmu)
{
	writel(PCPPMU_INTCLRMASK, xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
}

static inline u32 xgene_pmu_read_counter(struct xgene_pmu_dev *pmu_dev, int idx)
{
	return readl(pmu_dev->inf->csr + PMU_PMEVCNTR0 + (4 * idx));
}

static inline void
xgene_pmu_write_counter(struct xgene_pmu_dev *pmu_dev, int idx, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMEVCNTR0 + (4 * idx));
}

static inline void
xgene_pmu_write_evttype(struct xgene_pmu_dev *pmu_dev, int idx, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMEVTYPER0 + (4 * idx));
}

static inline void
xgene_pmu_write_agenttype(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAMR0);
}

static inline void
xgene_pmu_write_agent1type(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAMR1);
}

static inline void
xgene_pmu_enable_counter(struct xgene_pmu_dev *pmu_dev, int idx)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMCNTENSET);
	val |= 1 << idx;
	writel(val, pmu_dev->inf->csr + PMU_PMCNTENSET);
}

static inline void
xgene_pmu_disable_counter(struct xgene_pmu_dev *pmu_dev, int idx)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMCNTENCLR);
	val |= 1 << idx;
	writel(val, pmu_dev->inf->csr + PMU_PMCNTENCLR);
}

static inline void
xgene_pmu_enable_counter_int(struct xgene_pmu_dev *pmu_dev, int idx)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMINTENSET);
	val |= 1 << idx;
	writel(val, pmu_dev->inf->csr + PMU_PMINTENSET);
}

static inline void
xgene_pmu_disable_counter_int(struct xgene_pmu_dev *pmu_dev, int idx)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMINTENCLR);
	val |= 1 << idx;
	writel(val, pmu_dev->inf->csr + PMU_PMINTENCLR);
}

static inline void xgene_pmu_reset_counters(struct xgene_pmu_dev *pmu_dev)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMCR);
	val |= PMU_PMCR_P;
	writel(val, pmu_dev->inf->csr + PMU_PMCR);
}

static inline void xgene_pmu_start_counters(struct xgene_pmu_dev *pmu_dev)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMCR);
	val |= PMU_PMCR_E;
	writel(val, pmu_dev->inf->csr + PMU_PMCR);
}

static inline void xgene_pmu_stop_counters(struct xgene_pmu_dev *pmu_dev)
{
	u32 val;

	val = readl(pmu_dev->inf->csr + PMU_PMCR);
	val &= ~PMU_PMCR_E;
	writel(val, pmu_dev->inf->csr + PMU_PMCR);
}

static int xgene_perf_event_init(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 config, config1;

	/* Test the event attr type check for PMU enumeration */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * SOC PMU counters are shared across all cores.
	 * Therefore, it does not support per-process mode.
	 * Also, it does not support event sampling mode.
	 */
	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EINVAL;

	/* SOC counters do not have usr/os/guest/host bits */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_host || event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;
	/*
	 * Many perf core operations (eg. events rotation) operate on a
	 * single CPU context. This is obvious for CPU PMUs, where one
	 * expects the same sets of events being observed on all CPUs,
	 * but can lead to issues for off-core PMUs, where each
	 * event could be theoretically assigned to a different CPU. To
	 * mitigate this, we enforce CPU assignment to one, selected
	 * processor (the one described in the "cpumask" attribute).
	 */
	event->cpu = cpumask_first(&pmu_dev->parent->cpu);

	config = event->attr.config;
	config1 = event->attr.config1;

	hwc->config = config;
	/*
	 * Each bit of the config1 field represents an agent from which the
	 * request of the event come. The event is counted only if it's caused
	 * by a request of an agent has the bit set.
	 * By default, the event is counted for all agents.
	 */
	if (config1)
		hwc->extra_reg.config = config1;
	else
		hwc->extra_reg.config = 0xFFFFFFFFFFFFFFFFULL;

	return 0;
}

static void xgene_perf_enable_event(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_write_evttype(pmu_dev, _GET_CNTR(event), _GET_EVENTID(event));
	xgene_pmu_write_agenttype(pmu_dev, _GET_AGENTID(event));
	if (pmu_dev->inf->type == PMU_TYPE_IOB)
		xgene_pmu_write_agent1type(pmu_dev, _GET_AGENT1ID(event));

	xgene_pmu_start_counters(pmu_dev);
	xgene_pmu_enable_counter(pmu_dev, _GET_CNTR(event));
	xgene_pmu_enable_counter_int(pmu_dev, _GET_CNTR(event));
}

static void xgene_perf_disable_event(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_disable_counter(pmu_dev, _GET_CNTR(event));
	xgene_pmu_disable_counter_int(pmu_dev, _GET_CNTR(event));
}

static void xgene_perf_start(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (WARN_ON_ONCE(!(hwc->state & PERF_HES_STOPPED)))
		return;

	WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));
	hwc->state = 0;

	if (flags & PERF_EF_RELOAD) {
		u64 prev_raw_count =  local64_read(&hwc->prev_count);

		xgene_pmu_write_counter(pmu_dev, _GET_CNTR(event),
					(u32) prev_raw_count);
	}

	xgene_perf_enable_event(event);
	perf_event_update_userpage(event);
}

static void xgene_perf_read(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 prev_raw_count;
	u64 count;
	u64 delta;

	count = xgene_pmu_read_counter(pmu_dev, _GET_CNTR(event))
		& pmu_dev->max_period;

	prev_raw_count =  local64_read(&hwc->prev_count);
	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count, count) !=
		prev_raw_count)
		return;

	delta = (count - prev_raw_count) & pmu_dev->max_period;

	local64_add(delta, &event->count);
}

static void xgene_perf_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 config;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	xgene_perf_disable_event(event);
	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);
	hwc->state |= PERF_HES_STOPPED;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	config = hwc->config;
	xgene_perf_read(event);
	hwc->state |= PERF_HES_UPTODATE;
}

static int xgene_perf_add(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;

	event->hw.state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	/* Allocate an event counter */
	hw->idx = get_next_avail_cntr(pmu_dev);
	if (hw->idx < 0)
		return -EAGAIN;

	event->hw.extra_reg.reg = (u16) hw->idx;

	if (flags & PERF_EF_START)
		xgene_perf_start(event, PERF_EF_RELOAD);

	/* Update counter event pointer for Interrupt handler */
	pmu_dev->pmu_counter_event[hw->idx] = event;

	return 0;
}

static void xgene_perf_del(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_perf_stop(event, PERF_EF_UPDATE);

	/* clear the assigned counter */
	clear_avail_cntr(pmu_dev, _GET_CNTR(event));

	perf_event_update_userpage(event);
}

static u64 xgene_perf_event_update(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 delta, prev_raw_count, new_raw_count;

again:
	prev_raw_count = local64_read(&hwc->prev_count);
	new_raw_count = pmu_dev->max_period;

	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count,
			    new_raw_count) != prev_raw_count)
		goto again;

	delta = (new_raw_count - prev_raw_count) & pmu_dev->max_period;

	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return new_raw_count;
}

static int xgene_perf_event_set_period(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	s64 left = local64_read(&hwc->period_left);
	s64 period = hwc->sample_period;
	int ret = 0;

	if (unlikely(left <= -period)) {
		left = period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	if (unlikely(left <= 0)) {
		left += period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	/*
	 * Limit the maximum period to prevent the counter value
	 * from overtaking the one we are about to program. In
	 * effect we are reducing max_period to account for
	 * interrupt latency (and we are being very conservative).
	 */
	if (left > (pmu_dev->max_period >> 1))
		left = pmu_dev->max_period >> 1;

	local64_set(&hwc->prev_count, (u64) -left);

	xgene_pmu_write_counter(pmu_dev, hwc->idx, (u64)(-left) & 0xffffffff);

	perf_event_update_userpage(event);

	return ret;
}

static int xgene_init_perf(struct xgene_pmu_dev *pmu_dev, char *name)
{
	struct xgene_pmu *xgene_pmu;

	raw_spin_lock_init(&pmu_dev->lock);

	pmu_dev->max_period = PMU_CNT_MAX_VAL - 1;
	/* First PMU version supports only single event counter */
	xgene_pmu = pmu_dev->parent;
	if (xgene_pmu->version == PCP_PMU_V1)
		pmu_dev->max_counters = 1;
	else
		pmu_dev->max_counters = PMU_MAX_COUNTERS;

	/* Perf driver registration */
	pmu_dev->pmu = (struct pmu) {
		.attr_groups	= pmu_dev->attr_groups,
		.task_ctx_nr	= perf_invalid_context,
		.event_init	= xgene_perf_event_init,
		.add		= xgene_perf_add,
		.del		= xgene_perf_del,
		.start		= xgene_perf_start,
		.stop		= xgene_perf_stop,
		.read		= xgene_perf_read,
	};

	/* Hardware counter init */
	xgene_pmu_stop_counters(pmu_dev);
	xgene_pmu_reset_counters(pmu_dev);

	return perf_pmu_register(&pmu_dev->pmu, name, -1);
}

static int
xgene_pmu_dev_add(struct xgene_pmu *xgene_pmu, struct xgene_pmu_dev_ctx *ctx)
{
	struct device *dev = xgene_pmu->dev;
	struct xgene_pmu_dev *pmu;
	int rc;

	pmu = devm_kzalloc(dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;
	pmu->parent = xgene_pmu;
	pmu->inf = &ctx->inf;
	ctx->pmu_dev = pmu;

	switch (pmu->inf->type) {
	case PMU_TYPE_L3C:
		pmu->attr_groups = l3c_pmu_attr_groups;
		break;
	case PMU_TYPE_IOB:
		pmu->attr_groups = iob_pmu_attr_groups;
		break;
	case PMU_TYPE_MCB:
		if (!(xgene_pmu->mcb_active_mask & pmu->inf->enable_mask))
			goto dev_err;
		pmu->attr_groups = mcb_pmu_attr_groups;
		break;
	case PMU_TYPE_MC:
		if (!(xgene_pmu->mc_active_mask & pmu->inf->enable_mask))
			goto dev_err;
		pmu->attr_groups = mc_pmu_attr_groups;
		break;
	default:
		return -EINVAL;
	}

	rc = xgene_init_perf(pmu, ctx->name);
	if (rc) {
		dev_err(dev, "%s PMU: Failed to init perf driver\n", ctx->name);
		goto dev_err;
	}

	dev_info(dev, "%s PMU registered\n", ctx->name);

	return rc;

dev_err:
	devm_kfree(dev, pmu);
	return -ENODEV;
}

static irqreturn_t _xgene_pmu_isr(int irq, struct xgene_pmu_dev *pmu_dev)
{
	struct xgene_pmu *xgene_pmu = pmu_dev->parent;
	u32 pmovsr;
	int idx;

	pmovsr = readl(pmu_dev->inf->csr + PMU_PMOVSR) & PMU_OVERFLOW_MASK;
	/* Clear interrupt flag */
	if (xgene_pmu->version == PCP_PMU_V1)
		writel(0x0, pmu_dev->inf->csr + PMU_PMOVSR);
	else
		writel(pmovsr, pmu_dev->inf->csr + PMU_PMOVSR);

	if (!pmovsr)
		return IRQ_NONE;

	for (idx = 0; idx < PMU_MAX_COUNTERS; idx++) {
		struct perf_event *event = pmu_dev->pmu_counter_event[idx];
		int overflowed = pmovsr & BIT(idx);

		/* Ignore if we don't have an event. */
		if (!event || !overflowed)
			continue;
		xgene_perf_event_update(event);
		xgene_perf_event_set_period(event);
	}

	return IRQ_HANDLED;
}

static irqreturn_t xgene_pmu_isr(int irq, void *dev_id)
{
	struct xgene_pmu_dev_ctx *ctx, *temp_ctx;
	struct xgene_pmu *xgene_pmu = dev_id;
	u32 val;

	xgene_pmu_mask_int(xgene_pmu);

	/* Get Interrupt PMU source */
	val = readl(xgene_pmu->pcppmu_csr + PCPPMU_INTSTATUS_REG)
	      & PCPPMU_INTMASK;
	if (val & PCPPMU_INT_MCU) {
		list_for_each_entry_safe(ctx, temp_ctx,
				&xgene_pmu->mcpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & PCPPMU_INT_MCB) {
		list_for_each_entry_safe(ctx, temp_ctx,
				&xgene_pmu->mcbpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & PCPPMU_INT_L3C) {
		list_for_each_entry_safe(ctx, temp_ctx,
				&xgene_pmu->l3cpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & PCPPMU_INT_IOB) {
		list_for_each_entry_safe(ctx, temp_ctx,
				&xgene_pmu->iobpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}

	xgene_pmu_unmask_int(xgene_pmu);

	return IRQ_HANDLED;
}

static int acpi_pmu_probe_active_mcb_mcu(struct xgene_pmu *xgene_pmu,
					 struct platform_device *pdev)
{
	void __iomem *csw_csr, *mcba_csr, *mcbb_csr;
	struct resource *res;
	unsigned int reg;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	csw_csr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(csw_csr)) {
		dev_err(&pdev->dev, "ioremap failed for CSW CSR resource\n");
		return PTR_ERR(csw_csr);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	mcba_csr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mcba_csr)) {
		dev_err(&pdev->dev, "ioremap failed for MCBA CSR resource\n");
		return PTR_ERR(mcba_csr);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	mcbb_csr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mcbb_csr)) {
		dev_err(&pdev->dev, "ioremap failed for MCBB CSR resource\n");
		return PTR_ERR(mcbb_csr);
	}

	reg = readl(csw_csr + CSW_CSWCR);
	if (reg & CSW_CSWCR_DUALMCB_MASK) {
		/* Dual MCB active */
		xgene_pmu->mcb_active_mask = 0x3;
		/* Probe all active MC(s) */
		reg = readl(mcbb_csr + CSW_CSWCR);
		xgene_pmu->mc_active_mask =
			(reg & MCBADDRMR_DUALMCU_MODE_MASK) ? 0xF : 0x5;
	} else {
		/* Single MCB active */
		xgene_pmu->mcb_active_mask = 0x1;
		/* Probe all active MC(s) */
		reg = readl(mcba_csr + CSW_CSWCR);
		xgene_pmu->mc_active_mask =
			(reg & MCBADDRMR_DUALMCU_MODE_MASK) ? 0x3 : 0x1;
	}

	return 0;
}

static int fdt_pmu_probe_active_mcb_mcu(struct xgene_pmu *xgene_pmu,
					struct platform_device *pdev)
{
	struct regmap *csw_map, *mcba_map, *mcbb_map;
	struct device_node *np = pdev->dev.of_node;
	unsigned int reg;

	csw_map = syscon_regmap_lookup_by_phandle(np, "regmap-csw");
	if (IS_ERR(csw_map)) {
		dev_err(&pdev->dev, "unable to get syscon regmap csw\n");
		return PTR_ERR(csw_map);
	}

	mcba_map = syscon_regmap_lookup_by_phandle(np, "regmap-mcba");
	if (IS_ERR(mcba_map)) {
		dev_err(&pdev->dev, "unable to get syscon regmap mcba\n");
		return PTR_ERR(mcba_map);
	}

	mcbb_map = syscon_regmap_lookup_by_phandle(np, "regmap-mcbb");
	if (IS_ERR(mcbb_map)) {
		dev_err(&pdev->dev, "unable to get syscon regmap mcbb\n");
		return PTR_ERR(mcbb_map);
	}

	if (regmap_read(csw_map, CSW_CSWCR, &reg))
		return -EINVAL;

	if (reg & CSW_CSWCR_DUALMCB_MASK) {
		/* Dual MCB active */
		xgene_pmu->mcb_active_mask = 0x3;
		/* Probe all active MC(s) */
		if (regmap_read(mcbb_map, MCBADDRMR, &reg))
			return 0;
		xgene_pmu->mc_active_mask =
			(reg & MCBADDRMR_DUALMCU_MODE_MASK) ? 0xF : 0x5;
	} else {
		/* Single MCB active */
		xgene_pmu->mcb_active_mask = 0x1;
		/* Probe all active MC(s) */
		if (regmap_read(mcba_map, MCBADDRMR, &reg))
			return 0;
		xgene_pmu->mc_active_mask =
			(reg & MCBADDRMR_DUALMCU_MODE_MASK) ? 0x3 : 0x1;
	}

	return 0;
}

static int xgene_pmu_probe_active_mcb_mcu(struct xgene_pmu *xgene_pmu,
					  struct platform_device *pdev)
{
	if (has_acpi_companion(&pdev->dev))
		return acpi_pmu_probe_active_mcb_mcu(xgene_pmu, pdev);
	return fdt_pmu_probe_active_mcb_mcu(xgene_pmu, pdev);
}

static char *xgene_pmu_dev_name(u32 type, int id)
{
	switch (type) {
	case PMU_TYPE_L3C:
		return kasprintf(GFP_KERNEL, "l3c%d", id);
	case PMU_TYPE_IOB:
		return kasprintf(GFP_KERNEL, "iob%d", id);
	case PMU_TYPE_MCB:
		return kasprintf(GFP_KERNEL, "mcb%d", id);
	case PMU_TYPE_MC:
		return kasprintf(GFP_KERNEL, "mc%d", id);
	default:
		return kasprintf(GFP_KERNEL, "unknown");
	}
}

#if defined(CONFIG_ACPI)
static int acpi_pmu_dev_add_resource(struct acpi_resource *ares, void *data)
{
	struct resource *res = data;

	if (ares->type == ACPI_RESOURCE_TYPE_FIXED_MEMORY32)
		acpi_dev_resource_memory(ares, res);

	/* Always tell the ACPI core to skip this resource */
	return 1;
}

static struct
xgene_pmu_dev_ctx *acpi_get_pmu_hw_inf(struct xgene_pmu *xgene_pmu,
				       struct acpi_device *adev, u32 type)
{
	struct device *dev = xgene_pmu->dev;
	struct list_head resource_list;
	struct xgene_pmu_dev_ctx *ctx;
	const union acpi_object *obj;
	struct hw_pmu_info *inf;
	void __iomem *dev_csr;
	struct resource res;
	int enable_bit;
	int rc;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	INIT_LIST_HEAD(&resource_list);
	rc = acpi_dev_get_resources(adev, &resource_list,
				    acpi_pmu_dev_add_resource, &res);
	acpi_dev_free_resource_list(&resource_list);
	if (rc < 0 || IS_ERR(&res)) {
		dev_err(dev, "PMU type %d: No resource address found\n", type);
		goto err;
	}

	dev_csr = devm_ioremap_resource(dev, &res);
	if (IS_ERR(dev_csr)) {
		dev_err(dev, "PMU type %d: Fail to map resource\n", type);
		rc = PTR_ERR(dev_csr);
		goto err;
	}

	/* A PMU device node without enable-bit-index is always enabled */
	rc = acpi_dev_get_property(adev, "enable-bit-index",
				   ACPI_TYPE_INTEGER, &obj);
	if (rc < 0)
		enable_bit = 0;
	else
		enable_bit = (int) obj->integer.value;

	ctx->name = xgene_pmu_dev_name(type, enable_bit);
	inf = &ctx->inf;
	inf->type = type;
	inf->csr = dev_csr;
	inf->enable_mask = 1 << enable_bit;

	return ctx;
err:
	devm_kfree(dev, ctx);
	return NULL;
}

static acpi_status acpi_pmu_dev_add(acpi_handle handle, u32 level,
				    void *data, void **return_value)
{
	struct xgene_pmu *xgene_pmu = data;
	struct xgene_pmu_dev_ctx *ctx;
	struct acpi_device *adev;

	if (acpi_bus_get_device(handle, &adev))
		return AE_OK;
	if (acpi_bus_get_status(adev) || !adev->status.present)
		return AE_OK;

	if (!strcmp(acpi_device_hid(adev), "APMC0D5D"))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_L3C);
	else if (!strcmp(acpi_device_hid(adev), "APMC0D5E"))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_IOB);
	else if (!strcmp(acpi_device_hid(adev), "APMC0D5F"))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_MCB);
	else if (!strcmp(acpi_device_hid(adev), "APMC0D60"))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_MC);
	else
		ctx = NULL;

	if (!ctx)
		return AE_OK;

	if (xgene_pmu_dev_add(xgene_pmu, ctx))
		return AE_OK;

	switch (ctx->inf.type) {
	case PMU_TYPE_L3C:
		list_add(&ctx->next, &xgene_pmu->l3cpmus);
		break;
	case PMU_TYPE_IOB:
		list_add(&ctx->next, &xgene_pmu->iobpmus);
		break;
	case PMU_TYPE_MCB:
		list_add(&ctx->next, &xgene_pmu->mcbpmus);
		break;
	case PMU_TYPE_MC:
		list_add(&ctx->next, &xgene_pmu->mcpmus);
		break;
	}
	return AE_OK;
}

static int acpi_pmu_probe_pmu_dev(struct xgene_pmu *xgene_pmu,
				  struct platform_device *pdev)
{
	struct device *dev = xgene_pmu->dev;
	acpi_handle handle;
	acpi_status status;

	handle = ACPI_HANDLE(dev);
	if (!handle)
		return -EINVAL;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,
				     acpi_pmu_dev_add, NULL, xgene_pmu, NULL);
	if (ACPI_FAILURE(status))
		dev_err(dev, "failed to probe PMU devices\n");
	return 0;
}
#else
static int acpi_pmu_probe_pmu_dev(struct xgene_pmu *xgene_pmu,
				  struct platform_device *pdev)
{
	return 0;
}
#endif

static struct
xgene_pmu_dev_ctx *fdt_get_pmu_hw_inf(struct xgene_pmu *xgene_pmu,
				      struct device_node *np, u32 type)
{
	struct device *dev = xgene_pmu->dev;
	struct xgene_pmu_dev_ctx *ctx;
	struct hw_pmu_info *inf;
	void __iomem *dev_csr;
	struct resource res;
	int enable_bit;
	int rc;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	rc = of_address_to_resource(np, 0, &res);
	if (rc < 0) {
		dev_err(dev, "PMU type %d: No resource address found\n", type);
		goto err;
	}
	dev_csr = devm_ioremap_resource(dev, &res);
	if (IS_ERR(dev_csr)) {
		dev_err(dev, "PMU type %d: Fail to map resource\n", type);
		rc = PTR_ERR(dev_csr);
		goto err;
	}

	/* A PMU device node without enable-bit-index is always enabled */
	if (of_property_read_u32(np, "enable-bit-index", &enable_bit))
		enable_bit = 0;

	ctx->name = xgene_pmu_dev_name(type, enable_bit);
	inf = &ctx->inf;
	inf->type = type;
	inf->csr = dev_csr;
	inf->enable_mask = 1 << enable_bit;

	return ctx;
err:
	devm_kfree(dev, ctx);
	return NULL;
}

static int fdt_pmu_probe_pmu_dev(struct xgene_pmu *xgene_pmu,
				 struct platform_device *pdev)
{
	struct xgene_pmu_dev_ctx *ctx;
	struct device_node *np;

	for_each_child_of_node(pdev->dev.of_node, np) {
		if (!of_device_is_available(np))
			continue;

		if (of_device_is_compatible(np, "apm,xgene-pmu-l3c"))
			ctx = fdt_get_pmu_hw_inf(xgene_pmu, np, PMU_TYPE_L3C);
		else if (of_device_is_compatible(np, "apm,xgene-pmu-iob"))
			ctx = fdt_get_pmu_hw_inf(xgene_pmu, np, PMU_TYPE_IOB);
		else if (of_device_is_compatible(np, "apm,xgene-pmu-mcb"))
			ctx = fdt_get_pmu_hw_inf(xgene_pmu, np, PMU_TYPE_MCB);
		else if (of_device_is_compatible(np, "apm,xgene-pmu-mc"))
			ctx = fdt_get_pmu_hw_inf(xgene_pmu, np, PMU_TYPE_MC);
		else
			ctx = NULL;

		if (!ctx)
			continue;

		if (xgene_pmu_dev_add(xgene_pmu, ctx))
			continue;

		switch (ctx->inf.type) {
		case PMU_TYPE_L3C:
			list_add(&ctx->next, &xgene_pmu->l3cpmus);
			break;
		case PMU_TYPE_IOB:
			list_add(&ctx->next, &xgene_pmu->iobpmus);
			break;
		case PMU_TYPE_MCB:
			list_add(&ctx->next, &xgene_pmu->mcbpmus);
			break;
		case PMU_TYPE_MC:
			list_add(&ctx->next, &xgene_pmu->mcpmus);
			break;
		}
	}

	return 0;
}

static int xgene_pmu_probe_pmu_dev(struct xgene_pmu *xgene_pmu,
				   struct platform_device *pdev)
{
	if (has_acpi_companion(&pdev->dev))
		return acpi_pmu_probe_pmu_dev(xgene_pmu, pdev);
	return fdt_pmu_probe_pmu_dev(xgene_pmu, pdev);
}

static const struct xgene_pmu_data xgene_pmu_data = {
	.id   = PCP_PMU_V1,
	.data = 0,
};

static const struct xgene_pmu_data xgene_pmu_v2_data = {
	.id   = PCP_PMU_V2,
	.data = 0,
};

static const struct of_device_id xgene_pmu_of_match[] = {
	{ .compatible	= "apm,xgene-pmu",	.data = &xgene_pmu_data },
	{ .compatible	= "apm,xgene-pmu-v2",	.data = &xgene_pmu_v2_data },
	{},
};
MODULE_DEVICE_TABLE(of, xgene_pmu_of_match);
#ifdef CONFIG_ACPI
static const struct acpi_device_id xgene_pmu_acpi_match[] = {
	{"APMC0D5B", PCP_PMU_V1},
	{"APMC0D5C", PCP_PMU_V2},
	{},
};
MODULE_DEVICE_TABLE(acpi, xgene_pmu_acpi_match);
#endif

static int xgene_pmu_probe(struct platform_device *pdev)
{
	const struct xgene_pmu_data *dev_data;
	const struct of_device_id *of_id;
	struct xgene_pmu *xgene_pmu;
	struct resource *res;
	int irq, rc, i;
	int version;

	xgene_pmu = devm_kzalloc(&pdev->dev, sizeof(*xgene_pmu), GFP_KERNEL);
	if (!xgene_pmu)
		return -ENOMEM;
	xgene_pmu->dev = &pdev->dev;
	platform_set_drvdata(pdev, xgene_pmu);

	version = -EINVAL;
	of_id = of_match_device(xgene_pmu_of_match, &pdev->dev);
	if (of_id) {
		dev_data = (const struct xgene_pmu_data *) of_id->data;
		version = dev_data->id;
	}

#ifdef CONFIG_ACPI
	if (ACPI_COMPANION(&pdev->dev)) {
		const struct acpi_device_id *acpi_id;

		acpi_id = acpi_match_device(xgene_pmu_acpi_match, &pdev->dev);
		if (acpi_id)
			version = (int) acpi_id->driver_data;
	}
#endif
	if (version < 0)
		return -ENODEV;

	INIT_LIST_HEAD(&xgene_pmu->l3cpmus);
	INIT_LIST_HEAD(&xgene_pmu->iobpmus);
	INIT_LIST_HEAD(&xgene_pmu->mcbpmus);
	INIT_LIST_HEAD(&xgene_pmu->mcpmus);

	xgene_pmu->version = version;
	dev_info(&pdev->dev, "X-Gene PMU version %d\n", xgene_pmu->version);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xgene_pmu->pcppmu_csr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(xgene_pmu->pcppmu_csr)) {
		dev_err(&pdev->dev, "ioremap failed for PCP PMU resource\n");
		rc = PTR_ERR(xgene_pmu->pcppmu_csr);
		goto err;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		rc = -EINVAL;
		goto err;
	}
	rc = devm_request_irq(&pdev->dev, irq, xgene_pmu_isr, IRQF_SHARED,
			      dev_name(&pdev->dev), xgene_pmu);
	if (rc) {
		dev_err(&pdev->dev, "Could not request IRQ %d\n", irq);
		goto err;
	}

	/* Check for active MCBs and MCUs */
	rc = xgene_pmu_probe_active_mcb_mcu(xgene_pmu, pdev);
	if (rc) {
		dev_warn(&pdev->dev, "Unknown MCB/MCU active status\n");
		xgene_pmu->mcb_active_mask = 0x1;
		xgene_pmu->mc_active_mask = 0x1;
	}

	/* Pick one core to use for cpumask attributes */
	cpumask_set_cpu(smp_processor_id(), &xgene_pmu->cpu);

	/* Make sure that the overflow interrupt is handled by this CPU */
	rc = irq_set_affinity(irq, &xgene_pmu->cpu);
	if (rc) {
		dev_err(&pdev->dev, "Failed to set interrupt affinity!\n");
		goto err;
	}

	/* Enable interrupt */
	xgene_pmu_unmask_int(xgene_pmu);

	/* Populate PMU event atrribute arrays */
	for (i = 0; i < ARRAY_SIZE(l3c_pmu_events); i++)
		l3c_pmu_events_attrs[i] =
			(struct attribute *) &l3c_pmu_events[i].attr.attr;
	for (i = 0; i < ARRAY_SIZE(iob_pmu_events); i++)
		iob_pmu_events_attrs[i] =
			(struct attribute *) &iob_pmu_events[i].attr.attr;
	for (i = 0; i < ARRAY_SIZE(mcb_pmu_events); i++)
		mcb_pmu_events_attrs[i] =
			(struct attribute *) &mcb_pmu_events[i].attr.attr;
	for (i = 0; i < ARRAY_SIZE(mc_pmu_events); i++)
		mc_pmu_events_attrs[i] =
			(struct attribute *) &mc_pmu_events[i].attr.attr;

	/* Walk through the tree for all PMU perf devices */
	rc = xgene_pmu_probe_pmu_dev(xgene_pmu, pdev);
	if (rc) {
		dev_err(&pdev->dev, "No PMU perf devices found!\n");
		goto err;
	}

	return 0;

err:
	if (xgene_pmu->pcppmu_csr)
		devm_iounmap(&pdev->dev, xgene_pmu->pcppmu_csr);
	devm_kfree(&pdev->dev, xgene_pmu);

	return rc;
}

static void
xgene_pmu_dev_cleanup(struct xgene_pmu *xgene_pmu, struct list_head *pmus)
{
	struct xgene_pmu_dev_ctx *ctx, *temp_ctx;
	struct device *dev = xgene_pmu->dev;
	struct xgene_pmu_dev *pmu_dev;

	list_for_each_entry_safe(ctx, temp_ctx, pmus, next) {
		pmu_dev = ctx->pmu_dev;
		if (pmu_dev->inf->csr)
			devm_iounmap(dev, pmu_dev->inf->csr);
		devm_kfree(dev, ctx);
		devm_kfree(dev, pmu_dev);
	}
}

static int xgene_pmu_remove(struct platform_device *pdev)
{
	struct xgene_pmu *xgene_pmu = dev_get_drvdata(&pdev->dev);

	xgene_pmu_dev_cleanup(xgene_pmu, &xgene_pmu->l3cpmus);
	xgene_pmu_dev_cleanup(xgene_pmu, &xgene_pmu->iobpmus);
	xgene_pmu_dev_cleanup(xgene_pmu, &xgene_pmu->mcbpmus);
	xgene_pmu_dev_cleanup(xgene_pmu, &xgene_pmu->mcpmus);

	if (xgene_pmu->pcppmu_csr)
		devm_iounmap(&pdev->dev, xgene_pmu->pcppmu_csr);
	devm_kfree(&pdev->dev, xgene_pmu);

	return 0;
}

static struct platform_driver xgene_pmu_driver = {
	.probe = xgene_pmu_probe,
	.remove = xgene_pmu_remove,
	.driver = {
		.name		= "xgene-pmu",
		.of_match_table = xgene_pmu_of_match,
		.acpi_match_table = ACPI_PTR(xgene_pmu_acpi_match),
	},
};

module_platform_driver(xgene_pmu_driver);

MODULE_DESCRIPTION("APM X-Gene SoC PMU driver");
MODULE_AUTHOR("Hoan Tran <hotran@apm.com>");
MODULE_AUTHOR("Tai Nguyen <ttnguyen@apm.com>");
MODULE_LICENSE("GPL");
