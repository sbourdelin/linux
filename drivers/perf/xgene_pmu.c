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

#define CSW_CSWCR			0x0000
#define  CSW_CSWCR_DUALMCB_MASK		BIT(0)
#define  CSW_CSWCR_MCB0_ROUTING(x)	(((x) & 0x0C) >> 2)
#define  CSW_CSWCR_MCB1_ROUTING(x)	(((x) & 0x30) >> 4)

#define MCBADDRMR			0x0000
#define  MCBADDRMR_DUALMCU_MODE_MASK	BIT(2)

#define PCPPMU_INTSTATUS_REG	0x000
#define PCPPMU_INTMASK_REG	0x004
#define  PCPPMU_INTMASK		0x0000000F
#define  PCPPMU_INTENMASK	0xFFFFFFFF
#define  PCPPMU_INTCLRMASK	0xFFFFFFF0
#define  PCPPMU_INT_MCU		BIT(0)
#define  PCPPMU_INT_MCB		BIT(1)
#define  PCPPMU_INT_L3C		BIT(2)
#define  PCPPMU_INT_IOB		BIT(3)

#define  PCPPMU_V3_INTMASK	0x00FF33FF
#define  PCPPMU_V3_INTENMASK	0xFFFFFFFF
#define  PCPPMU_V3_INTCLRMASK	0xFF00CC00
#define  PCPPMU_V3_INT_MCU	0x000000FF
#define  PCPPMU_V3_INT_MCB	0x00000300
#define  PCPPMU_V3_INT_L3C	0x00FF0000
#define  PCPPMU_V3_INT_IOB	0x00003000

#define PMU_MAX_COUNTERS	4
#define PMU_CNT_MAX_PERIOD	0xFFFFFFFFULL
#define PMU_V3_CNT_MAX_PERIOD	0xFFFFFFFFFFFFFFFFULL
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

/* PMU registers for V3 */
#define PMU_PMOVSCLR		0xC80
#define PMU_PMOVSSET		0xCC0
#define PMU_PMAUXR0		0xD80
#define PMU_PMAUXR1		0xD84
#define PMU_PMAUXR2		0xD88
#define PMU_PMAUXR3		0xD8C

/* Default PMU agent ID values for V3 */
#define PCPPMU_V3_L3C_DEFAULT_AGENTID		0x0ULL
#define PCPPMU_V3_IOB_DEFAULT_AGENTID		0x0ULL
#define PCPPMU_V3_IOB_SLOW_DEFAULT_AGENTID	0x0ULL
#define PCPPMU_V3_MCB_DEFAULT_AGENTID		0x100000000ULL
#define PCPPMU_V3_MC_DEFAULT_AGENTID		0xFFFFFF00ULL

#define to_pmu_dev(p)		container_of(p, struct xgene_pmu_dev, pmu)
#define GET_CNTR(ev)		(ev->hw.idx)
#define GET_EVENTID(ev)		(ev->hw.config & 0xFFULL)
#define GET_AGENTID(ev)		(ev->hw.config_base & 0xFFFFFFFFUL)
#define GET_AGENT1ID(ev)	((ev->hw.config_base >> 32) & 0xFFFFFFFFUL)

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
	u64 max_period;
	u64 default_agentid;
	const struct attribute_group **attr_groups;
	struct perf_event *pmu_counter_event[PMU_MAX_COUNTERS];
};

struct xgene_pmu {
	struct device *dev;
	int version;
	void __iomem *pcppmu_csr;
	u32 mcb_active_mask;
	u32 mc_active_mask;
	u32 l3c_active_mask;
	cpumask_t cpu;
	raw_spinlock_t lock;
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

struct xgene_pmu_data {
	int id;
	u32 data;
};

enum xgene_pmu_version {
	PCP_PMU_V1 = 1,
	PCP_PMU_V2,
	PCP_PMU_V3,
};

enum xgene_pmu_dev_type {
	PMU_TYPE_L3C = 0,
	PMU_TYPE_IOB,
	PMU_TYPE_IOB_SLOW,
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

#define XGENE_PMU_FORMAT_ATTR(_name, _config)		\
	(&((struct dev_ext_attribute[]) {		\
		{ .attr = __ATTR(_name, S_IRUGO, xgene_pmu_format_show, NULL), \
		  .var = (void *) _config, }		\
	})[0].attr.attr)

static struct attribute *l3c_pmu_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(l3c_eventid, "config:0-7"),
	XGENE_PMU_FORMAT_ATTR(l3c_agentid, "config1:0-9"),
	NULL,
};

static struct attribute *iob_pmu_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(iob_eventid, "config:0-7"),
	XGENE_PMU_FORMAT_ATTR(iob_agentid, "config1:0-63"),
	NULL,
};

static struct attribute *mcb_pmu_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(mcb_eventid, "config:0-5"),
	XGENE_PMU_FORMAT_ATTR(mcb_agentid, "config1:0-9"),
	NULL,
};

static struct attribute *mc_pmu_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(mc_eventid, "config:0-28"),
	NULL,
};

static const struct attribute_group l3c_pmu_format_attr_group = {
	.name = "format",
	.attrs = l3c_pmu_format_attrs,
};

static const struct attribute_group iob_pmu_format_attr_group = {
	.name = "format",
	.attrs = iob_pmu_format_attrs,
};

static const struct attribute_group mcb_pmu_format_attr_group = {
	.name = "format",
	.attrs = mcb_pmu_format_attrs,
};

static const struct attribute_group mc_pmu_format_attr_group = {
	.name = "format",
	.attrs = mc_pmu_format_attrs,
};

static struct attribute *l3c_pmu_v3_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(l3c_eventid, "config:0-39"),
	XGENE_PMU_FORMAT_ATTR(l3c_agentid, "config1:0-31"),
	NULL,
};

static struct attribute *iob_pmu_v3_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(iob_eventid, "config:0-47"),
	XGENE_PMU_FORMAT_ATTR(iob_agentid, "config1:0-63"),
	NULL,
};

static struct attribute *iob_slow_pmu_v3_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(iob_slow_eventid, "config:0-16"),
	XGENE_PMU_FORMAT_ATTR(iob_slow_agentid, "config1:0-63"),
	NULL,
};

static struct attribute *mcb_pmu_v3_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(mcb_eventid, "config:0-35"),
	XGENE_PMU_FORMAT_ATTR(mcb_agentid, "config1:0-63"),
	NULL,
};

static struct attribute *mc_pmu_v3_format_attrs[] = {
	XGENE_PMU_FORMAT_ATTR(mc_eventid, "config:0-44"),
	XGENE_PMU_FORMAT_ATTR(mc_agentid, "config1:0-31"),
	NULL,
};

static const struct attribute_group l3c_pmu_v3_format_attr_group = {
	.name = "format",
	.attrs = l3c_pmu_v3_format_attrs,
};

