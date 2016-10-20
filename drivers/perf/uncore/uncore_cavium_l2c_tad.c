/*
 * Cavium Thunder uncore PMU support,
 * L2 Cache tag-and-data-units (L2C TAD) counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/perf_event.h>
#include <linux/slab.h>

#include "uncore_cavium.h"

struct thunder_uncore *thunder_uncore_l2c_tad;

#define L2C_TAD_NR_COUNTERS             4
#define L2C_TAD_PRF_OFFSET		0x10000
#define L2C_TAD_PFC_OFFSET		0x10100

/*
 * Counters are selected via L2C_TAD(x)_PRF:
 *
 *   63					    32
 *   +---------------------------------------+
 *   |  Reserved			     |
 *   +---------------------------------------+
 *   | CNT3SEL | CNT2SEL | CNT1SEL | CNT0SEL |
 *   +---------------------------------------+
 *   31       24	16	  8	     0
 *
 * config_base contains the offset of the selected CNTxSEL in the mapped BAR.
 *
 * Counters are read via L2C_TAD(x)_PFC(0..3).
 * event_base contains the associated address to read the counter.
 */

/* L2C TAD event list */
#define L2C_TAD_EVENTS_DISABLED			0x00
#define L2C_TAD_EVENT_L2T_HIT			0x01
#define L2C_TAD_EVENT_L2T_MISS			0x02
#define L2C_TAD_EVENT_L2T_NOALLOC		0x03
#define L2C_TAD_EVENT_L2_VIC			0x04
#define L2C_TAD_EVENT_SC_FAIL			0x05
#define L2C_TAD_EVENT_SC_PASS			0x06
#define L2C_TAD_EVENT_LFB_OCC			0x07
#define L2C_TAD_EVENT_WAIT_LFB			0x08
#define L2C_TAD_EVENT_WAIT_VAB			0x09
#define L2C_TAD_EVENT_OPEN_CCPI			0x0a
#define L2C_TAD_EVENT_LOOKUP			0x40
#define L2C_TAD_EVENT_LOOKUP_XMC_LCL		0x41
#define L2C_TAD_EVENT_LOOKUP_XMC_RMT		0x42
#define L2C_TAD_EVENT_LOOKUP_MIB		0x43
#define L2C_TAD_EVENT_LOOKUP_ALL		0x44
#define L2C_TAD_EVENT_TAG_ALC_HIT		0x48
#define L2C_TAD_EVENT_TAG_ALC_MISS		0x49
#define L2C_TAD_EVENT_TAG_ALC_NALC		0x4a
#define L2C_TAD_EVENT_TAG_NALC_HIT		0x4b
#define L2C_TAD_EVENT_TAG_NALC_MISS		0x4c
#define L2C_TAD_EVENT_LMC_WR			0x4e
#define L2C_TAD_EVENT_LMC_SBLKDTY		0x4f
#define L2C_TAD_EVENT_TAG_ALC_RTG_HIT		0x50
#define L2C_TAD_EVENT_TAG_ALC_RTG_HITE		0x51
#define L2C_TAD_EVENT_TAG_ALC_RTG_HITS		0x52
#define L2C_TAD_EVENT_TAG_ALC_RTG_MISS		0x53
#define L2C_TAD_EVENT_TAG_NALC_RTG_HIT		0x54
#define L2C_TAD_EVENT_TAG_NALC_RTG_MISS		0x55
#define L2C_TAD_EVENT_TAG_NALC_RTG_HITE		0x56
#define L2C_TAD_EVENT_TAG_NALC_RTG_HITS		0x57
#define L2C_TAD_EVENT_TAG_ALC_LCL_EVICT		0x58
#define L2C_TAD_EVENT_TAG_ALC_LCL_CLNVIC	0x59
#define L2C_TAD_EVENT_TAG_ALC_LCL_DTYVIC	0x5a
#define L2C_TAD_EVENT_TAG_ALC_RMT_EVICT		0x5b
#define L2C_TAD_EVENT_TAG_ALC_RMT_VIC		0x5c
#define L2C_TAD_EVENT_RTG_ALC			0x5d
#define L2C_TAD_EVENT_RTG_ALC_HIT		0x5e
#define L2C_TAD_EVENT_RTG_ALC_HITWB		0x5f
#define L2C_TAD_EVENT_STC_TOTAL			0x60
#define L2C_TAD_EVENT_STC_TOTAL_FAIL		0x61
#define L2C_TAD_EVENT_STC_RMT			0x62
#define L2C_TAD_EVENT_STC_RMT_FAIL		0x63
#define L2C_TAD_EVENT_STC_LCL			0x64
#define L2C_TAD_EVENT_STC_LCL_FAIL		0x65
#define L2C_TAD_EVENT_OCI_RTG_WAIT		0x68
#define L2C_TAD_EVENT_OCI_FWD_CYC_HIT		0x69
#define L2C_TAD_EVENT_OCI_FWD_RACE		0x6a
#define L2C_TAD_EVENT_OCI_HAKS			0x6b
#define L2C_TAD_EVENT_OCI_FLDX_TAG_E_NODAT	0x6c
#define L2C_TAD_EVENT_OCI_FLDX_TAG_E_DAT	0x6d
#define L2C_TAD_EVENT_OCI_RLDD			0x6e
#define L2C_TAD_EVENT_OCI_RLDD_PEMD		0x6f
#define L2C_TAD_EVENT_OCI_RRQ_DAT_CNT		0x70
#define L2C_TAD_EVENT_OCI_RRQ_DAT_DMASK		0x71
#define L2C_TAD_EVENT_OCI_RSP_DAT_CNT		0x72
#define L2C_TAD_EVENT_OCI_RSP_DAT_DMASK		0x73
#define L2C_TAD_EVENT_OCI_RSP_DAT_VICD_CNT	0x74
#define L2C_TAD_EVENT_OCI_RSP_DAT_VICD_DMASK	0x75
#define L2C_TAD_EVENT_OCI_RTG_ALC_EVICT		0x76
#define L2C_TAD_EVENT_OCI_RTG_ALC_VIC		0x77
#define L2C_TAD_EVENT_QD0_IDX			0x80
#define L2C_TAD_EVENT_QD0_RDAT			0x81
#define L2C_TAD_EVENT_QD0_BNKS			0x82
#define L2C_TAD_EVENT_QD0_WDAT			0x83
#define L2C_TAD_EVENT_QD1_IDX			0x90
#define L2C_TAD_EVENT_QD1_RDAT			0x91
#define L2C_TAD_EVENT_QD1_BNKS			0x92
#define L2C_TAD_EVENT_QD1_WDAT			0x93
#define L2C_TAD_EVENT_QD2_IDX			0xa0
#define L2C_TAD_EVENT_QD2_RDAT			0xa1
#define L2C_TAD_EVENT_QD2_BNKS			0xa2
#define L2C_TAD_EVENT_QD2_WDAT			0xa3
#define L2C_TAD_EVENT_QD3_IDX			0xb0
#define L2C_TAD_EVENT_QD3_RDAT			0xb1
#define L2C_TAD_EVENT_QD3_BNKS			0xb2
#define L2C_TAD_EVENT_QD3_WDAT			0xb3
#define L2C_TAD_EVENT_QD4_IDX			0xc0
#define L2C_TAD_EVENT_QD4_RDAT			0xc1
#define L2C_TAD_EVENT_QD4_BNKS			0xc2
#define L2C_TAD_EVENT_QD4_WDAT			0xc3
#define L2C_TAD_EVENT_QD5_IDX			0xd0
#define L2C_TAD_EVENT_QD5_RDAT			0xd1
#define L2C_TAD_EVENT_QD5_BNKS			0xd2
#define L2C_TAD_EVENT_QD5_WDAT			0xd3
#define L2C_TAD_EVENT_QD6_IDX			0xe0
#define L2C_TAD_EVENT_QD6_RDAT			0xe1
#define L2C_TAD_EVENT_QD6_BNKS			0xe2
#define L2C_TAD_EVENT_QD6_WDAT			0xe3
#define L2C_TAD_EVENT_QD7_IDX			0xf0
#define L2C_TAD_EVENT_QD7_RDAT			0xf1
#define L2C_TAD_EVENT_QD7_BNKS			0xf2
#define L2C_TAD_EVENT_QD7_WDAT			0xf3

