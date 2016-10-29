/*
 * Cavium Thunder uncore PMU support,
 * CCPI interface controller (OCX) Transmit link (TLK) counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/perf_event.h>
#include <linux/slab.h>

#include "uncore_cavium.h"

struct thunder_uncore *thunder_uncore_ocx_tlk;

#define OCX_TLK_NR_UNITS			3
#define OCX_TLK_UNIT_OFFSET			0x2000
#define OCX_TLK_STAT_CTL			0x10040
#define OCX_TLK_STAT_OFFSET			0x10400

#define OCX_TLK_STAT_ENABLE_BIT			BIT_ULL(0)
#define OCX_TLK_STAT_RESET_BIT			BIT_ULL(1)

/* OCX TLK event list */
#define OCX_TLK_EVENT_STAT_IDLE_CNT		0x00
#define OCX_TLK_EVENT_STAT_DATA_CNT		0x08
#define OCX_TLK_EVENT_STAT_SYNC_CNT		0x10
#define OCX_TLK_EVENT_STAT_RETRY_CNT		0x18
#define OCX_TLK_EVENT_STAT_ERR_CNT		0x20
#define OCX_TLK_EVENT_STAT_MAT0_CNT		0x40
#define OCX_TLK_EVENT_STAT_MAT1_CNT		0x48
#define OCX_TLK_EVENT_STAT_MAT2_CNT		0x50
#define OCX_TLK_EVENT_STAT_MAT3_CNT		0x58
#define OCX_TLK_EVENT_STAT_VC0_CMD		0x80
#define OCX_TLK_EVENT_STAT_VC1_CMD		0x88
#define OCX_TLK_EVENT_STAT_VC2_CMD		0x90
#define OCX_TLK_EVENT_STAT_VC3_CMD		0x98
#define OCX_TLK_EVENT_STAT_VC4_CMD		0xa0
#define OCX_TLK_EVENT_STAT_VC5_CMD		0xa8
#define OCX_TLK_EVENT_STAT_VC0_PKT		0x100
#define OCX_TLK_EVENT_STAT_VC1_PKT		0x108
#define OCX_TLK_EVENT_STAT_VC2_PKT		0x110
#define OCX_TLK_EVENT_STAT_VC3_PKT		0x118
#define OCX_TLK_EVENT_STAT_VC4_PKT		0x120
#define OCX_TLK_EVENT_STAT_VC5_PKT		0x128
#define OCX_TLK_EVENT_STAT_VC6_PKT		0x130
#define OCX_TLK_EVENT_STAT_VC7_PKT		0x138
#define OCX_TLK_EVENT_STAT_VC8_PKT		0x140
#define OCX_TLK_EVENT_STAT_VC9_PKT		0x148
#define OCX_TLK_EVENT_STAT_VC10_PKT		0x150
#define OCX_TLK_EVENT_STAT_VC11_PKT		0x158
#define OCX_TLK_EVENT_STAT_VC12_PKT		0x160
#define OCX_TLK_EVENT_STAT_VC13_PKT		0x168
#define OCX_TLK_EVENT_STAT_VC0_CON		0x180
#define OCX_TLK_EVENT_STAT_VC1_CON		0x188
#define OCX_TLK_EVENT_STAT_VC2_CON		0x190
#define OCX_TLK_EVENT_STAT_VC3_CON		0x198
#define OCX_TLK_EVENT_STAT_VC4_CON		0x1a0
#define OCX_TLK_EVENT_STAT_VC5_CON		0x1a8
#define OCX_TLK_EVENT_STAT_VC6_CON		0x1b0
#define OCX_TLK_EVENT_STAT_VC7_CON		0x1b8
#define OCX_TLK_EVENT_STAT_VC8_CON		0x1c0
#define OCX_TLK_EVENT_STAT_VC9_CON		0x1c8
#define OCX_TLK_EVENT_STAT_VC10_CON		0x1d0
#define OCX_TLK_EVENT_STAT_VC11_CON		0x1d8
#define OCX_TLK_EVENT_STAT_VC12_CON		0x1e0
#define OCX_TLK_EVENT_STAT_VC13_CON		0x1e8