static const struct attribute_group iob_pmu_v3_format_attr_group = {
	.name = "format",
	.attrs = iob_pmu_v3_format_attrs,
};

static const struct attribute_group iob_slow_pmu_v3_format_attr_group = {
	.name = "format",
	.attrs = iob_slow_pmu_v3_format_attrs,
};

static const struct attribute_group mcb_pmu_v3_format_attr_group = {
	.name = "format",
	.attrs = mcb_pmu_v3_format_attrs,
};

static const struct attribute_group mc_pmu_v3_format_attr_group = {
	.name = "format",
	.attrs = mc_pmu_v3_format_attrs,
};
/*
 * sysfs event attributes
 */
static ssize_t xgene_pmu_event_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sprintf(buf, "config=0x%lx\n", (unsigned long) eattr->var);
}

#define XGENE_PMU_EVENT_ATTR(_name, _config)		\
	(&((struct dev_ext_attribute[]) {		\
		{ .attr = __ATTR(_name, S_IRUGO, xgene_pmu_event_show, NULL), \
		  .var = (void *) _config, }		\
	 })[0].attr.attr)

static struct attribute *l3c_pmu_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT_ATTR(read-hit,				0x02),
	XGENE_PMU_EVENT_ATTR(read-miss,				0x03),
	XGENE_PMU_EVENT_ATTR(write-need-replacement,		0x06),
	XGENE_PMU_EVENT_ATTR(write-not-need-replacement,	0x07),
	XGENE_PMU_EVENT_ATTR(tq-full,				0x08),
	XGENE_PMU_EVENT_ATTR(ackq-full,				0x09),
	XGENE_PMU_EVENT_ATTR(wdb-full,				0x0a),
	XGENE_PMU_EVENT_ATTR(bank-fifo-full,			0x0b),
	XGENE_PMU_EVENT_ATTR(odb-full,				0x0c),
	XGENE_PMU_EVENT_ATTR(wbq-full,				0x0d),
	XGENE_PMU_EVENT_ATTR(bank-conflict-fifo-issue,		0x0e),
	XGENE_PMU_EVENT_ATTR(bank-fifo-issue,			0x0f),
	NULL,
};

static struct attribute *iob_pmu_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT_ATTR(axi0-read,				0x02),
	XGENE_PMU_EVENT_ATTR(axi0-read-partial,			0x03),
	XGENE_PMU_EVENT_ATTR(axi1-read,				0x04),
	XGENE_PMU_EVENT_ATTR(axi1-read-partial,			0x05),
	XGENE_PMU_EVENT_ATTR(csw-read-block,			0x06),
	XGENE_PMU_EVENT_ATTR(csw-read-partial,			0x07),
	XGENE_PMU_EVENT_ATTR(axi0-write,			0x10),
	XGENE_PMU_EVENT_ATTR(axi0-write-partial,		0x11),
	XGENE_PMU_EVENT_ATTR(axi1-write,			0x13),
	XGENE_PMU_EVENT_ATTR(axi1-write-partial,		0x14),
	XGENE_PMU_EVENT_ATTR(csw-inbound-dirty,			0x16),
	NULL,
};

static struct attribute *mcb_pmu_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT_ATTR(csw-read,				0x02),
	XGENE_PMU_EVENT_ATTR(csw-write-request,			0x03),
	XGENE_PMU_EVENT_ATTR(mcb-csw-stall,			0x04),
	XGENE_PMU_EVENT_ATTR(cancel-read-gack,			0x05),
	NULL,
};

static struct attribute *mc_pmu_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(cycle-count-div-64,		0x01),
	XGENE_PMU_EVENT_ATTR(act-cmd-sent,			0x02),
	XGENE_PMU_EVENT_ATTR(pre-cmd-sent,			0x03),
	XGENE_PMU_EVENT_ATTR(rd-cmd-sent,			0x04),
	XGENE_PMU_EVENT_ATTR(rda-cmd-sent,			0x05),
	XGENE_PMU_EVENT_ATTR(wr-cmd-sent,			0x06),
	XGENE_PMU_EVENT_ATTR(wra-cmd-sent,			0x07),
	XGENE_PMU_EVENT_ATTR(pde-cmd-sent,			0x08),
	XGENE_PMU_EVENT_ATTR(sre-cmd-sent,			0x09),
	XGENE_PMU_EVENT_ATTR(prea-cmd-sent,			0x0a),
	XGENE_PMU_EVENT_ATTR(ref-cmd-sent,			0x0b),
	XGENE_PMU_EVENT_ATTR(rd-rda-cmd-sent,			0x0c),
	XGENE_PMU_EVENT_ATTR(wr-wra-cmd-sent,			0x0d),
	XGENE_PMU_EVENT_ATTR(in-rd-collision,			0x0e),
	XGENE_PMU_EVENT_ATTR(in-wr-collision,			0x0f),
	XGENE_PMU_EVENT_ATTR(collision-queue-not-empty,		0x10),
	XGENE_PMU_EVENT_ATTR(collision-queue-full,		0x11),
	XGENE_PMU_EVENT_ATTR(mcu-request,			0x12),
	XGENE_PMU_EVENT_ATTR(mcu-rd-request,			0x13),
	XGENE_PMU_EVENT_ATTR(mcu-hp-rd-request,			0x14),
	XGENE_PMU_EVENT_ATTR(mcu-wr-request,			0x15),
	XGENE_PMU_EVENT_ATTR(mcu-rd-proceed-all,		0x16),
	XGENE_PMU_EVENT_ATTR(mcu-rd-proceed-cancel,		0x17),
	XGENE_PMU_EVENT_ATTR(mcu-rd-response,			0x18),
	XGENE_PMU_EVENT_ATTR(mcu-rd-proceed-speculative-all,	0x19),
	XGENE_PMU_EVENT_ATTR(mcu-rd-proceed-speculative-cancel,	0x1a),
	XGENE_PMU_EVENT_ATTR(mcu-wr-proceed-all,		0x1b),
	XGENE_PMU_EVENT_ATTR(mcu-wr-proceed-cancel,		0x1c),
	NULL,
};

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

