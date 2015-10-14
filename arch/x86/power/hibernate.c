/*
 * Hibernation common support for x86
 *
 * Distribute under GPLv2
 *
 * Copyright (c) 2015 Chen Yu <yu.c.chen@intel.com>
 */

#include <linux/suspend.h>
#include <linux/kdebug.h>

#include <asm/init.h>
#include <asm/suspend.h>

/*
 * The following section is to check whether the old e820 map
 * (system before hibernation) is compatible with current
 * e820 map(system for resuming).
 * We check two types of regions: E820_RAM and E820_ACPI,
 * and to make sure the two kinds of regions will satisfy:
 * 1. E820_RAM: each old region is a subset of the current ones.
 * 2. E820_ACPI: each old region is strictly the same as the current ones.
 *
 * We save the old e820 map inside the swsusp_info page,
 * then pass it to the second system for resuming, by the
 * following format:
 *
 *
 *  +--------+---------+------+------+------+
 *  | swsusp |e820entry|entry0|entry1|entry2|
 *  |  info  | number  |      |      |      |
 *  +--------+---------+------+------+------+
 *  ^                                                        ^
 *  |                                                        |
 *  +--------------struct swsusp_info(PAGE_SIZE)-------------+
 */

/*
 * Record the first pair of conflicted new/old
 * e820 entries if there's any.
 */
static u32 bad_old_type;
static u64 bad_old_start, bad_old_end;

static u32 bad_new_type;
static u64 bad_new_start, bad_new_end;

/**
 *	arch_image_info_save - save specified e820 data to
 *		 the hibernation image header
 *	@dst: address to save the data to.
 *	@src: source data need to be saved,
 *	      if NULL then save current system's e820 map.
 *	@limit_len: max len in bytes to write.
 */
int arch_image_info_save(char *dst, char *src, unsigned int limit_len)
{
	unsigned int e820_nr_map;
	unsigned int size_to_copy;
	struct e820map *e820_map;

	/*
	 * The final copied structure is illustrated below:
	 * [number_of_e820entry][e820entry0)[e820entry1)...
	 */
	if (src) {
		e820_nr_map = *(unsigned int *)src;
		e820_map = (struct e820map *)(src + sizeof(unsigned int));
	} else {
		e820_nr_map = e820_saved.nr_map;
		e820_map = &e820_saved;
	}

	size_to_copy = e820_nr_map * sizeof(struct e820entry);

	if ((size_to_copy + sizeof(unsigned int)) > limit_len) {
		pr_warn("PM: Hibernation can not save extra info due to too many e820 entries\n");
		return -ENOMEM;
	}
	*(unsigned int *)dst = e820_nr_map;
	dst += sizeof(unsigned int);
	memcpy(dst, (void *)&e820_map->map[0], size_to_copy);
	return 0;
}

/**
 *	arch_image_info_check - check the relationship between
 *	new and old e820 map, to make sure that, the E820_RAM
 *	in old e820, is a subset of the new e820 map, and the
 *	E820_ACPI regions in old e820 map, are strictly the
 *	same as new e820 map. If it is, return true, otherwise return false.
 *
 *	@new: New e820 map address, usually it is the
 *	      current system's e820_saved.
 *	@old: Old e820 map address, it is usually the
 *	      e820 map before hibernation.
 */
bool arch_image_info_check(const char *new, const char *old)
{
	struct e820map *e820_old, *e820_new;
	int i, j, e820_old_num, e820_new_num;

	e820_old = (struct e820map *)old;
	e820_old_num = *(unsigned int *)e820_old;

	if (new)
		e820_new = (struct e820map *)new;
	else
		e820_new = &e820_saved;

	e820_new_num = e820_new->nr_map;

	if ((e820_old_num == 0) || (e820_new_num == 0) ||
		(e820_old_num > E820_X_MAX) || (e820_new_num > E820_X_MAX))
		return false;

	for (i = 0; i < e820_old_num; i++) {
		u64 old_start, old_end;
		struct e820entry *ei_old;
		bool valid_old_entry = false;

		ei_old = &e820_old->map[i];

		/*
		 * Only check RAM memory and ACPI table regions,
		 * and we follow this policy:
		 * 1.The old e820 RAM region must be new RAM's subset.
		 * 2.The old e820 ACPI table region must be the same
		 *   as the new one.
		 */
		if (ei_old->type != E820_RAM && ei_old->type != E820_ACPI)
			continue;

		old_start = ei_old->addr;
		old_end = ei_old->addr + ei_old->size;

		for (j = 0; j < e820_new_num; j++) {
			u64 new_start, new_end;
			struct e820entry *ei_new;

			if (valid_old_entry)
				break;

			ei_new = &e820_new->map[i];
			new_start = ei_new->addr;
			new_end = ei_new->addr + ei_new->size;

			/*
			 * Check the relationship between these two regions.
			 */
			if (old_start >= new_start && old_start < new_end) {
				   /* Must be of the same type. */
				if ((ei_old->type != ei_new->type) ||
				   /* E820_RAM must be the subset */
				    ((ei_old->type == E820_RAM) &&
				     (old_end > new_end)) ||
				   /* E820_ACPI must remain unchanged. */
				    ((ei_old->type == E820_ACPI) &&
				     (old_start != new_start ||
						old_end != new_end))) {
					bad_old_start = old_start;
					bad_old_end = old_end;
					bad_old_type = ei_old->type;
					bad_new_start = new_start;
					bad_new_end = new_end;
					bad_new_type = ei_new->type;

					return false;
				}
				/* OK, this one is a valid e820 region. */
				valid_old_entry = true;
			}
		}
		/* If we did not find any overlapping between this old e820
		 * region and the new regions, return invalid.
		 */
		if (!valid_old_entry) {
			bad_old_start = old_start;
			bad_old_end = old_end;
			return false;
		}
	}
	/* All the old e820 entries are valid */
	return true;
}

/*
 * This hook is invoked when kernel dies, and will print the broken e820 map
 * if it is caused by BIOS memory bug.
 */
static int arch_hibernation_die_check(struct notifier_block *nb,
				      unsigned long action,
				      void *data)
{
	if (!bad_old_start || !bad_old_end)
		return 0;

	pr_err("PM: Hibernation Caution! Oops might be due to inconsistent e820 table.\n");
	pr_err("PM: [mem %#010llx-%#010llx][%s] is an invalid old e820 region.\n",
			bad_old_start, bad_old_end,
			(bad_old_type == E820_RAM) ? "RAM" : "ACPI Table");
	if (bad_new_start && bad_new_end)
		pr_err("PM: Inconsistent with current [mem %#010llx-%#010llx][%s]\n",
			bad_new_start, bad_new_end,
			(bad_new_type == E820_RAM) ? "RAM" : "ACPI Table");
	pr_err("PM: Please update your BIOS, or do not use hibernation on this machine.\n");

	/* Avoid nested die print*/
	bad_old_start = bad_old_end = 0;

	return 0;
}

static struct notifier_block hibernation_notifier = {
	.notifier_call = arch_hibernation_die_check,
};

static int __init arch_init_hibernation(void)
{
	int retval;

	retval = register_die_notifier(&hibernation_notifier);
	if (retval)
		return retval;

	return 0;
}

late_initcall(arch_init_hibernation);
