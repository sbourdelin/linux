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
#include <linux/platform_device.h>

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

static inline struct acpi_gtdt_timer_block *get_timer_block(unsigned int index)
{
	if (index >= acpi_gtdt_desc.timer_block_count || !timer_block)
		return NULL;

	return timer_block[index];
}

static inline bool is_watchdog(void *platform_timer)
{
	struct acpi_gtdt_header *gh = platform_timer;

	return gh->type == ACPI_GTDT_TYPE_WATCHDOG;
}

static inline struct acpi_gtdt_watchdog *get_watchdog(unsigned int index)
{
	if (index >= acpi_gtdt_desc.watchdog_count || !watchdog)
		return NULL;

	return watchdog[index];
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

/*
 * Get ONE GT block info for memory-mapped timer from GTDT table.
 * @data: the GT block data (parsing result)
 * @index: the index number of GT block
 * Note: we already verify @data in caller, it can't be NULL here.
 * Returns 0 if success, -EINVAL/-ENODEV if error.
 */
int __init gtdt_arch_timer_mem_init(struct arch_timer_mem *data,
				    unsigned int index)
{
	struct acpi_gtdt_timer_block *block;
	struct acpi_gtdt_timer_entry *frame;
	int i;

	block = get_timer_block(index);
	if (!block)
		return -ENODEV;

	if (!block->timer_count) {
		pr_err(FW_BUG "GT block present, but frame count is zero.");
		return -ENODEV;
	}

	if (block->timer_count > ARCH_TIMER_MEM_MAX_FRAMES) {
		pr_err(FW_BUG "GT block lists %d frames, ACPI spec only allows 8\n",
		       block->timer_count);
		return -EINVAL;
	}

	data->cntctlbase = (phys_addr_t)block->block_address;
	/*
	 * We can NOT get the size info from GTDT table,
	 * but according to "Table * CNTCTLBase memory map" of
	 * <ARM Architecture Reference Manual> for ARMv8,
	 * it should be 4KB(Offset 0x000 – 0xFFC).
	 */
	data->size = SZ_4K;
	data->num_frames = block->timer_count;

	frame = (void *)block + block->timer_offset;
	if (frame + block->timer_count != (void *)block + block->header.length)
		return -EINVAL;

	/*
	 * Get the GT timer Frame data for every GT Block Timer
	 */
	for (i = 0; i < block->timer_count; i++, frame++) {
		if (!frame->base_address || !frame->timer_interrupt)
			return -EINVAL;

		data->frame[i].phys_irq = map_gt_gsi(frame->timer_interrupt,
						     frame->timer_flags);
		if (data->frame[i].phys_irq <= 0) {
			pr_warn("failed to map physical timer irq in frame %d.\n",
				i);
			return -EINVAL;
		}

		if (frame->virtual_timer_interrupt) {
			data->frame[i].virt_irq =
				map_gt_gsi(frame->virtual_timer_interrupt,
					   frame->virtual_timer_flags);
			if (data->frame[i].virt_irq <= 0) {
				pr_warn("failed to map virtual timer irq in frame %d.\n",
					i);
				return -EINVAL;
			}
		}

		data->frame[i].frame_nr = frame->frame_number;
		data->frame[i].cntbase = frame->base_address;
		/*
		 * We can NOT get the size info from GTDT table,
		 * but according to "Table * CNTBaseN memory map" of
		 * <ARM Architecture Reference Manual> for ARMv8,
		 * it should be 4KB(Offset 0x000 – 0xFFC).
		 */
		data->frame[i].size = SZ_4K;
	}

	if (acpi_gtdt_desc.timer_block_count)
		pr_info("parsed No.%d of %d memory-mapped timer block(s).\n",
			index, acpi_gtdt_desc.timer_block_count);

	return 0;
}

/*
 * Initialize a SBSA generic Watchdog platform device info from GTDT
 */
static int __init gtdt_import_sbsa_gwdt(struct acpi_gtdt_watchdog *wd,
					int index)
{
	struct platform_device *pdev;
	int irq = map_gt_gsi(wd->timer_interrupt, wd->timer_flags);
	int no_irq = 1;

	/*
	 * According to SBSA specification the size of refresh and control
	 * frames of SBSA Generic Watchdog is SZ_4K(Offset 0x000 – 0xFFF).
	 */
	struct resource res[] = {
		DEFINE_RES_MEM(wd->control_frame_address, SZ_4K),
		DEFINE_RES_MEM(wd->refresh_frame_address, SZ_4K),
		DEFINE_RES_IRQ(irq),
	};

	pr_debug("found a Watchdog (0x%llx/0x%llx gsi:%u flags:0x%x).\n",
		 wd->refresh_frame_address, wd->control_frame_address,
		 wd->timer_interrupt, wd->timer_flags);

	if (!(wd->refresh_frame_address && wd->control_frame_address)) {
		pr_err(FW_BUG "failed to get the Watchdog base address.\n");
		return -EINVAL;
	}

	if (!wd->timer_interrupt)
		pr_warn(FW_BUG "failed to get the Watchdog interrupt.\n");
	else if (irq <= 0)
		pr_warn("failed to map the Watchdog interrupt.\n");
	else
		no_irq = 0;

	/*
	 * Add a platform device named "sbsa-gwdt" to match the platform driver.
	 * "sbsa-gwdt": SBSA(Server Base System Architecture) Generic Watchdog
	 * The platform driver (like drivers/watchdog/sbsa_gwdt.c)can get device
	 * info below by matching this name.
	 */
	pdev = platform_device_register_simple("sbsa-gwdt", index, res,
					       ARRAY_SIZE(res) - no_irq);
	if (IS_ERR(pdev)) {
		acpi_unregister_gsi(wd->timer_interrupt);
		return PTR_ERR(pdev);
	}

	return 0;
}

static int __init gtdt_sbsa_gwdt_init(void)
{
	int i, ret;
	struct acpi_table_header *table;
	struct acpi_gtdt_watchdog *watchdog;

	if (acpi_disabled)
		return 0;

	if (ACPI_FAILURE(acpi_get_table(ACPI_SIG_GTDT, 0, &table)))
		return -EINVAL;

	ret = acpi_gtdt_init(table);
	if (ret)
		return ret;

	if (!acpi_gtdt_desc.watchdog_count)
		return 0;

	for (i = 0; i < acpi_gtdt_desc.watchdog_count; i++) {
		watchdog = get_watchdog(i);
		if (!watchdog) {
			ret = -ENODEV;
			break;
		}
		ret = gtdt_import_sbsa_gwdt(watchdog, i);
		if (ret)
			break;
	}

	pr_info("found %d SBSA generic Watchdog(s), %d imported.\n",
		acpi_gtdt_desc.watchdog_count, i);

	acpi_gtdt_release();
	return ret;
}

device_initcall(gtdt_sbsa_gwdt_init);