static int ocx_tlk_events[] = {
	OCX_TLK_EVENT_STAT_IDLE_CNT,
	OCX_TLK_EVENT_STAT_DATA_CNT,
	OCX_TLK_EVENT_STAT_SYNC_CNT,
	OCX_TLK_EVENT_STAT_RETRY_CNT,
	OCX_TLK_EVENT_STAT_ERR_CNT,
	OCX_TLK_EVENT_STAT_MAT0_CNT,
	OCX_TLK_EVENT_STAT_MAT1_CNT,
	OCX_TLK_EVENT_STAT_MAT2_CNT,
	OCX_TLK_EVENT_STAT_MAT3_CNT,
	OCX_TLK_EVENT_STAT_VC0_CMD,
	OCX_TLK_EVENT_STAT_VC1_CMD,
	OCX_TLK_EVENT_STAT_VC2_CMD,
	OCX_TLK_EVENT_STAT_VC3_CMD,
	OCX_TLK_EVENT_STAT_VC4_CMD,
	OCX_TLK_EVENT_STAT_VC5_CMD,
	OCX_TLK_EVENT_STAT_VC0_PKT,
	OCX_TLK_EVENT_STAT_VC1_PKT,
	OCX_TLK_EVENT_STAT_VC2_PKT,
	OCX_TLK_EVENT_STAT_VC3_PKT,
	OCX_TLK_EVENT_STAT_VC4_PKT,
	OCX_TLK_EVENT_STAT_VC5_PKT,
	OCX_TLK_EVENT_STAT_VC6_PKT,
	OCX_TLK_EVENT_STAT_VC7_PKT,
	OCX_TLK_EVENT_STAT_VC8_PKT,
	OCX_TLK_EVENT_STAT_VC9_PKT,
	OCX_TLK_EVENT_STAT_VC10_PKT,
	OCX_TLK_EVENT_STAT_VC11_PKT,
	OCX_TLK_EVENT_STAT_VC12_PKT,
	OCX_TLK_EVENT_STAT_VC13_PKT,
	OCX_TLK_EVENT_STAT_VC0_CON,
	OCX_TLK_EVENT_STAT_VC1_CON,
	OCX_TLK_EVENT_STAT_VC2_CON,
	OCX_TLK_EVENT_STAT_VC3_CON,
	OCX_TLK_EVENT_STAT_VC4_CON,
	OCX_TLK_EVENT_STAT_VC5_CON,
	OCX_TLK_EVENT_STAT_VC6_CON,
	OCX_TLK_EVENT_STAT_VC7_CON,
	OCX_TLK_EVENT_STAT_VC8_CON,
	OCX_TLK_EVENT_STAT_VC9_CON,
	OCX_TLK_EVENT_STAT_VC10_CON,
	OCX_TLK_EVENT_STAT_VC11_CON,
	OCX_TLK_EVENT_STAT_VC12_CON,
	OCX_TLK_EVENT_STAT_VC13_CON,
};

/*
 * The OCX devices have a single device per node, therefore picking the
 * first device from the list is correct.
 */
static inline void __iomem *map_offset(struct thunder_uncore_node *node,
				       unsigned long addr, int offset, int nr)
{
	struct thunder_uncore_unit *unit;

	unit = list_first_entry(&node->unit_list, struct thunder_uncore_unit,
				entry);
	return (void __iomem *)(addr + unit->map + nr * offset);
}

static void __iomem *map_offset_ocx_tlk(struct thunder_uncore_node *node,
					unsigned long addr, int nr)
{
	return (void __iomem *)map_offset(node, addr, nr, OCX_TLK_UNIT_OFFSET);
}

/*
 * The OCX TLK counters can only be enabled/disabled as a set so we do
 * this in pmu_enable/disable instead of start/stop.
 */
static void thunder_uncore_pmu_enable_ocx_tlk(struct pmu *pmu)
{
	struct thunder_uncore *uncore =
		container_of(pmu, struct thunder_uncore, pmu);
	int node = 0, i;

	while (uncore->nodes[node++]) {
		for (i = 0; i < OCX_TLK_NR_UNITS; i++) {
			/* reset all TLK counters to zero */
			writeb(OCX_TLK_STAT_RESET_BIT,
			       map_offset_ocx_tlk(uncore->nodes[node],
						  OCX_TLK_STAT_CTL, i));
			/* enable all TLK counters */
			writeb(OCX_TLK_STAT_ENABLE_BIT,
			       map_offset_ocx_tlk(uncore->nodes[node],
						  OCX_TLK_STAT_CTL, i));
		}
	}
}

/*
 * The OCX TLK counters can only be enabled/disabled as a set so we do
 * this in pmu_enable/disable instead of start/stop.
 */
static void thunder_uncore_pmu_disable_ocx_tlk(struct pmu *pmu)
{
	struct thunder_uncore *uncore =
		container_of(pmu, struct thunder_uncore, pmu);
	int node = 0, i;

	while (uncore->nodes[node++]) {
		for (i = 0; i < OCX_TLK_NR_UNITS; i++) {
			/* disable all TLK counters */
			writeb(0, map_offset_ocx_tlk(uncore->nodes[node],
						     OCX_TLK_STAT_CTL, i));
		}
	}
}

/*
 * Summarize counters across all TLK's. Different from the other uncore
 * PMUs because all TLK's are on one PCI device.
 */
static void thunder_uncore_read_ocx_tlk(struct perf_event *event)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	u64 new = 0;
	int i;

	/* read counter values from all units */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < OCX_TLK_NR_UNITS; i++)
		new += readq(map_offset_ocx_tlk(node, hwc->event_base, i));

	local64_add(new, &hwc->prev_count);
	local64_add(new, &event->count);
}

static void thunder_uncore_start_ocx_tlk(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	u64 new = 0;
	int i;

	/* read counter values from all units on the node */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < OCX_TLK_NR_UNITS; i++)
		new += readq(map_offset_ocx_tlk(node, hwc->event_base, i));
	local64_set(&hwc->prev_count, new);

	hwc->state = 0;
	perf_event_update_userpage(event);
}

