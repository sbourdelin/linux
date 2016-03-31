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
#include <linux/efi.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/perf_event.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

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
#define _GET_CNTR(ev)     ((u8)(ev->hw.extra_reg.reg))
#define _GET_EVENTID(ev)  (ev->hw.config & 0xFFULL)
#define _GET_AGENTID(ev)  (ev->hw.extra_reg.config & 0xFFFFFFFFULL)
#define _GET_AGENT1ID(ev) ((ev->hw.extra_reg.config >> 32) & 0xFFFFFFFFULL)

struct hw_pmu_info {
	u32			id;
	u32			type;
	void __iomem		*csr;
};

struct xgene_pmu_dev {
	struct hw_pmu_info	*inf;
	struct pmu		pmu;
	u8			max_counters;
	u64			cntr_assign_mask;
	raw_spinlock_t		lock;
	u64			max_period;
	const struct attribute_group *attr_groups[4];
	struct perf_event	*pmu_counter_event[4];
	struct xgene_pmu	*parent;
};

struct xgene_pmu {
	struct device		*dev;
	int			version;
	void __iomem		*pcppmu_csr;
	struct list_head	l3cpmus;
	struct list_head	iobpmus;
	struct list_head	mcbpmus;
	struct list_head	mcpmus;
	u32			mcb_active_mask;
	u32			mc_active_mask;
};

struct xgene_pmu_dev_ctx {
	struct list_head	next;
	struct xgene_pmu_dev	*pmu_dev;
	struct hw_pmu_info	inf;
};

#define format_group	attr_groups[0]
#define cpumask_group	attr_groups[1]
#define events_group	attr_groups[2]
#define null_group	attr_groups[3]

struct xgene_pmu_data {
	int	id;
	u32	data;
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
#define XGENE_PMU_FORMAT_ATTR(_id, _name, _format)		\
static ssize_t							\
_id##_name##_show(struct device *dev,				\
		  struct device_attribute *attr,		\
		  char *page)					\
{								\
	BUILD_BUG_ON(sizeof(_format) >= PAGE_SIZE);		\
	return sprintf(page, _format "\n");			\
}								\
								\
static struct device_attribute format_attr_##_id##_name = __ATTR_RO(_id##_name)

XGENE_PMU_FORMAT_ATTR(l3c, eventid, "config:0-7");
XGENE_PMU_FORMAT_ATTR(l3c, agentid, "config1:0-9");

static struct attribute *l3cpmu_format_attrs[] = {
	&format_attr_l3ceventid.attr,
	&format_attr_l3cagentid.attr,
	NULL,
};

static struct attribute_group xgene_l3cpmu_format_group = {
	.name = "format",
	.attrs = l3cpmu_format_attrs,
};

XGENE_PMU_FORMAT_ATTR(iob, eventid, "config:0-7");
XGENE_PMU_FORMAT_ATTR(iob, agentid, "config1:0-63");

static struct attribute *iobpmu_format_attrs[] = {
	&format_attr_iobeventid.attr,
	&format_attr_iobagentid.attr,
	NULL,
};

static struct attribute_group xgene_iobpmu_format_group = {
	.name = "format",
	.attrs = iobpmu_format_attrs,
};

XGENE_PMU_FORMAT_ATTR(mcb, eventid, "config:0-5");
XGENE_PMU_FORMAT_ATTR(mcb, agentid, "config1:0-9");

static struct attribute *mcbpmu_format_attrs[] = {
	&format_attr_mcbeventid.attr,
	&format_attr_mcbagentid.attr,
	NULL,
};

static struct attribute_group xgene_mcbpmu_format_group = {
	.name = "format",
	.attrs = mcbpmu_format_attrs,
};

XGENE_PMU_FORMAT_ATTR(mc, eventid, "config:0-28");

static struct attribute *mcpmu_format_attrs[] = {
	&format_attr_mceventid.attr,
	NULL,
	NULL,
};

static struct attribute_group xgene_mcpmu_format_group = {
	.name = "format",
	.attrs = mcpmu_format_attrs,
};

/*
 * sysfs event attributes
 */
struct xgene_pmu_dev_event_desc {
	struct kobj_attribute attr;
	const char *event;
};

static ssize_t _xgene_pmu_event_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct xgene_pmu_dev_event_desc *event;

	event = container_of(attr, struct xgene_pmu_dev_event_desc, attr);
	return sprintf(buf, "%s\n", event->event);
}

#define XGENE_PMU_EVENT_DESC(_name, _event)				\
{									\
	.attr  = __ATTR(_name, 0444, _xgene_pmu_event_show, NULL),	\
	.event = _event,						\
}

