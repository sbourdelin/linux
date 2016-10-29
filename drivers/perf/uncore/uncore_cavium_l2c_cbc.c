/*
 * Cavium Thunder uncore PMU support, L2 Cache,
 * Crossbar connect (CBC) counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/perf_event.h>
#include <linux/slab.h>

#include "uncore_cavium.h"

struct thunder_uncore *thunder_uncore_l2c_cbc;

/* L2C CBC event list */
#define L2C_CBC_EVENT_XMC0		0x00
#define L2C_CBC_EVENT_XMD0		0x08
#define L2C_CBC_EVENT_RSC0		0x10
#define L2C_CBC_EVENT_RSD0		0x18
#define L2C_CBC_EVENT_INV0		0x20
#define L2C_CBC_EVENT_IOC0		0x28
#define L2C_CBC_EVENT_IOR0		0x30
#define L2C_CBC_EVENT_XMC1		0x40
#define L2C_CBC_EVENT_XMD1		0x48
#define L2C_CBC_EVENT_RSC1		0x50
#define L2C_CBC_EVENT_RSD1		0x58
#define L2C_CBC_EVENT_INV1		0x60
#define L2C_CBC_EVENT_XMC2		0x80
#define L2C_CBC_EVENT_XMD2		0x88
#define L2C_CBC_EVENT_RSC2		0x90
#define L2C_CBC_EVENT_RSD2		0x98

static int l2c_cbc_events[] = {
	L2C_CBC_EVENT_XMC0,
	L2C_CBC_EVENT_XMD0,
	L2C_CBC_EVENT_RSC0,
	L2C_CBC_EVENT_RSD0,
	L2C_CBC_EVENT_INV0,
	L2C_CBC_EVENT_IOC0,
	L2C_CBC_EVENT_IOR0,
	L2C_CBC_EVENT_XMC1,
	L2C_CBC_EVENT_XMD1,
	L2C_CBC_EVENT_RSC1,
	L2C_CBC_EVENT_RSD1,
	L2C_CBC_EVENT_INV1,
	L2C_CBC_EVENT_XMC2,
	L2C_CBC_EVENT_XMD2,
	L2C_CBC_EVENT_RSC2,
	L2C_CBC_EVENT_RSD2,
};

static int thunder_uncore_add_l2c_cbc(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	return thunder_uncore_add(event, flags, 0,
				  l2c_cbc_events[get_id(hwc->config)]);
}

PMU_FORMAT_ATTR(event, "config:0-4");

static struct attribute *thunder_l2c_cbc_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_l2c_cbc_format_group = {
	.name = "format",
	.attrs = thunder_l2c_cbc_format_attr,
};

static struct attribute *thunder_l2c_cbc_events_attr[] = {
	UC_EVENT_ENTRY(xmc0, 0),
	UC_EVENT_ENTRY(xmd0, 1),
	UC_EVENT_ENTRY(rsc0, 2),
	UC_EVENT_ENTRY(rsd0, 3),
	UC_EVENT_ENTRY(inv0, 4),
	UC_EVENT_ENTRY(ioc0, 5),
	UC_EVENT_ENTRY(ior0, 6),
	UC_EVENT_ENTRY(xmc1, 7),
	UC_EVENT_ENTRY(xmd1, 8),
	UC_EVENT_ENTRY(rsc1, 9),
	UC_EVENT_ENTRY(rsd1, 10),
	UC_EVENT_ENTRY(inv1, 11),
	UC_EVENT_ENTRY(xmc2, 12),
	UC_EVENT_ENTRY(xmd2, 13),
	UC_EVENT_ENTRY(rsc2, 14),
	UC_EVENT_ENTRY(rsd2, 15),
	NULL,
};

static struct attribute_group thunder_l2c_cbc_events_group = {
	.name = "events",
	.attrs = thunder_l2c_cbc_events_attr,
};

static const struct attribute_group *thunder_l2c_cbc_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_l2c_cbc_format_group,
	&thunder_l2c_cbc_events_group,
	NULL,
};

struct pmu thunder_l2c_cbc_pmu = {
	.name		= "thunder_l2c_cbc",
	.task_ctx_nr    = perf_invalid_context,
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add_l2c_cbc,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read,
	.attr_groups	= thunder_l2c_cbc_attr_groups,
};

static bool event_valid(u64 config)
{
	if (config < ARRAY_SIZE(l2c_cbc_events))
		return true;

	return false;
}

int __init thunder_uncore_l2c_cbc_setup(void)
{
	int ret = -ENOMEM;

	thunder_uncore_l2c_cbc = kzalloc(sizeof(*thunder_uncore_l2c_cbc),
					 GFP_KERNEL);
	if (!thunder_uncore_l2c_cbc)
		goto fail_nomem;

	ret = thunder_uncore_setup(thunder_uncore_l2c_cbc, 0xa02f,
				   &thunder_l2c_cbc_pmu,
				   ARRAY_SIZE(l2c_cbc_events));
	if (ret)
		goto fail;

	thunder_uncore_l2c_cbc->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_l2c_cbc);
fail_nomem:
	return ret;
}