static struct attribute *l3c_pmu_v3_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(read-hit,				0x01),
	XGENE_PMU_EVENT_ATTR(read-miss,				0x02),
	XGENE_PMU_EVENT_ATTR(index-flush-eviction,		0x03),
	XGENE_PMU_EVENT_ATTR(write-caused-replacement,		0x04),
	XGENE_PMU_EVENT_ATTR(write-not-caused-replacement,	0x05),
	XGENE_PMU_EVENT_ATTR(clean-eviction,			0x06),
	XGENE_PMU_EVENT_ATTR(dirty-eviction,			0x07),
	XGENE_PMU_EVENT_ATTR(reads,				0x08),
	XGENE_PMU_EVENT_ATTR(writes,				0x09),
	XGENE_PMU_EVENT_ATTR(requests,				0x0a),
	XGENE_PMU_EVENT_ATTR(tq-bank-conflict-issue-stall,	0x0b),
	XGENE_PMU_EVENT_ATTR(tq-full,				0x0c),
	XGENE_PMU_EVENT_ATTR(ackq-full,				0x0d),
	XGENE_PMU_EVENT_ATTR(wdb-full,				0x0e),
	XGENE_PMU_EVENT_ATTR(odb-full,				0x10),
	XGENE_PMU_EVENT_ATTR(wbq-full,				0x11),
	XGENE_PMU_EVENT_ATTR(input-req-async-fifo-stall,	0x12),
	XGENE_PMU_EVENT_ATTR(output-req-async-fifo-stall,	0x13),
	XGENE_PMU_EVENT_ATTR(output-data-async-fifo-stall,	0x14),
	XGENE_PMU_EVENT_ATTR(total-insertions,			0x15),
	XGENE_PMU_EVENT_ATTR(sip-insertions-r-set,		0x16),
	XGENE_PMU_EVENT_ATTR(sip-insertions-r-clear,		0x17),
	XGENE_PMU_EVENT_ATTR(dip-insertions-r-set,		0x18),
	XGENE_PMU_EVENT_ATTR(dip-insertions-r-clear,		0x19),
	XGENE_PMU_EVENT_ATTR(dip-insertions-force-r-set,	0x1a),
	XGENE_PMU_EVENT_ATTR(egressions,			0x1b),
	XGENE_PMU_EVENT_ATTR(replacements,			0x1c),
	XGENE_PMU_EVENT_ATTR(old-replacements,			0x1d),
	XGENE_PMU_EVENT_ATTR(young-replacements,		0x1e),
	XGENE_PMU_EVENT_ATTR(r-set-replacements,		0x1f),
	XGENE_PMU_EVENT_ATTR(r-clear-replacements,		0x20),
	XGENE_PMU_EVENT_ATTR(old-r-replacements,		0x21),
	XGENE_PMU_EVENT_ATTR(old-nr-replacements,		0x22),
	XGENE_PMU_EVENT_ATTR(young-r-replacements,		0x23),
	XGENE_PMU_EVENT_ATTR(young-nr-replacements,		0x24),
	XGENE_PMU_EVENT_ATTR(bloomfilter-clearings,		0x25),
	XGENE_PMU_EVENT_ATTR(generation-flips,			0x26),
	XGENE_PMU_EVENT_ATTR(vcc-droop-detected,		0x27),
	NULL,
};

static struct attribute *iob_fast_pmu_v3_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(PA-req-buf-alloc-all,		0x01),
	XGENE_PMU_EVENT_ATTR(PA-req-buf-alloc-rd,		0x02),
	XGENE_PMU_EVENT_ATTR(PA-req-buf-alloc-wr,		0x03),
	XGENE_PMU_EVENT_ATTR(PA-all-CP-req,			0x04),
	XGENE_PMU_EVENT_ATTR(PA-CP-blk-req,			0x05),
	XGENE_PMU_EVENT_ATTR(PA-CP-ptl-req,			0x06),
	XGENE_PMU_EVENT_ATTR(PA-CP-rd-req,			0x07),
	XGENE_PMU_EVENT_ATTR(PA-CP-wr-req,			0x08),
	XGENE_PMU_EVENT_ATTR(BA-all-req,			0x09),
	XGENE_PMU_EVENT_ATTR(BA-rd-req,				0x0a),
	XGENE_PMU_EVENT_ATTR(BA-wr-req,				0x0b),
	XGENE_PMU_EVENT_ATTR(PA-rd-shared-req-issued,		0x10),
	XGENE_PMU_EVENT_ATTR(PA-rd-exclusive-req-issued,	0x11),
	XGENE_PMU_EVENT_ATTR(PA-wr-invalidate-req-issued-stashable, 0x12),
	XGENE_PMU_EVENT_ATTR(PA-wr-invalidate-req-issued-nonstashable, 0x13),
	XGENE_PMU_EVENT_ATTR(PA-wr-back-req-issued-stashable,	0x14),
	XGENE_PMU_EVENT_ATTR(PA-wr-back-req-issued-nonstashable, 0x15),
	XGENE_PMU_EVENT_ATTR(PA-ptl-wr-req,			0x16),
	XGENE_PMU_EVENT_ATTR(PA-ptl-rd-req,			0x17),
	XGENE_PMU_EVENT_ATTR(PA-wr-back-clean-data,		0x18),
	XGENE_PMU_EVENT_ATTR(PA-wr-back-cancelled-on-SS,	0x1b),
	XGENE_PMU_EVENT_ATTR(PA-barrier-occurrence,		0x1c),
	XGENE_PMU_EVENT_ATTR(PA-barrier-cycles,			0x1d),
	XGENE_PMU_EVENT_ATTR(PA-total-CP-snoops,		0x20),
	XGENE_PMU_EVENT_ATTR(PA-rd-shared-snoop,		0x21),
	XGENE_PMU_EVENT_ATTR(PA-rd-shared-snoop-hit,		0x22),
	XGENE_PMU_EVENT_ATTR(PA-rd-exclusive-snoop,		0x23),
	XGENE_PMU_EVENT_ATTR(PA-rd-exclusive-snoop-hit,		0x24),
	XGENE_PMU_EVENT_ATTR(PA-rd-wr-invalid-snoop,		0x25),
	XGENE_PMU_EVENT_ATTR(PA-rd-wr-invalid-snoop-hit,	0x26),
	XGENE_PMU_EVENT_ATTR(PA-req-buffer-full,		0x28),
	XGENE_PMU_EVENT_ATTR(Cswlf-outbound-req-fifo-full,	0x29),
	XGENE_PMU_EVENT_ATTR(Cswlf-inbound-snoop-fifo-backpressure, 0x2a),
	XGENE_PMU_EVENT_ATTR(Cswlf-outbound-lack-fifo-full,	0x2b),
	XGENE_PMU_EVENT_ATTR(Cswlf-inbound-gack-fifo-backpressure, 0x2c),
	XGENE_PMU_EVENT_ATTR(Cswlf-outbound-data-fifo-full,	0x2d),
	XGENE_PMU_EVENT_ATTR(Cswlf-inbound-data-fifo-backpressure, 0x2e),
	XGENE_PMU_EVENT_ATTR(Cswlf-inbound-req-backpressure,	0x2f),
	NULL,
};