static void thunder_uncore_start_l2c_tad(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;
	int id;

	node = get_node(hwc->config, uncore);
	id = get_id(hwc->config);

	/* reset counter values to zero */
	if (flags & PERF_EF_RELOAD)
		list_for_each_entry(unit, &node->unit_list, entry)
			writeq(0, hwc->event_base + unit->map);

	/* start counters on all units on the node */
	list_for_each_entry(unit, &node->unit_list, entry)
		writeb(id, hwc->config_base + unit->map);

	hwc->state = 0;
	perf_event_update_userpage(event);
}

static void thunder_uncore_stop_l2c_tad(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;

	node = get_node(hwc->config, uncore);

	/* disable counters for all units on the node */
	list_for_each_entry(unit, &node->unit_list, entry)
		writeb(L2C_TAD_EVENTS_DISABLED, hwc->config_base + unit->map);
	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunder_uncore_read(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int thunder_uncore_add_l2c_tad(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = to_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int i;

	node = get_node(hwc->config, uncore);

	/* take the first available counter */
	for (i = 0; i < node->num_counters; i++) {
		if (!cmpxchg(&node->events[i], NULL, event)) {
			hwc->idx = i;
			break;
		}
	}

	if (hwc->idx == -1)
		return -EBUSY;

	/* see comment at beginning of file */
	hwc->config_base = L2C_TAD_PRF_OFFSET + hwc->idx;
	hwc->event_base = L2C_TAD_PFC_OFFSET + hwc->idx * sizeof(u64);

	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;
	if (flags & PERF_EF_START)
		thunder_uncore_start(event, PERF_EF_RELOAD);
	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *thunder_l2c_tad_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_l2c_tad_format_group = {
	.name = "format",
	.attrs = thunder_l2c_tad_format_attr,
};

static struct attribute *thunder_l2c_tad_events_attr[] = {
	UC_EVENT_ENTRY(l2t_hit,			L2C_TAD_EVENT_L2T_HIT),
	UC_EVENT_ENTRY(l2t_miss,		L2C_TAD_EVENT_L2T_MISS),
	UC_EVENT_ENTRY(l2t_noalloc,		L2C_TAD_EVENT_L2T_NOALLOC),
	UC_EVENT_ENTRY(l2_vic,			L2C_TAD_EVENT_L2_VIC),
	UC_EVENT_ENTRY(sc_fail,			L2C_TAD_EVENT_SC_FAIL),
	UC_EVENT_ENTRY(sc_pass,			L2C_TAD_EVENT_SC_PASS),
	UC_EVENT_ENTRY(lfb_occ,			L2C_TAD_EVENT_LFB_OCC),
	UC_EVENT_ENTRY(wait_lfb,		L2C_TAD_EVENT_WAIT_LFB),
	UC_EVENT_ENTRY(wait_vab,		L2C_TAD_EVENT_WAIT_VAB),
	UC_EVENT_ENTRY(open_ccpi,		L2C_TAD_EVENT_OPEN_CCPI),
	UC_EVENT_ENTRY(lookup,			L2C_TAD_EVENT_LOOKUP),
	UC_EVENT_ENTRY(lookup_xmc_lcl,		L2C_TAD_EVENT_LOOKUP_XMC_LCL),
	UC_EVENT_ENTRY(lookup_xmc_rmt,		L2C_TAD_EVENT_LOOKUP_XMC_RMT),
	UC_EVENT_ENTRY(lookup_mib,		L2C_TAD_EVENT_LOOKUP_MIB),
	UC_EVENT_ENTRY(lookup_all,		L2C_TAD_EVENT_LOOKUP_ALL),
	UC_EVENT_ENTRY(tag_alc_hit,		L2C_TAD_EVENT_TAG_ALC_HIT),
	UC_EVENT_ENTRY(tag_alc_miss,		L2C_TAD_EVENT_TAG_ALC_MISS),
	UC_EVENT_ENTRY(tag_alc_nalc,		L2C_TAD_EVENT_TAG_ALC_NALC),
	UC_EVENT_ENTRY(tag_nalc_hit,		L2C_TAD_EVENT_TAG_NALC_HIT),
	UC_EVENT_ENTRY(tag_nalc_miss,		L2C_TAD_EVENT_TAG_NALC_MISS),
	UC_EVENT_ENTRY(lmc_wr,			L2C_TAD_EVENT_LMC_WR),
	UC_EVENT_ENTRY(lmc_sblkdty,		L2C_TAD_EVENT_LMC_SBLKDTY),
	UC_EVENT_ENTRY(tag_alc_rtg_hit,		L2C_TAD_EVENT_TAG_ALC_RTG_HIT),
	UC_EVENT_ENTRY(tag_alc_rtg_hite,	L2C_TAD_EVENT_TAG_ALC_RTG_HITE),
	UC_EVENT_ENTRY(tag_alc_rtg_hits,	L2C_TAD_EVENT_TAG_ALC_RTG_HITS),
	UC_EVENT_ENTRY(tag_alc_rtg_miss,	L2C_TAD_EVENT_TAG_ALC_RTG_MISS),
	UC_EVENT_ENTRY(tag_alc_nalc_rtg_hit,	L2C_TAD_EVENT_TAG_NALC_RTG_HIT),
	UC_EVENT_ENTRY(tag_nalc_rtg_miss,	L2C_TAD_EVENT_TAG_NALC_RTG_MISS),
	UC_EVENT_ENTRY(tag_nalc_rtg_hite,	L2C_TAD_EVENT_TAG_NALC_RTG_HITE),
	UC_EVENT_ENTRY(tag_nalc_rtg_hits,	L2C_TAD_EVENT_TAG_NALC_RTG_HITS),
	UC_EVENT_ENTRY(tag_alc_lcl_evict,	L2C_TAD_EVENT_TAG_ALC_LCL_EVICT),
	UC_EVENT_ENTRY(tag_alc_lcl_clnvic,	L2C_TAD_EVENT_TAG_ALC_LCL_CLNVIC),
	UC_EVENT_ENTRY(tag_alc_lcl_dtyvic,	L2C_TAD_EVENT_TAG_ALC_LCL_DTYVIC),
	UC_EVENT_ENTRY(tag_alc_rmt_evict,	L2C_TAD_EVENT_TAG_ALC_RMT_EVICT),
	UC_EVENT_ENTRY(tag_alc_rmt_vic,		L2C_TAD_EVENT_TAG_ALC_RMT_VIC),
	UC_EVENT_ENTRY(rtg_alc,			L2C_TAD_EVENT_RTG_ALC),
	UC_EVENT_ENTRY(rtg_alc_hit,		L2C_TAD_EVENT_RTG_ALC_HIT),
	UC_EVENT_ENTRY(rtg_alc_hitwb,		L2C_TAD_EVENT_RTG_ALC_HITWB),
	UC_EVENT_ENTRY(stc_total,		L2C_TAD_EVENT_STC_TOTAL),
	UC_EVENT_ENTRY(stc_total_fail,		L2C_TAD_EVENT_STC_TOTAL_FAIL),
	UC_EVENT_ENTRY(stc_rmt,			L2C_TAD_EVENT_STC_RMT),
	UC_EVENT_ENTRY(stc_rmt_fail,		L2C_TAD_EVENT_STC_RMT_FAIL),
	UC_EVENT_ENTRY(stc_lcl,			L2C_TAD_EVENT_STC_LCL),
	UC_EVENT_ENTRY(stc_lcl_fail,		L2C_TAD_EVENT_STC_LCL_FAIL),
	UC_EVENT_ENTRY(oci_rtg_wait,		L2C_TAD_EVENT_OCI_RTG_WAIT),
	UC_EVENT_ENTRY(oci_fwd_cyc_hit,		L2C_TAD_EVENT_OCI_FWD_CYC_HIT),
	UC_EVENT_ENTRY(oci_fwd_race,		L2C_TAD_EVENT_OCI_FWD_RACE),
	UC_EVENT_ENTRY(oci_haks,		L2C_TAD_EVENT_OCI_HAKS),
	UC_EVENT_ENTRY(oci_fldx_tag_e_nodat,	L2C_TAD_EVENT_OCI_FLDX_TAG_E_NODAT),
	UC_EVENT_ENTRY(oci_fldx_tag_e_dat,	L2C_TAD_EVENT_OCI_FLDX_TAG_E_DAT),
	UC_EVENT_ENTRY(oci_rldd,		L2C_TAD_EVENT_OCI_RLDD),
	UC_EVENT_ENTRY(oci_rldd_pemd,		L2C_TAD_EVENT_OCI_RLDD_PEMD),
	UC_EVENT_ENTRY(oci_rrq_dat_cnt,		L2C_TAD_EVENT_OCI_RRQ_DAT_CNT),
	UC_EVENT_ENTRY(oci_rrq_dat_dmask,	L2C_TAD_EVENT_OCI_RRQ_DAT_DMASK),
	UC_EVENT_ENTRY(oci_rsp_dat_cnt,		L2C_TAD_EVENT_OCI_RSP_DAT_CNT),
	UC_EVENT_ENTRY(oci_rsp_dat_dmaks,	L2C_TAD_EVENT_OCI_RSP_DAT_DMASK),
	UC_EVENT_ENTRY(oci_rsp_dat_vicd_cnt,	L2C_TAD_EVENT_OCI_RSP_DAT_VICD_CNT),
	UC_EVENT_ENTRY(oci_rsp_dat_vicd_dmask,	L2C_TAD_EVENT_OCI_RSP_DAT_VICD_DMASK),
	UC_EVENT_ENTRY(oci_rtg_alc_evict,	L2C_TAD_EVENT_OCI_RTG_ALC_EVICT),
	UC_EVENT_ENTRY(oci_rtg_alc_vic,		L2C_TAD_EVENT_OCI_RTG_ALC_VIC),
	UC_EVENT_ENTRY(qd0_idx,			L2C_TAD_EVENT_QD0_IDX),
	UC_EVENT_ENTRY(qd0_rdat,		L2C_TAD_EVENT_QD0_RDAT),
	UC_EVENT_ENTRY(qd0_bnks,		L2C_TAD_EVENT_QD0_BNKS),
	UC_EVENT_ENTRY(qd0_wdat,		L2C_TAD_EVENT_QD0_WDAT),
	UC_EVENT_ENTRY(qd1_idx,			L2C_TAD_EVENT_QD1_IDX),
	UC_EVENT_ENTRY(qd1_rdat,		L2C_TAD_EVENT_QD1_RDAT),
	UC_EVENT_ENTRY(qd1_bnks,		L2C_TAD_EVENT_QD1_BNKS),
	UC_EVENT_ENTRY(qd1_wdat,		L2C_TAD_EVENT_QD1_WDAT),
	UC_EVENT_ENTRY(qd2_idx,			L2C_TAD_EVENT_QD2_IDX),
	UC_EVENT_ENTRY(qd2_rdat,		L2C_TAD_EVENT_QD2_RDAT),
	UC_EVENT_ENTRY(qd2_bnks,		L2C_TAD_EVENT_QD2_BNKS),
	UC_EVENT_ENTRY(qd2_wdat,		L2C_TAD_EVENT_QD2_WDAT),
	UC_EVENT_ENTRY(qd3_idx,			L2C_TAD_EVENT_QD3_IDX),
	UC_EVENT_ENTRY(qd3_rdat,		L2C_TAD_EVENT_QD3_RDAT),
	UC_EVENT_ENTRY(qd3_bnks,		L2C_TAD_EVENT_QD3_BNKS),
	UC_EVENT_ENTRY(qd3_wdat,		L2C_TAD_EVENT_QD3_WDAT),
	UC_EVENT_ENTRY(qd4_idx,			L2C_TAD_EVENT_QD4_IDX),
	UC_EVENT_ENTRY(qd4_rdat,		L2C_TAD_EVENT_QD4_RDAT),
	UC_EVENT_ENTRY(qd4_bnks,		L2C_TAD_EVENT_QD4_BNKS),
	UC_EVENT_ENTRY(qd4_wdat,		L2C_TAD_EVENT_QD4_WDAT),
	UC_EVENT_ENTRY(qd5_idx,			L2C_TAD_EVENT_QD5_IDX),
	UC_EVENT_ENTRY(qd5_rdat,		L2C_TAD_EVENT_QD5_RDAT),
	UC_EVENT_ENTRY(qd5_bnks,		L2C_TAD_EVENT_QD5_BNKS),
	UC_EVENT_ENTRY(qd5_wdat,		L2C_TAD_EVENT_QD5_WDAT),
	UC_EVENT_ENTRY(qd6_idx,			L2C_TAD_EVENT_QD6_IDX),
	UC_EVENT_ENTRY(qd6_rdat,		L2C_TAD_EVENT_QD6_RDAT),
	UC_EVENT_ENTRY(qd6_bnks,		L2C_TAD_EVENT_QD6_BNKS),
	UC_EVENT_ENTRY(qd6_wdat,		L2C_TAD_EVENT_QD6_WDAT),
	UC_EVENT_ENTRY(qd7_idx,			L2C_TAD_EVENT_QD7_IDX),
	UC_EVENT_ENTRY(qd7_rdat,		L2C_TAD_EVENT_QD7_RDAT),
	UC_EVENT_ENTRY(qd7_bnks,		L2C_TAD_EVENT_QD7_BNKS),
	UC_EVENT_ENTRY(qd7_wdat,		L2C_TAD_EVENT_QD7_WDAT),
	NULL,
};

static struct attribute_group thunder_l2c_tad_events_group = {
	.name = "events",
	.attrs = thunder_l2c_tad_events_attr,
};

static const struct attribute_group *thunder_l2c_tad_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_l2c_tad_format_group,
	&thunder_l2c_tad_events_group,
	NULL,
};

struct pmu thunder_l2c_tad_pmu = {
	.name		= "thunder_l2c_tad",
	.task_ctx_nr    = perf_sw_context,
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add_l2c_tad,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start_l2c_tad,
	.stop		= thunder_uncore_stop_l2c_tad,
	.read		= thunder_uncore_read,
	.attr_groups	= thunder_l2c_tad_attr_groups,
};

static bool event_valid(u64 c)
{
	if ((c > 0 &&
	     c <= L2C_TAD_EVENT_OPEN_CCPI) ||
	    (c >= L2C_TAD_EVENT_LOOKUP &&
	     c <= L2C_TAD_EVENT_LOOKUP_ALL) ||
	    (c >= L2C_TAD_EVENT_TAG_ALC_HIT &&
	     c <= L2C_TAD_EVENT_TAG_NALC_MISS) ||
	    (c >= L2C_TAD_EVENT_LMC_WR &&
	     c <= L2C_TAD_EVENT_STC_LCL_FAIL) ||
	    (c >= L2C_TAD_EVENT_OCI_RTG_WAIT &&
	     c <= L2C_TAD_EVENT_OCI_RTG_ALC_VIC) ||
	    /* L2C_TAD_EVENT_QD[0..7] IDX,RDAT,BNKS,WDAT => 0x80 .. 0xf3 */
	    ((c & 0x80) && ((c & 0xf) <= 3)))
		return true;

	return false;
}

int __init thunder_uncore_l2c_tad_setup(void)
{
	int ret = -ENOMEM;

	thunder_uncore_l2c_tad = kzalloc(sizeof(*thunder_uncore_l2c_tad),
					 GFP_KERNEL);
	if (!thunder_uncore_l2c_tad)
		goto fail_nomem;

	ret = thunder_uncore_setup(thunder_uncore_l2c_tad, 0xa02e,
				   &thunder_l2c_tad_pmu, L2C_TAD_NR_COUNTERS);
	if (ret)
		goto fail;

	thunder_uncore_l2c_tad->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_l2c_tad);
fail_nomem:
	return ret;
}