static struct xgene_pmu_dev_event_desc xgene_l3c_event_descs[] = {
	XGENE_PMU_EVENT_DESC(cycle-count,			"config=0x00"),
	XGENE_PMU_EVENT_DESC(cycle-count-div-64,		"config=0x01"),
	XGENE_PMU_EVENT_DESC(read-hit,				"config=0x02"),
	XGENE_PMU_EVENT_DESC(read-miss,				"config=0x03"),
	XGENE_PMU_EVENT_DESC(write-need-replacement,		"config=0x06"),
	XGENE_PMU_EVENT_DESC(write-not-need-replacement,	"config=0x07"),
	XGENE_PMU_EVENT_DESC(tq-full,				"config=0x08"),
	XGENE_PMU_EVENT_DESC(ackq-full,				"config=0x09"),
	XGENE_PMU_EVENT_DESC(wdb-full,				"config=0x0a"),
	XGENE_PMU_EVENT_DESC(bank-fifo-full,			"config=0x0b"),
	XGENE_PMU_EVENT_DESC(odb-full,				"config=0x0c"),
	XGENE_PMU_EVENT_DESC(wbq-full,				"config=0x0d"),
	XGENE_PMU_EVENT_DESC(bank-conflict-fifo-issue,		"config=0x0e"),
	XGENE_PMU_EVENT_DESC(bank-fifo-issue,			"config=0x0f"),
	{ },
};

static struct xgene_pmu_dev_event_desc xgene_iob_event_descs[] = {
	XGENE_PMU_EVENT_DESC(cycle-count,			"config=0x00"),
	XGENE_PMU_EVENT_DESC(cycle-count-div-64,		"config=0x01"),
	XGENE_PMU_EVENT_DESC(axi0-read,				"config=0x02"),
	XGENE_PMU_EVENT_DESC(axi0-read-partial,			"config=0x03"),
	XGENE_PMU_EVENT_DESC(axi1-read,				"config=0x04"),
	XGENE_PMU_EVENT_DESC(axi1-read-partial,			"config=0x05"),
	XGENE_PMU_EVENT_DESC(csw-read-block,			"config=0x06"),
	XGENE_PMU_EVENT_DESC(csw-read-partial,			"config=0x07"),
	XGENE_PMU_EVENT_DESC(axi0-write,			"config=0x10"),
	XGENE_PMU_EVENT_DESC(axi0-write-partial,		"config=0x11"),
	XGENE_PMU_EVENT_DESC(axi1-write,			"config=0x13"),
	XGENE_PMU_EVENT_DESC(axi1-write-partial,		"config=0x14"),
	XGENE_PMU_EVENT_DESC(csw-inbound-dirty,			"config=0x16"),
	{ },
};

static struct xgene_pmu_dev_event_desc xgene_mcb_event_descs[] = {
	XGENE_PMU_EVENT_DESC(cycle-count,			"config=0x00"),
	XGENE_PMU_EVENT_DESC(cycle-count-div-64,		"config=0x01"),
	XGENE_PMU_EVENT_DESC(csw-read,				"config=0x02"),
	XGENE_PMU_EVENT_DESC(csw-write-request,			"config=0x03"),
	XGENE_PMU_EVENT_DESC(mcb-csw-stall,			"config=0x04"),
	XGENE_PMU_EVENT_DESC(cancel-read-gack,			"config=0x05"),
	{ },
};