static struct attribute *iob_slow_pmu_v3_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(PA-AXI0-rd-req,			0x01),
	XGENE_PMU_EVENT_ATTR(PA-AXI0-wr-req,			0x02),
	XGENE_PMU_EVENT_ATTR(PA-AXI1-rd-req,			0x03),
	XGENE_PMU_EVENT_ATTR(PA-AXI1-wr-req,			0x04),
	XGENE_PMU_EVENT_ATTR(BA-all-AXI-req,			0x07),
	XGENE_PMU_EVENT_ATTR(BA-AXI-rd-req,			0x08),
	XGENE_PMU_EVENT_ATTR(BA-AXI-wr-req,			0x09),
	XGENE_PMU_EVENT_ATTR(BA-free-list-empty,		0x10),
	NULL,
};

static struct attribute *mcb_pmu_v3_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(req-receive,			0x01),
	XGENE_PMU_EVENT_ATTR(rd-req-recv,			0x02),
	XGENE_PMU_EVENT_ATTR(rd-req-recv-2,			0x03),
	XGENE_PMU_EVENT_ATTR(wr-req-recv,			0x04),
	XGENE_PMU_EVENT_ATTR(wr-req-recv-2,			0x05),
	XGENE_PMU_EVENT_ATTR(rd-req-sent-to-mcu,		0x06),
	XGENE_PMU_EVENT_ATTR(rd-req-sent-to-mcu-2,		0x07),
	XGENE_PMU_EVENT_ATTR(rd-req-sent-to-spec-mcu,		0x08),
	XGENE_PMU_EVENT_ATTR(rd-req-sent-to-spec-mcu-2,		0x09),
	XGENE_PMU_EVENT_ATTR(glbl-ack-recv-for-rd-sent-to-spec-mcu, 0x0a),
	XGENE_PMU_EVENT_ATTR(glbl-ack-go-recv-for-rd-sent-to-spec-mcu, 0x0b),
	XGENE_PMU_EVENT_ATTR(glbl-ack-nogo-recv-for-rd-sent-to-spec-mcu, 0x0c),
	XGENE_PMU_EVENT_ATTR(glbl-ack-go-recv-any-rd-req,	0x0d),
	XGENE_PMU_EVENT_ATTR(glbl-ack-go-recv-any-rd-req-2,	0x0e),
	XGENE_PMU_EVENT_ATTR(wr-req-sent-to-mcu,		0x0f),
	XGENE_PMU_EVENT_ATTR(gack-recv,				0x10),
	XGENE_PMU_EVENT_ATTR(rd-gack-recv,			0x11),
	XGENE_PMU_EVENT_ATTR(wr-gack-recv,			0x12),
	XGENE_PMU_EVENT_ATTR(cancel-rd-gack,			0x13),
	XGENE_PMU_EVENT_ATTR(cancel-wr-gack,			0x14),
	XGENE_PMU_EVENT_ATTR(mcb-csw-req-stall,			0x15),
	XGENE_PMU_EVENT_ATTR(mcu-req-intf-blocked,		0x16),
	XGENE_PMU_EVENT_ATTR(mcb-mcu-rd-intf-stall,		0x17),
	XGENE_PMU_EVENT_ATTR(csw-rd-intf-blocked,		0x18),
	XGENE_PMU_EVENT_ATTR(csw-local-ack-intf-blocked,	0x19),
	XGENE_PMU_EVENT_ATTR(mcu-req-table-full,		0x1a),
	XGENE_PMU_EVENT_ATTR(mcu-stat-table-full,		0x1b),
	XGENE_PMU_EVENT_ATTR(mcu-wr-table-full,			0x1c),
	XGENE_PMU_EVENT_ATTR(mcu-rdreceipt-resp,		0x1d),
	XGENE_PMU_EVENT_ATTR(mcu-wrcomplete-resp,		0x1e),
	XGENE_PMU_EVENT_ATTR(mcu-retryack-resp,			0x1f),
	XGENE_PMU_EVENT_ATTR(mcu-pcrdgrant-resp,		0x20),
	XGENE_PMU_EVENT_ATTR(mcu-req-from-lastload,		0x21),
	XGENE_PMU_EVENT_ATTR(mcu-req-from-bypass,		0x22),
	XGENE_PMU_EVENT_ATTR(volt-droop-detect,			0x23),
	NULL,
};

static struct attribute *mc_pmu_v3_events_attrs[] = {
	XGENE_PMU_EVENT_ATTR(cycle-count,			0x00),
	XGENE_PMU_EVENT_ATTR(pmu-act-sent,			0x01),
	XGENE_PMU_EVENT_ATTR(pmu-pre-sent,			0x02),
	XGENE_PMU_EVENT_ATTR(pmu-rd-sent,			0x03),
	XGENE_PMU_EVENT_ATTR(pmu-rda-sent,			0x04),
	XGENE_PMU_EVENT_ATTR(pmu-wr-sent,			0x05),
	XGENE_PMU_EVENT_ATTR(pmu-wra-sent,			0x06),
	XGENE_PMU_EVENT_ATTR(pmu-pd-entry-vld,			0x07),
	XGENE_PMU_EVENT_ATTR(pmu-sref-entry-vld,		0x08),
	XGENE_PMU_EVENT_ATTR(pmu-prea-sent,			0x09),
	XGENE_PMU_EVENT_ATTR(pmu-ref-sent,			0x0a),
	XGENE_PMU_EVENT_ATTR(pmu-rd-rda-sent,			0x0b),
	XGENE_PMU_EVENT_ATTR(pmu-wr-wra-sent,			0x0c),
	XGENE_PMU_EVENT_ATTR(pmu-raw-hazard,			0x0d),
	XGENE_PMU_EVENT_ATTR(pmu-war-hazard,			0x0e),
	XGENE_PMU_EVENT_ATTR(pmu-waw-hazard,			0x0f),
	XGENE_PMU_EVENT_ATTR(pmu-rar-hazard,			0x10),
	XGENE_PMU_EVENT_ATTR(pmu-raw-war-waw-hazard,		0x11),
	XGENE_PMU_EVENT_ATTR(pmu-hprd-lprd-wr-req-vld,		0x12),
	XGENE_PMU_EVENT_ATTR(pmu-lprd-req-vld,			0x13),
	XGENE_PMU_EVENT_ATTR(pmu-hprd-req-vld,			0x14),
	XGENE_PMU_EVENT_ATTR(pmu-hprd-lprd-req-vld,		0x15),
	XGENE_PMU_EVENT_ATTR(pmu-wr-req-vld,			0x16),
	XGENE_PMU_EVENT_ATTR(pmu-partial-wr-req-vld,		0x17),
	XGENE_PMU_EVENT_ATTR(pmu-rd-retry,			0x18),
	XGENE_PMU_EVENT_ATTR(pmu-wr-retry,			0x19),
	XGENE_PMU_EVENT_ATTR(pmu-retry-gnt,			0x1a),
	XGENE_PMU_EVENT_ATTR(pmu-rank-change,			0x1b),
	XGENE_PMU_EVENT_ATTR(pmu-dir-change,			0x1c),
	XGENE_PMU_EVENT_ATTR(pmu-rank-dir-change,		0x1d),
	XGENE_PMU_EVENT_ATTR(pmu-rank-active,			0x1e),
	XGENE_PMU_EVENT_ATTR(pmu-rank-idle,			0x1f),
	XGENE_PMU_EVENT_ATTR(pmu-rank-pd,			0x20),
	XGENE_PMU_EVENT_ATTR(pmu-rank-sref,			0x21),
	XGENE_PMU_EVENT_ATTR(pmu-queue-fill-gt-thresh,		0x22),
	XGENE_PMU_EVENT_ATTR(pmu-queue-rds-gt-thresh,		0x23),
	XGENE_PMU_EVENT_ATTR(pmu-queue-wrs-gt-thresh,		0x24),
	XGENE_PMU_EVENT_ATTR(pmu-phy-updt-complt,		0x25),
	XGENE_PMU_EVENT_ATTR(pmu-tz-fail,			0x26),
	XGENE_PMU_EVENT_ATTR(pmu-dram-errc,			0x27),
	XGENE_PMU_EVENT_ATTR(pmu-dram-errd,			0x28),
	XGENE_PMU_EVENT_ATTR(pmu-rd-enq,			0x29),
	XGENE_PMU_EVENT_ATTR(pmu-wr-enq,			0x2a),
	XGENE_PMU_EVENT_ATTR(pmu-tmac-limit-reached,		0x2b),
	XGENE_PMU_EVENT_ATTR(pmu-tmaw-tracker-full,		0x2c),
	NULL,
};