static int thunder_uncore_add_ocx_tlk(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	return thunder_uncore_add(event, flags,
				  OCX_TLK_STAT_CTL,
				  OCX_TLK_STAT_OFFSET + ocx_tlk_events[get_id(hwc->config)]);
}

PMU_FORMAT_ATTR(event, "config:0-5");

static struct attribute *thunder_ocx_tlk_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_ocx_tlk_format_group = {
	.name = "format",
	.attrs = thunder_ocx_tlk_format_attr,
};

static struct attribute *thunder_ocx_tlk_events_attr[] = {
	UC_EVENT_ENTRY(idle_cnt,	0),
	UC_EVENT_ENTRY(data_cnt,	1),
	UC_EVENT_ENTRY(sync_cnt,	2),
	UC_EVENT_ENTRY(retry_cnt,	3),
	UC_EVENT_ENTRY(err_cnt,		4),
	UC_EVENT_ENTRY(mat0_cnt,	5),
	UC_EVENT_ENTRY(mat1_cnt,	6),
	UC_EVENT_ENTRY(mat2_cnt,	7),
	UC_EVENT_ENTRY(mat3_cnt,	8),
	UC_EVENT_ENTRY(vc0_cmd,		9),
	UC_EVENT_ENTRY(vc1_cmd,		10),
	UC_EVENT_ENTRY(vc2_cmd,		11),
	UC_EVENT_ENTRY(vc3_cmd,		12),
	UC_EVENT_ENTRY(vc4_cmd,		13),
	UC_EVENT_ENTRY(vc5_cmd,		14),
	UC_EVENT_ENTRY(vc0_pkt,		15),
	UC_EVENT_ENTRY(vc1_pkt,		16),
	UC_EVENT_ENTRY(vc2_pkt,		17),
	UC_EVENT_ENTRY(vc3_pkt,		18),
	UC_EVENT_ENTRY(vc4_pkt,		19),
	UC_EVENT_ENTRY(vc5_pkt,		20),
	UC_EVENT_ENTRY(vc6_pkt,		21),
	UC_EVENT_ENTRY(vc7_pkt,		22),
	UC_EVENT_ENTRY(vc8_pkt,		23),
	UC_EVENT_ENTRY(vc9_pkt,		24),
	UC_EVENT_ENTRY(vc10_pkt,	25),
	UC_EVENT_ENTRY(vc11_pkt,	26),
	UC_EVENT_ENTRY(vc12_pkt,	27),
	UC_EVENT_ENTRY(vc13_pkt,	28),
	UC_EVENT_ENTRY(vc0_con,		29),
	UC_EVENT_ENTRY(vc1_con,		30),
	UC_EVENT_ENTRY(vc2_con,		31),
	UC_EVENT_ENTRY(vc3_con,		32),
	UC_EVENT_ENTRY(vc4_con,		33),
	UC_EVENT_ENTRY(vc5_con,		34),
	UC_EVENT_ENTRY(vc6_con,		35),
	UC_EVENT_ENTRY(vc7_con,		36),
	UC_EVENT_ENTRY(vc8_con,		37),
	UC_EVENT_ENTRY(vc9_con,		38),
	UC_EVENT_ENTRY(vc10_con,	39),
	UC_EVENT_ENTRY(vc11_con,	40),
	UC_EVENT_ENTRY(vc12_con,	41),
	UC_EVENT_ENTRY(vc13_con,	42),
	NULL,
};

static struct attribute_group thunder_ocx_tlk_events_group = {
	.name = "events",
	.attrs = thunder_ocx_tlk_events_attr,
};

static const struct attribute_group *thunder_ocx_tlk_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_ocx_tlk_format_group,
	&thunder_ocx_tlk_events_group,
	NULL,
};

struct pmu thunder_ocx_tlk_pmu = {
	.name		= "thunder_ocx_tlk",
	.task_ctx_nr    = perf_invalid_context,
	.pmu_enable	= thunder_uncore_pmu_enable_ocx_tlk,
	.pmu_disable	= thunder_uncore_pmu_disable_ocx_tlk,
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add_ocx_tlk,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start_ocx_tlk,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read_ocx_tlk,
	.attr_groups	= thunder_ocx_tlk_attr_groups,
};

static bool event_valid(u64 config)
{
	if (config < ARRAY_SIZE(ocx_tlk_events))
		return true;

	return false;
}

int __init thunder_uncore_ocx_tlk_setup(void)
{
	int ret;

	thunder_uncore_ocx_tlk = kzalloc(sizeof(*thunder_uncore_ocx_tlk),
					 GFP_KERNEL);
	if (!thunder_uncore_ocx_tlk) {
		ret = -ENOMEM;
		goto fail_nomem;
	}

	ret = thunder_uncore_setup(thunder_uncore_ocx_tlk, 0xa013,
				   &thunder_ocx_tlk_pmu,
				   ARRAY_SIZE(ocx_tlk_events));
	if (ret)
		goto fail;

	thunder_uncore_ocx_tlk->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_ocx_tlk);
fail_nomem:
	return ret;
}