static struct xgene_pmu_dev_event_desc xgene_mc_event_descs[] = {
	XGENE_PMU_EVENT_DESC(cycle-count,			"config=0x00"),
	XGENE_PMU_EVENT_DESC(cycle-count-div-64,		"config=0x01"),
	XGENE_PMU_EVENT_DESC(act-cmd-sent,			"config=0x02"),
	XGENE_PMU_EVENT_DESC(pre-cmd-sent,			"config=0x03"),
	XGENE_PMU_EVENT_DESC(rd-cmd-sent,			"config=0x04"),
	XGENE_PMU_EVENT_DESC(rda-cmd-sent,			"config=0x05"),
	XGENE_PMU_EVENT_DESC(wr-cmd-sent,			"config=0x06"),
	XGENE_PMU_EVENT_DESC(wra-cmd-sent,			"config=0x07"),
	XGENE_PMU_EVENT_DESC(pde-cmd-sent,			"config=0x08"),
	XGENE_PMU_EVENT_DESC(sre-cmd-sent,			"config=0x09"),
	XGENE_PMU_EVENT_DESC(prea-cmd-sent,			"config=0x0a"),
	XGENE_PMU_EVENT_DESC(ref-cmd-sent,			"config=0x0b"),
	XGENE_PMU_EVENT_DESC(rd-rda-cmd-sent,			"config=0x0c"),
	XGENE_PMU_EVENT_DESC(wr-wra-cmd-sent,			"config=0x0d"),
	XGENE_PMU_EVENT_DESC(in-rd-collision,			"config=0x0e"),
	XGENE_PMU_EVENT_DESC(in-wr-collision,			"config=0x0f"),
	XGENE_PMU_EVENT_DESC(collision-queue-not-empty,		"config=0x10"),
	XGENE_PMU_EVENT_DESC(collision-queue-full,		"config=0x11"),
	XGENE_PMU_EVENT_DESC(mcu-request,			"config=0x12"),
	XGENE_PMU_EVENT_DESC(mcu-rd-request,			"config=0x13"),
	XGENE_PMU_EVENT_DESC(mcu-hp-rd-request,			"config=0x14"),
	XGENE_PMU_EVENT_DESC(mcu-wr-request,			"config=0x15"),
	XGENE_PMU_EVENT_DESC(mcu-rd-proceed-all,		"config=0x16"),
	XGENE_PMU_EVENT_DESC(mcu-rd-proceed-cancel,		"config=0x17"),
	XGENE_PMU_EVENT_DESC(mcu-rd-response,			"config=0x18"),
	XGENE_PMU_EVENT_DESC(mcu-rd-proceed-speculative-all,	"config=0x19"),
	XGENE_PMU_EVENT_DESC(mcu-rd-proceed-speculative-cancel,	"config=0x1a"),
	XGENE_PMU_EVENT_DESC(mcu-wr-proceed-all,		"config=0x1b"),
	XGENE_PMU_EVENT_DESC(mcu-wr-proceed-cancel,		"config=0x1c"),
	{ },
};

/*
 * sysfs cpumask attributes
 */
static cpumask_t xgene_pmu_cpumask;

static ssize_t xgene_pmu_cpumask_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &xgene_pmu_cpumask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, xgene_pmu_cpumask_show, NULL);

static struct attribute *xgene_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group xgene_pmu_cpumask_group = {
	.attrs = xgene_pmu_cpumask_attrs,
};

static int get_next_avail_cntr(struct xgene_pmu_dev *pmu_dev)
{
	int shift, cntr, retval;
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu_dev->lock, flags);

	for (cntr = 0; cntr < pmu_dev->max_counters; cntr++) {
		shift = cntr;
		if (!(pmu_dev->cntr_assign_mask & (1ULL << shift))) {
			pmu_dev->cntr_assign_mask |= (1ULL << shift);
			retval = cntr;
			goto out;
		}
	}
	retval = -ENOSPC;

out:
	raw_spin_unlock_irqrestore(&pmu_dev->lock, flags);

	return retval;
}

static int clear_avail_cntr(struct xgene_pmu_dev *pmu_dev, u8 cntr)
{
	unsigned long flags;
	int shift;

	if (cntr > pmu_dev->max_counters)
		return -EINVAL;

	shift = cntr;

	raw_spin_lock_irqsave(&pmu_dev->lock, flags);
	pmu_dev->cntr_assign_mask &= ~(1ULL << shift);
	raw_spin_unlock_irqrestore(&pmu_dev->lock, flags);

	return 0;
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

	/* test the event attr type check for PMU enumeration */
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

	if (event->pmu != &pmu_dev->pmu)
		return -ENOENT;

	if (pmu_dev) {
		config = event->attr.config;
		config1 = event->attr.config1;
	} else {
		return -EINVAL;
	}

	if (pmu_dev->max_counters == 0)
		return -EINVAL;

	hwc->config = config;
	if (config1)
		hwc->extra_reg.config = config1;
	else
		/* Enable all Agents */
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
	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count, count)
		!= prev_raw_count)
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
	int retval;

	event->hw.state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	retval = get_next_avail_cntr(pmu_dev);
	if (retval != -ENOSPC)
		event->hw.extra_reg.reg = (u16) retval;
	else
		return retval;

	if (flags & PERF_EF_START)
		xgene_perf_start(event, PERF_EF_RELOAD);

	/* Update counter event pointer for Interrupt handler */
	pmu_dev->pmu_counter_event[retval] = event;

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