static const struct attribute_group l3c_pmu_v3_events_attr_group = {
	.name = "events",
	.attrs = l3c_pmu_v3_events_attrs,
};

static const struct attribute_group iob_fast_pmu_v3_events_attr_group = {
	.name = "events",
	.attrs = iob_fast_pmu_v3_events_attrs,
};

static const struct attribute_group iob_slow_pmu_v3_events_attr_group = {
	.name = "events",
	.attrs = iob_slow_pmu_v3_events_attrs,
};

static const struct attribute_group mcb_pmu_v3_events_attr_group = {
	.name = "events",
	.attrs = mcb_pmu_v3_events_attrs,
};

static const struct attribute_group mc_pmu_v3_events_attr_group = {
	.name = "events",
	.attrs = mc_pmu_v3_events_attrs,
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

static struct attribute *xgene_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group pmu_cpumask_attr_group = {
	.attrs = xgene_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups of PMU v1 and v2
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

/*
 * Per PMU device attribute groups of PMU v3
 */
static const struct attribute_group *l3c_pmu_v3_attr_groups[] = {
	&l3c_pmu_v3_format_attr_group,
	&pmu_cpumask_attr_group,
	&l3c_pmu_v3_events_attr_group,
	NULL
};

static const struct attribute_group *iob_fast_pmu_v3_attr_groups[] = {
	&iob_pmu_v3_format_attr_group,
	&pmu_cpumask_attr_group,
	&iob_fast_pmu_v3_events_attr_group,
	NULL
};

static const struct attribute_group *iob_slow_pmu_v3_attr_groups[] = {
	&iob_slow_pmu_v3_format_attr_group,
	&pmu_cpumask_attr_group,
	&iob_slow_pmu_v3_events_attr_group,
	NULL
};

static const struct attribute_group *mcb_pmu_v3_attr_groups[] = {
	&mcb_pmu_v3_format_attr_group,
	&pmu_cpumask_attr_group,
	&mcb_pmu_v3_events_attr_group,
	NULL
};

static const struct attribute_group *mc_pmu_v3_attr_groups[] = {
	&mc_pmu_v3_format_attr_group,
	&pmu_cpumask_attr_group,
	&mc_pmu_v3_events_attr_group,
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
	if (xgene_pmu->version == PCP_PMU_V3) {
		writel(PCPPMU_V3_INTENMASK,
		       xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
	} else {
		writel(PCPPMU_INTENMASK,
		       xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
	}
}

static inline void xgene_pmu_unmask_int(struct xgene_pmu *xgene_pmu)
{
	if (xgene_pmu->version == PCP_PMU_V3) {
		writel(PCPPMU_V3_INTCLRMASK,
		       xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
	} else {
		writel(PCPPMU_INTCLRMASK,
		       xgene_pmu->pcppmu_csr + PCPPMU_INTMASK_REG);
	}
}

static inline u64 xgene_pmu_read_counter(struct xgene_pmu_dev *pmu_dev, int idx)
{
	u32 cnt_lo, cnt_hi, cnt_hi2;

	if (pmu_dev->parent->version == PCP_PMU_V3) {
		/*
		 * v3 has 64-bit counter registers composed by 2 32-bit registers
		 * This can be a problem if the counter increases and carries
		 * out of bit [31] between 2 reads. The extra reads would help
		 * to prevent this issue.
		 */
		while (1) {
			cnt_hi = readl(pmu_dev->inf->csr + PMU_PMEVCNTR0 + 4 + (8 * idx));
			cnt_lo = readl(pmu_dev->inf->csr + PMU_PMEVCNTR0 + (8 * idx));
			cnt_hi2 = readl(pmu_dev->inf->csr + PMU_PMEVCNTR0 + 4 + (8 * idx));
			if (cnt_hi == cnt_hi2)
				return (((u64)cnt_hi << 32) | cnt_lo);
		}
	}

	return ((u64)readl(pmu_dev->inf->csr + PMU_PMEVCNTR0 + (4 * idx)));
}

static inline void
xgene_pmu_write_counter(struct xgene_pmu_dev *pmu_dev, int idx, u64 val)
{
	u32 cnt_lo, cnt_hi;

	cnt_lo = val & 0xFFFFFFFF;
	cnt_hi = val >> 32;

	if (pmu_dev->parent->version == PCP_PMU_V3) {
		/* v3 has 64-bit counter registers composed by 2 32-bit registers */
		writel(cnt_lo, pmu_dev->inf->csr + PMU_PMEVCNTR0 + (8 * idx));
		writel(cnt_hi, pmu_dev->inf->csr + PMU_PMEVCNTR0 + 4 + (8 * idx));
	} else {
		writel(cnt_lo, pmu_dev->inf->csr + PMU_PMEVCNTR0 + (4 * idx));
	}
}

static inline void
xgene_pmu_write_evttype(struct xgene_pmu_dev *pmu_dev, int idx, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMEVTYPER0 + (4 * idx));
}

static inline void
xgene_pmu_write_agentmsk(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAMR0);
}

static inline void
xgene_pmu_write_agent1msk(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAMR1);
}

static inline void
xgene_pmu_v3_write_agentmsk(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAUXR0);
}

static inline void
xgene_pmu_v3_write_agent1msk(struct xgene_pmu_dev *pmu_dev, u32 val)
{
	writel(val, pmu_dev->inf->csr + PMU_PMAUXR1);
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

static void xgene_perf_pmu_enable(struct pmu *pmu)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(pmu);
	int enabled = bitmap_weight(pmu_dev->cntr_assign_mask,
			pmu_dev->max_counters);

	if (!enabled)
		return;

	xgene_pmu_start_counters(pmu_dev);
}

static void xgene_perf_pmu_disable(struct pmu *pmu)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(pmu);

	xgene_pmu_stop_counters(pmu_dev);
}

