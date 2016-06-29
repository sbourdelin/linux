/*
 * ARM Specific GTDT table Support
 *
 * Copyright (C) 2015, Linaro Ltd.
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *         Fu Wei <fu.wei@linaro.org>
 *         Hanjun Guo <hanjun.guo@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <clocksource/arm_arch_timer.h>

#undef pr_fmt
#define pr_fmt(fmt) "GTDT: " fmt

typedef struct {
	struct acpi_table_gtdt *gtdt;
	void *platform_timer_start;
	void *gtdt_end;
} acpi_gtdt_desc_t;

static acpi_gtdt_desc_t acpi_gtdt_desc __initdata;

static inline void *gtdt_next(void *platform_timer, void *end, int type)
{
	struct acpi_gtdt_header *gh = platform_timer;

	while ((void *)(gh += gh->length) < end)
		if (gh->type == type)
			return (void *)gh;
	return NULL;
}

#define for_each_gtdt_type(_g, _t)				\
	for (_g = acpi_gtdt_desc.platform_timer_start; _g;	\
	     _g = gtdt_next(_g, acpi_gtdt_desc.gtdt_end, _t))

#define for_each_gtdt_timer(_g)				\
	for_each_gtdt_type(_g, ACPI_GTDT_TYPE_TIMER_BLOCK)

#define for_each_gtdt_watchdog(_g)			\
	for_each_gtdt_type(_g, ACPI_GTDT_TYPE_WATCHDOG)

/*
 * Get some basic info from GTDT table, and init the global variables above
 * for all timers initialization of Generic Timer.
 * This function does some validation on GTDT table.
 */
static int __init acpi_gtdt_desc_init(struct acpi_table_header *table)
{
	struct acpi_table_gtdt *gtdt = container_of(table,
						    struct acpi_table_gtdt,
						    header);

	acpi_gtdt_desc.gtdt = gtdt;
	acpi_gtdt_desc.gtdt_end = (void *)table + table->length;

	if (table->revision < 2) {
		pr_info("Revision:%d doesn't support Platform Timers.\n",
			table->revision);
		return 0;
	}

	if (!gtdt->platform_timer_count) {
		pr_info("No Platform Timer.\n");
		return 0;
	}

	acpi_gtdt_desc.platform_timer_start = (void *)gtdt +
					      gtdt->platform_timer_offset;
	if (acpi_gtdt_desc.platform_timer_start <
	    (void *)table + sizeof(struct acpi_table_gtdt)) {
		pr_err(FW_BUG "Platform Timer pointer error.\n");
		acpi_gtdt_desc.platform_timer_start = NULL;
		return -EINVAL;
	}

	return gtdt->platform_timer_count;
}

static int __init map_generic_timer_interrupt(u32 interrupt, u32 flags)
{
	int trigger, polarity;

	if (!interrupt)
		return 0;

	trigger = (flags & ACPI_GTDT_INTERRUPT_MODE) ? ACPI_EDGE_SENSITIVE
			: ACPI_LEVEL_SENSITIVE;

	polarity = (flags & ACPI_GTDT_INTERRUPT_POLARITY) ? ACPI_ACTIVE_LOW
			: ACPI_ACTIVE_HIGH;

	return acpi_register_gsi(NULL, interrupt, trigger, polarity);
}

/*
 * Map the PPIs of per-cpu arch_timer.
 * @type: the type of PPI
 * Returns 0 if error.
 */
int __init acpi_gtdt_map_ppi(int type)
{
	struct acpi_table_gtdt *gtdt = acpi_gtdt_desc.gtdt;

	switch (type) {
	case PHYS_SECURE_PPI:
		return map_generic_timer_interrupt(gtdt->secure_el1_interrupt,
						   gtdt->secure_el1_flags);
	case PHYS_NONSECURE_PPI:
		return map_generic_timer_interrupt(gtdt->non_secure_el1_interrupt,
						   gtdt->non_secure_el1_flags);
	case VIRT_PPI:
		return map_generic_timer_interrupt(gtdt->virtual_timer_interrupt,
						   gtdt->virtual_timer_flags);

	case HYP_PPI:
		return map_generic_timer_interrupt(gtdt->non_secure_el2_interrupt,
						   gtdt->non_secure_el2_flags);
	default:
		pr_err("ppi type error.\n");
	}

	return 0;
}

/*
 * acpi_gtdt_c3stop - got c3stop info from GTDT
 *
 * Returns 1 if the timer is powered in deep idle state, 0 otherwise.
 */
int __init acpi_gtdt_c3stop(void)
{
	struct acpi_table_gtdt *gtdt = acpi_gtdt_desc.gtdt;

	return !(gtdt->non_secure_el1_flags & ACPI_GTDT_ALWAYS_ON);
}

int __init gtdt_arch_timer_init(struct acpi_table_header *table)
{
	if (table)
		return acpi_gtdt_desc_init(table);

	pr_err("table pointer error.\n");

	return -EINVAL;
}
