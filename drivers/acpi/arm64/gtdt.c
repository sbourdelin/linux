/*
 * ARM Specific GTDT table Support
 *
 * Copyright (C) 2016, Linaro Ltd.
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

#include <clocksource/arm_arch_timer.h>

#undef pr_fmt
#define pr_fmt(fmt) "ACPI GTDT: " fmt

struct acpi_gtdt_descriptor {
	struct acpi_table_gtdt *gtdt;
	void *gtdt_end;
	unsigned int timer_block_count;
	unsigned int watchdog_count;
};

static struct acpi_gtdt_descriptor acpi_gtdt_desc __initdata;
static struct acpi_gtdt_timer_block **timer_block __initdata;
static struct acpi_gtdt_watchdog **watchdog __initdata;

static inline void *next_platform_timer(void *platform_timer)
{
	struct acpi_gtdt_header *gh = platform_timer;

	platform_timer += gh->length;
	if (platform_timer < acpi_gtdt_desc.gtdt_end)
		return platform_timer;

	return NULL;
}

#define for_each_platform_timer(_g) for (; _g; _g = next_platform_timer(_g))

static inline bool is_timer_block(void *platform_timer)
{
	struct acpi_gtdt_header *gh = platform_timer;

	return gh->type == ACPI_GTDT_TYPE_TIMER_BLOCK;
}

static inline bool is_watchdog(void *platform_timer)
{
	struct acpi_gtdt_header *gh = platform_timer;

	return gh->type == ACPI_GTDT_TYPE_WATCHDOG;
}

static int __init map_gt_gsi(u32 interrupt, u32 flags)
{
	int trigger, polarity;

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
 * Note: Linux on arm64 isn't supported on the secure side.
 * So we only handle the non-secure timer PPIs,
 * ARCH_TIMER_PHYS_SECURE_PPI is treated as invalid type.
 */
int __init acpi_gtdt_map_ppi(int type)
{
	struct acpi_table_gtdt *gtdt = acpi_gtdt_desc.gtdt;

	switch (type) {
	case ARCH_TIMER_PHYS_NONSECURE_PPI:
		return map_gt_gsi(gtdt->non_secure_el1_interrupt,
				  gtdt->non_secure_el1_flags);
	case ARCH_TIMER_VIRT_PPI:
		return map_gt_gsi(gtdt->virtual_timer_interrupt,
				  gtdt->virtual_timer_flags);

	case ARCH_TIMER_HYP_PPI:
		return map_gt_gsi(gtdt->non_secure_el2_interrupt,
				  gtdt->non_secure_el2_flags);
	default:
		pr_err("Failed to map timer interrupt: invalid type.\n");
	}

	return 0;
}

/*
 * acpi_gtdt_c3stop - got c3stop info from GTDT
 * @type: the type of PPI
 * Returns 1 if the timer is powered in deep idle state, 0 otherwise.
 */
bool __init acpi_gtdt_c3stop(int type)
{
	struct acpi_table_gtdt *gtdt = acpi_gtdt_desc.gtdt;

	switch (type) {
	case ARCH_TIMER_PHYS_NONSECURE_PPI:
		return !(gtdt->non_secure_el1_flags & ACPI_GTDT_ALWAYS_ON);

	case ARCH_TIMER_VIRT_PPI:
		return !(gtdt->virtual_timer_flags & ACPI_GTDT_ALWAYS_ON);

	case ARCH_TIMER_HYP_PPI:
		return !(gtdt->non_secure_el2_flags & ACPI_GTDT_ALWAYS_ON);

	default:
		pr_err("Failed to get c3stop info: invalid type.\n");
	}

	return 0;
}

/*
 * Release the memory we have allocated in acpi_gtdt_init.
 * This should be called, when the driver who called "acpi_gtdt_init" previously
 * doesn't need the GTDT info anymore.
 */
void __init acpi_gtdt_release(void)
{
	kfree(timer_block);
	kfree(watchdog);
	timer_block = NULL;
	watchdog = NULL;
}

/*
 * Get some basic info from GTDT table, and init the global variables above
 * for all timers initialization of Generic Timer.
 * This function does some validation on GTDT table.
 */
int __init acpi_gtdt_init(struct acpi_table_header *table)
{
	int timer_count;
	void *platform_timer;
	struct acpi_table_gtdt *gtdt;

	gtdt = container_of(table, struct acpi_table_gtdt, header);

	if (table->revision < 2) {
		pr_debug("Revision:%d doesn't support Platform Timers.\n",
			 table->revision);
		timer_count = 0;
	} else if (!gtdt->platform_timer_count) {
		pr_debug("No Platform Timer.\n");
		timer_count = 0;
	} else {
		timer_count = gtdt->platform_timer_count;
	}

	acpi_gtdt_desc.gtdt = gtdt;
	acpi_gtdt_desc.gtdt_end = (void *)table + table->length;

	if (!timer_count)
		return 0;

	platform_timer = (void *)gtdt + gtdt->platform_timer_offset;
	if (platform_timer < (void *)table + sizeof(struct acpi_table_gtdt)) {
		pr_err(FW_BUG "Failed to retrieve timer info from firmware: invalid data.\n");
		return -EINVAL;
	}

	timer_block = kcalloc(timer_count,
			      sizeof(struct acpi_gtdt_timer_block *),
			      GFP_KERNEL);
	if (!timer_block)
		return -ENOMEM;

	watchdog = kcalloc(timer_count, sizeof(struct acpi_gtdt_watchdog *),
			   GFP_KERNEL);
	if (!watchdog) {
		kfree(timer_block);
		timer_block = NULL;
		return -ENOMEM;
	}

	acpi_gtdt_desc.timer_block_count = 0;
	acpi_gtdt_desc.watchdog_count = 0;
	for_each_platform_timer(platform_timer) {
		if (is_timer_block(platform_timer)) {
			timer_block[acpi_gtdt_desc.timer_block_count++] =
				platform_timer;
		} else if (is_watchdog(platform_timer)) {
			watchdog[acpi_gtdt_desc.watchdog_count++] =
				platform_timer;
		} else {
			pr_err(FW_BUG "Invalid platform timer type.\n");
			goto error;
		}
	}

	if (timer_count == acpi_gtdt_desc.watchdog_count +
			   acpi_gtdt_desc.timer_block_count)
		return 0;

	pr_err(FW_BUG "Invalid platform timer number.\n");
error:
	acpi_gtdt_release();
	return -EINVAL;
}