static int xgene_perf_event_init(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;
	struct perf_event *sibling;

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

	hw->config = event->attr.config;
	/*
	 * Each bit of the config1 field represents an agent from which the
	 * request of the event come. The event is counted only if it's caused
	 * by a request of an agent has the bit cleared.
	 * By default, the event is counted for all agents.
	 */
	if (pmu_dev->parent->version == PCP_PMU_V3 && !event->attr.config1)
		hw->config_base = pmu_dev->default_agentid;
	else
		hw->config_base = event->attr.config1;

	/*
	 * We must NOT create groups containing mixed PMUs, although software
	 * events are acceptable
	 */
	if (event->group_leader->pmu != event->pmu &&
			!is_software_event(event->group_leader))
		return -EINVAL;

	list_for_each_entry(sibling, &event->group_leader->sibling_list,
			group_entry)
		if (sibling->pmu != event->pmu &&
				!is_software_event(sibling))
			return -EINVAL;

	return 0;
}

static void xgene_pmu_select_agent(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_write_agentmsk(pmu_dev, ~((u32)GET_AGENTID(event)));
	if (pmu_dev->inf->type == PMU_TYPE_IOB)
		xgene_pmu_write_agent1msk(pmu_dev, ~((u32)GET_AGENT1ID(event)));
}

static void xgene_pmu_v3_select_agent(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_v3_write_agentmsk(pmu_dev, (u32)GET_AGENTID(event));
	if (pmu_dev->inf->type == PMU_TYPE_MCB ||
	    pmu_dev->inf->type == PMU_TYPE_IOB ||
	    pmu_dev->inf->type == PMU_TYPE_IOB_SLOW)
		xgene_pmu_v3_write_agent1msk(pmu_dev, (u32)GET_AGENT1ID(event));
}

static void xgene_perf_enable_event(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_write_evttype(pmu_dev, GET_CNTR(event), GET_EVENTID(event));
	if (pmu_dev->parent->version == PCP_PMU_V3)
		xgene_pmu_v3_select_agent(event);
	else
		xgene_pmu_select_agent(event);

	xgene_pmu_enable_counter(pmu_dev, GET_CNTR(event));
	xgene_pmu_enable_counter_int(pmu_dev, GET_CNTR(event));
}

static void xgene_perf_disable_event(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);

	xgene_pmu_disable_counter(pmu_dev, GET_CNTR(event));
	xgene_pmu_disable_counter_int(pmu_dev, GET_CNTR(event));
}

static void xgene_perf_event_set_period(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;
	/*
	 * The X-Gene PMU counters have a period of 2^32 or more. To account for the
	 * possiblity of extreme interrupt latency we program for a period of
	 * half that. Hopefully we can handle the interrupt before another 2^31
	 * events occur and the counter overtakes its previous value.
	 */
	u64 val = 1ULL << 31;

	local64_set(&hw->prev_count, val);
	xgene_pmu_write_counter(pmu_dev, hw->idx, val);
}

static void xgene_perf_event_update(struct perf_event *event)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;
	u64 delta, prev_raw_count, new_raw_count;

again:
	prev_raw_count = local64_read(&hw->prev_count);
	new_raw_count = xgene_pmu_read_counter(pmu_dev, GET_CNTR(event));

	if (local64_cmpxchg(&hw->prev_count, prev_raw_count,
			    new_raw_count) != prev_raw_count)
		goto again;

	delta = (new_raw_count - prev_raw_count) & pmu_dev->max_period;

	local64_add(delta, &event->count);
}

static void xgene_perf_read(struct perf_event *event)
{
	xgene_perf_event_update(event);
}

static void xgene_perf_start(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;

	if (WARN_ON_ONCE(!(hw->state & PERF_HES_STOPPED)))
		return;

	WARN_ON_ONCE(!(hw->state & PERF_HES_UPTODATE));
	hw->state = 0;

	xgene_perf_event_set_period(event);

	if (flags & PERF_EF_RELOAD) {
		u64 prev_raw_count =  local64_read(&hw->prev_count);

		xgene_pmu_write_counter(pmu_dev, GET_CNTR(event),
					prev_raw_count);
	}

	xgene_perf_enable_event(event);
	perf_event_update_userpage(event);
}

static void xgene_perf_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hw = &event->hw;
	u64 config;

	if (hw->state & PERF_HES_UPTODATE)
		return;

	xgene_perf_disable_event(event);
	WARN_ON_ONCE(hw->state & PERF_HES_STOPPED);
	hw->state |= PERF_HES_STOPPED;

	if (hw->state & PERF_HES_UPTODATE)
		return;

	config = hw->config;
	xgene_perf_read(event);
	hw->state |= PERF_HES_UPTODATE;
}

static int xgene_perf_add(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;

	hw->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	/* Allocate an event counter */
	hw->idx = get_next_avail_cntr(pmu_dev);
	if (hw->idx < 0)
		return -EAGAIN;

	/* Update counter event pointer for Interrupt handler */
	pmu_dev->pmu_counter_event[hw->idx] = event;

	if (flags & PERF_EF_START)
		xgene_perf_start(event, PERF_EF_RELOAD);

	return 0;
}

static void xgene_perf_del(struct perf_event *event, int flags)
{
	struct xgene_pmu_dev *pmu_dev = to_pmu_dev(event->pmu);
	struct hw_perf_event *hw = &event->hw;

	xgene_perf_stop(event, PERF_EF_UPDATE);

	/* clear the assigned counter */
	clear_avail_cntr(pmu_dev, GET_CNTR(event));

	perf_event_update_userpage(event);
	pmu_dev->pmu_counter_event[hw->idx] = NULL;
}