static int xgene_init_events_attrs(struct xgene_pmu_dev *pmu_dev,
				   struct xgene_pmu_dev_event_desc *event_desc)
{
	struct attribute_group *attr_group;
	struct attribute **attrs;
	int i = 0, j;

	while (event_desc[i].attr.attr.name)
		i++;

	attr_group = devm_kzalloc(pmu_dev->parent->dev,
			sizeof(struct attribute *) * (i + 1)
			+ sizeof(*attr_group), GFP_KERNEL);
	if (!attr_group)
		return -ENOMEM;

	attrs = (struct attribute **)(attr_group + 1);
	for (j = 0; j < i; j++)
		attrs[j] = &event_desc[j].attr.attr;

	attr_group->name = "events";
	attr_group->attrs = attrs;
	pmu_dev->events_group = attr_group;

	return 0;
}

static void xgene_pc_exit(struct xgene_pmu_dev *pmu_dev)
{
	if (pmu_dev->events_group != NULL) {
		devm_kfree(pmu_dev->parent->dev,
			   (void *) pmu_dev->events_group);
		pmu_dev->events_group = NULL;
	}
}

static u64 xgene_perf_event_update(struct perf_event *event,
				   struct hw_perf_event *hwc, int idx)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
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

static int xgene_perf_event_set_period(struct perf_event *event,
				       struct hw_perf_event *hwc, int idx)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
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

	xgene_pmu_write_counter(pmu_dev, idx, (u64)(-left) & 0xffffffff);

	perf_event_update_userpage(event);

	return ret;
}

static void xgene_perf_pmu_init(struct xgene_pmu_dev *pmu_dev)
{
	pmu_dev->pmu.event_init	= xgene_perf_event_init;
	pmu_dev->pmu.add	= xgene_perf_add;
	pmu_dev->pmu.del	= xgene_perf_del;
	pmu_dev->pmu.start	= xgene_perf_start;
	pmu_dev->pmu.stop	= xgene_perf_stop;
	pmu_dev->pmu.read	= xgene_perf_read;
}

static int xgene_init_perf(struct xgene_pmu_dev *pmu_dev, char *name)
{
	struct xgene_pmu *xgene_pmu;
	int ret;

	raw_spin_lock_init(&pmu_dev->lock);

	pmu_dev->cpumask_group = &xgene_pmu_cpumask_group;

	pmu_dev->max_period = PMU_CNT_MAX_VAL - 1;
	/* First PMU version supports only single event counter */
	xgene_pmu = pmu_dev->parent;
	if (xgene_pmu->version == 1)
		pmu_dev->max_counters = 1;
	else
		pmu_dev->max_counters = PMU_MAX_COUNTERS;

	/* Init null attributes */
	pmu_dev->null_group = NULL;
	pmu_dev->pmu.attr_groups = pmu_dev->attr_groups;

	/* Hardware counter init */
	xgene_pmu_stop_counters(pmu_dev);
	xgene_pmu_reset_counters(pmu_dev);

	xgene_perf_pmu_init(pmu_dev);
	ret = perf_pmu_register(&pmu_dev->pmu, name, -1);
	if (ret)
		xgene_pc_exit(pmu_dev);

	return ret;
}

static int
xgene_pmu_dev_add(struct xgene_pmu *xgene_pmu, struct xgene_pmu_dev_ctx *ctx)
{
	struct xgene_pmu_dev *pmu;
	struct device *dev = xgene_pmu->dev;
	struct attribute_group *format_group;
	struct xgene_pmu_dev_event_desc *event_desc;
	char name[10];
	int rc;

	pmu = devm_kzalloc(dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;
	pmu->parent = xgene_pmu;
	pmu->inf = &ctx->inf;
	ctx->pmu_dev = pmu;

	switch (pmu->inf->type) {
	case PMU_TYPE_L3C:
		format_group = &xgene_l3cpmu_format_group;
		event_desc = xgene_l3c_event_descs;
		sprintf(name, "l3c%d", pmu->inf->id);
		break;
	case PMU_TYPE_IOB:
		format_group = &xgene_iobpmu_format_group;
		event_desc = xgene_iob_event_descs;
		sprintf(name, "iob%d", pmu->inf->id);
		break;
	case PMU_TYPE_MCB:
		if (!(xgene_pmu->mcb_active_mask & (1 << pmu->inf->id)))
			goto dev_err;
		format_group = &xgene_mcbpmu_format_group;
		event_desc = xgene_mcb_event_descs;
		sprintf(name, "mcb%d", pmu->inf->id);
		break;
	case PMU_TYPE_MC:
		if (!(xgene_pmu->mc_active_mask & (1 << pmu->inf->id)))
			goto dev_err;
		format_group = &xgene_mcpmu_format_group;
		event_desc = xgene_mc_event_descs;
		sprintf(name, "mc%d", pmu->inf->id);
		break;
	default:
		return -EINVAL;
	}

	/* Init agent attributes */
	pmu->format_group = format_group;
	/* Init events attributes */
	if (xgene_init_events_attrs(pmu, event_desc))
		pr_err("PMU: Only support raw events.\n");
	rc = xgene_init_perf(pmu, name);
	if (!rc)
		dev_info(dev, "%s PMU registered\n", name);

	return rc;

dev_err:
	devm_kfree(dev, pmu);
	return -ENODEV;
}

static irqreturn_t _xgene_pmu_isr(int irq, struct xgene_pmu_dev *pmu_dev)
{
	struct perf_event *event = NULL;
	struct perf_sample_data data;
	struct xgene_pmu *xgene_pmu;
	struct hw_perf_event *hwc;
	struct pt_regs *regs;
	int idx;
	u32 val;

	/* Get interrupt counter source */
	val = readl(pmu_dev->inf->csr + PMU_PMOVSR);
	idx = ffs(val) - 1;
	if (!(val & PMU_OVERFLOW_MASK))
		goto out;
	event = pmu_dev->pmu_counter_event[idx];

	/*
	 * Handle the counter(s) overflow(s)
	 */
	regs = get_irq_regs();

	/* Ignore if we don't have an event. */
	if (!event)
		goto out;

	hwc = &event->hw;

	xgene_perf_event_update(event, hwc, idx);
	perf_sample_data_init(&data, 0, hwc->last_period);
	if (!xgene_perf_event_set_period(event, hwc, idx))
		goto out;

	if (perf_event_overflow(event, &data, regs))
		xgene_perf_disable_event(event);

out:
	/* Clear interrupt flag */
	xgene_pmu = pmu_dev->parent;
	if (xgene_pmu->version == 1)
		writel(0x0, pmu_dev->inf->csr + PMU_PMOVSR);
	else
		writel(val, pmu_dev->inf->csr + PMU_PMOVSR);

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
	if (efi_enabled(EFI_BOOT))
		return acpi_pmu_probe_active_mcb_mcu(xgene_pmu, pdev);
	else
		return fdt_pmu_probe_active_mcb_mcu(xgene_pmu, pdev);
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
	u32 id;
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

	rc = acpi_dev_get_property(adev, "index", ACPI_TYPE_INTEGER, &obj);
	if (rc < 0) {
		dev_err(&adev->dev, "No index property found\n");
		id = 0;
	} else {
		id = (u32) obj->integer.value;
	}

	inf = &ctx->inf;
	inf->type = type;
	inf->csr = dev_csr;
	inf->id = id;

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
	u32 id;
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

	if (of_property_read_u32(np, "index", &id)) {
		dev_err(dev, "PMU type %d: No index property found\n", type);
		id = 0;
	}

	inf = &ctx->inf;
	inf->type = type;
	inf->csr = dev_csr;
	inf->id = id;

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
	if (efi_enabled(EFI_BOOT))
		return acpi_pmu_probe_pmu_dev(xgene_pmu, pdev);
	else
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
	int version, dev_id;
	int irq, rc;

	xgene_pmu = devm_kzalloc(&pdev->dev, sizeof(*xgene_pmu), GFP_KERNEL);
	if (!xgene_pmu)
		return -ENOMEM;
	xgene_pmu->dev = &pdev->dev;
	platform_set_drvdata(pdev, xgene_pmu);

	dev_id = -EINVAL;
	of_id = of_match_device(xgene_pmu_of_match, &pdev->dev);
	if (of_id) {
		dev_data = (const struct xgene_pmu_data *) of_id->data;
		dev_id = dev_data->id;
	}

#ifdef CONFIG_ACPI
	if (ACPI_COMPANION(&pdev->dev)) {
		const struct acpi_device_id *acpi_id;

		acpi_id = acpi_match_device(xgene_pmu_acpi_match, &pdev->dev);
		if (acpi_id)
			dev_id = (int) acpi_id->driver_data;
	}
#endif
	if (dev_id < 0)
		return -ENODEV;

	version = (dev_id == PCP_PMU_V1) ? 1 : 2;

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

	/* Init cpumask attributes to only core 0 */
	cpumask_set_cpu(0, &xgene_pmu_cpumask);

	/* Enable interrupt */
	xgene_pmu_unmask_int(xgene_pmu);

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