static int xgene_init_perf(struct xgene_pmu_dev *pmu_dev, char *name)
{
	struct xgene_pmu *xgene_pmu;

	if (pmu_dev->parent->version == PCP_PMU_V3)
		pmu_dev->max_period = PMU_V3_CNT_MAX_PERIOD;
	else
		pmu_dev->max_period = PMU_CNT_MAX_PERIOD;
	/* First version PMU supports only single event counter */
	xgene_pmu = pmu_dev->parent;
	if (xgene_pmu->version == PCP_PMU_V1)
		pmu_dev->max_counters = 1;
	else
		pmu_dev->max_counters = PMU_MAX_COUNTERS;

	/* Perf driver registration */
	pmu_dev->pmu = (struct pmu) {
		.attr_groups	= pmu_dev->attr_groups,
		.task_ctx_nr	= perf_invalid_context,
		.pmu_enable	= xgene_perf_pmu_enable,
		.pmu_disable	= xgene_perf_pmu_disable,
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
		if (!(xgene_pmu->l3c_active_mask & pmu->inf->enable_mask))
			goto dev_err;
		if (xgene_pmu->version == PCP_PMU_V3) {
			pmu->attr_groups = l3c_pmu_v3_attr_groups;
			pmu->default_agentid = PCPPMU_V3_L3C_DEFAULT_AGENTID;
		} else {
			pmu->attr_groups = l3c_pmu_attr_groups;
		}
		break;
	case PMU_TYPE_IOB:
		if (xgene_pmu->version == PCP_PMU_V3) {
			pmu->attr_groups = iob_fast_pmu_v3_attr_groups;
			pmu->default_agentid = PCPPMU_V3_IOB_DEFAULT_AGENTID;
		} else {
			pmu->attr_groups = iob_pmu_attr_groups;
		}
		break;
	case PMU_TYPE_IOB_SLOW:
		if (xgene_pmu->version == PCP_PMU_V3) {
			pmu->attr_groups = iob_slow_pmu_v3_attr_groups;
			pmu->default_agentid = PCPPMU_V3_IOB_SLOW_DEFAULT_AGENTID;
		}
		break;
	case PMU_TYPE_MCB:
		if (!(xgene_pmu->mcb_active_mask & pmu->inf->enable_mask))
			goto dev_err;
		if (xgene_pmu->version == PCP_PMU_V3) {
			pmu->attr_groups = mcb_pmu_v3_attr_groups;
			pmu->default_agentid = PCPPMU_V3_MCB_DEFAULT_AGENTID;
		} else {
			pmu->attr_groups = mcb_pmu_attr_groups;
		}
		break;
	case PMU_TYPE_MC:
		if (!(xgene_pmu->mc_active_mask & pmu->inf->enable_mask))
			goto dev_err;
		if (xgene_pmu->version == PCP_PMU_V3) {
			pmu->attr_groups = mc_pmu_v3_attr_groups;
			pmu->default_agentid = PCPPMU_V3_MC_DEFAULT_AGENTID;
		} else {
			pmu->attr_groups = mc_pmu_attr_groups;
		}
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

static void _xgene_pmu_isr(int irq, struct xgene_pmu_dev *pmu_dev)
{
	struct xgene_pmu *xgene_pmu = pmu_dev->parent;
	u32 pmovsr;
	int idx;

	if (xgene_pmu->version == PCP_PMU_V3)
		pmovsr = readl(pmu_dev->inf->csr + PMU_PMOVSSET) & PMU_OVERFLOW_MASK;
	else
		pmovsr = readl(pmu_dev->inf->csr + PMU_PMOVSR) & PMU_OVERFLOW_MASK;

	if (!pmovsr)
		return;

	/* Clear interrupt flag */
	if (xgene_pmu->version == PCP_PMU_V1)
		writel(0x0, pmu_dev->inf->csr + PMU_PMOVSR);
	else if (xgene_pmu->version == PCP_PMU_V2)
		writel(pmovsr, pmu_dev->inf->csr + PMU_PMOVSR);
	else
		writel(pmovsr, pmu_dev->inf->csr + PMU_PMOVSCLR);

	for (idx = 0; idx < PMU_MAX_COUNTERS; idx++) {
		struct perf_event *event = pmu_dev->pmu_counter_event[idx];
		int overflowed = pmovsr & BIT(idx);

		/* Ignore if we don't have an event. */
		if (!event || !overflowed)
			continue;
		xgene_perf_event_update(event);
		xgene_perf_event_set_period(event);
	}
}

static irqreturn_t xgene_pmu_isr(int irq, void *dev_id)
{
	u32 intr_mcu, intr_mcb, intr_l3c, intr_iob;
	struct xgene_pmu_dev_ctx *ctx;
	struct xgene_pmu *xgene_pmu = dev_id;
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&xgene_pmu->lock, flags);

	/* Get Interrupt PMU source */
	val = readl(xgene_pmu->pcppmu_csr + PCPPMU_INTSTATUS_REG);
	if (xgene_pmu->version == PCP_PMU_V3) {
		intr_mcu = PCPPMU_V3_INT_MCU;
		intr_mcb = PCPPMU_V3_INT_MCB;
		intr_l3c = PCPPMU_V3_INT_L3C;
		intr_iob = PCPPMU_V3_INT_IOB;
	} else {
		intr_mcu = PCPPMU_INT_MCU;
		intr_mcb = PCPPMU_INT_MCB;
		intr_l3c = PCPPMU_INT_L3C;
		intr_iob = PCPPMU_INT_IOB;
	}
	if (val & intr_mcu) {
		list_for_each_entry(ctx, &xgene_pmu->mcpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & intr_mcb) {
		list_for_each_entry(ctx, &xgene_pmu->mcbpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & intr_l3c) {
		list_for_each_entry(ctx, &xgene_pmu->l3cpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}
	if (val & intr_iob) {
		list_for_each_entry(ctx, &xgene_pmu->iobpmus, next) {
			_xgene_pmu_isr(irq, ctx->pmu_dev);
		}
	}

	raw_spin_unlock_irqrestore(&xgene_pmu->lock, flags);

	return IRQ_HANDLED;
}

static int acpi_pmu_probe_active_mcb_mcu_l3c(struct xgene_pmu *xgene_pmu,
					 struct platform_device *pdev)
{
	void __iomem *csw_csr, *mcba_csr, *mcbb_csr;
	struct resource *res;
	unsigned int reg;
	u32 mcb0routing;
	u32 mcb1routing;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	csw_csr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(csw_csr)) {
		dev_err(&pdev->dev, "ioremap failed for CSW CSR resource\n");
		return PTR_ERR(csw_csr);
	}

	reg = readl(csw_csr + CSW_CSWCR);
	if (xgene_pmu->version == PCP_PMU_V3) {
		mcb0routing = CSW_CSWCR_MCB0_ROUTING(reg);
		mcb1routing = CSW_CSWCR_MCB0_ROUTING(reg);
		if (reg & CSW_CSWCR_DUALMCB_MASK) {
			xgene_pmu->mcb_active_mask = 0x3;
			xgene_pmu->l3c_active_mask = 0xFF;
			if ((mcb0routing == 0x2) && (mcb1routing == 0x2))
				xgene_pmu->mc_active_mask = 0xFF;
			else if ((mcb0routing == 0x1) && (mcb1routing == 0x1))
				xgene_pmu->mc_active_mask =  0x33;
			else
				xgene_pmu->mc_active_mask =  0x11;

		} else {
			xgene_pmu->mcb_active_mask = 0x1;
			xgene_pmu->l3c_active_mask = 0x0F;
			if ((mcb0routing == 0x2) && (mcb1routing == 0x0))
				xgene_pmu->mc_active_mask = 0x0F;
			else if ((mcb0routing == 0x1) && (mcb1routing == 0x0))
				xgene_pmu->mc_active_mask =  0x03;
			else
				xgene_pmu->mc_active_mask =  0x01;
		}
	} else {
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

		xgene_pmu->l3c_active_mask = 0x1;
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
	}
	return 0;
}

static int fdt_pmu_probe_active_mcb_mcu_l3c(struct xgene_pmu *xgene_pmu,
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

	xgene_pmu->l3c_active_mask = 0x1;
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
		return acpi_pmu_probe_active_mcb_mcu_l3c(xgene_pmu, pdev);
	return fdt_pmu_probe_active_mcb_mcu_l3c(xgene_pmu, pdev);
}

static char *xgene_pmu_dev_name(struct device *dev, u32 type, int id)
{
	switch (type) {
	case PMU_TYPE_L3C:
		return devm_kasprintf(dev, GFP_KERNEL, "l3c%d", id);
	case PMU_TYPE_IOB:
		return devm_kasprintf(dev, GFP_KERNEL, "iob%d", id);
	case PMU_TYPE_IOB_SLOW:
		return devm_kasprintf(dev, GFP_KERNEL, "iob-slow%d", id);
	case PMU_TYPE_MCB:
		return devm_kasprintf(dev, GFP_KERNEL, "mcb%d", id);
	case PMU_TYPE_MC:
		return devm_kasprintf(dev, GFP_KERNEL, "mc%d", id);
	default:
		return devm_kasprintf(dev, GFP_KERNEL, "unknown");
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
	if (rc < 0) {
		dev_err(dev, "PMU type %d: No resource address found\n", type);
		goto err;
	}

	dev_csr = devm_ioremap_resource(dev, &res);
	if (IS_ERR(dev_csr)) {
		dev_err(dev, "PMU type %d: Fail to map resource\n", type);
		goto err;
	}

	/* A PMU device node without enable-bit-index is always enabled */
	rc = acpi_dev_get_property(adev, "enable-bit-index",
				   ACPI_TYPE_INTEGER, &obj);
	if (rc < 0)
		enable_bit = 0;
	else
		enable_bit = (int) obj->integer.value;

	ctx->name = xgene_pmu_dev_name(dev, type, enable_bit);
	if (!ctx->name) {
		dev_err(dev, "PMU type %d: Fail to get device name\n", type);
		goto err;
	}
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

	if ((!strcmp(acpi_device_hid(adev), "APMC0D5D")) ||
	    (!strcmp(acpi_device_hid(adev), "APMC0D84")))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_L3C);
	else if ((!strcmp(acpi_device_hid(adev), "APMC0D5E")) ||
		 (!strcmp(acpi_device_hid(adev), "APMC0D85")))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_IOB);
	else if (!strcmp(acpi_device_hid(adev), "APMC0D86"))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_IOB_SLOW);
	else if ((!strcmp(acpi_device_hid(adev), "APMC0D5F")) ||
		 (!strcmp(acpi_device_hid(adev), "APMC0D87")))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_MCB);
	else if ((!strcmp(acpi_device_hid(adev), "APMC0D60")) ||
		 (!strcmp(acpi_device_hid(adev), "APMC0D88")))
		ctx = acpi_get_pmu_hw_inf(xgene_pmu, adev, PMU_TYPE_MC);
	else
		ctx = NULL;

	if (!ctx)
		return AE_OK;

	if (xgene_pmu_dev_add(xgene_pmu, ctx)) {
		/* Can't add the PMU device, skip it */
		devm_kfree(xgene_pmu->dev, ctx);
		return AE_OK;
	}

	switch (ctx->inf.type) {
	case PMU_TYPE_L3C:
		list_add(&ctx->next, &xgene_pmu->l3cpmus);
		break;
	case PMU_TYPE_IOB:
		list_add(&ctx->next, &xgene_pmu->iobpmus);
		break;
	case PMU_TYPE_IOB_SLOW:
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
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "failed to probe PMU devices\n");
		return -ENODEV;
	}

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
		goto err;
	}

	/* A PMU device node without enable-bit-index is always enabled */
	if (of_property_read_u32(np, "enable-bit-index", &enable_bit))
		enable_bit = 0;

	ctx->name = xgene_pmu_dev_name(dev, type, enable_bit);
	if (!ctx->name) {
		dev_err(dev, "PMU type %d: Fail to get device name\n", type);
		goto err;
	}
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

		if (xgene_pmu_dev_add(xgene_pmu, ctx)) {
			/* Can't add the PMU device, skip it */
			devm_kfree(xgene_pmu->dev, ctx);
			continue;
		}

		switch (ctx->inf.type) {
		case PMU_TYPE_L3C:
			list_add(&ctx->next, &xgene_pmu->l3cpmus);
			break;
		case PMU_TYPE_IOB:
			list_add(&ctx->next, &xgene_pmu->iobpmus);
			break;
		case PMU_TYPE_IOB_SLOW:
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
};

static const struct xgene_pmu_data xgene_pmu_v2_data = {
	.id   = PCP_PMU_V2,
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
	{"APMC0D83", PCP_PMU_V3},
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
	int irq, rc;
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
	rc = devm_request_irq(&pdev->dev, irq, xgene_pmu_isr,
				IRQF_NOBALANCING | IRQF_NO_THREAD,
				dev_name(&pdev->dev), xgene_pmu);
	if (rc) {
		dev_err(&pdev->dev, "Could not request IRQ %d\n", irq);
		goto err;
	}

	raw_spin_lock_init(&xgene_pmu->lock);

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

	/* Walk through the tree for all PMU perf devices */
	rc = xgene_pmu_probe_pmu_dev(xgene_pmu, pdev);
	if (rc) {
		dev_err(&pdev->dev, "No PMU perf devices found!\n");
		goto err;
	}

	/* Enable interrupt */
	xgene_pmu_unmask_int(xgene_pmu);

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
	struct xgene_pmu_dev_ctx *ctx;
	struct device *dev = xgene_pmu->dev;
	struct xgene_pmu_dev *pmu_dev;

	list_for_each_entry(ctx, pmus, next) {
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

builtin_platform_driver(xgene_pmu_driver);
