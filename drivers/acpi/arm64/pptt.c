/*
 * Copyright (C) 2017, ARM
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * This file implements parsing of Processor Properties Topology Table (PPTT)
 * which is optionally used to describe the processor and cache topology.
 * Due to the relative pointers used throughout the table, this doesn't
 * leverage the existing subtable parsing in the kernel.
 */

#define pr_fmt(fmt) "ACPI PPTT: " fmt

#include <linux/acpi.h>
#include <linux/cacheinfo.h>
#include <acpi/processor.h>

/*
 * Given the PPTT table, find and verify that the subtable entry
 * is located within the table
 */
static struct acpi_subtable_header *fetch_pptt_subtable(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	struct acpi_subtable_header *entry;

	/* there isn't a subtable at reference 0 */
	if (!pptt_ref)
		return NULL;

	if (pptt_ref + sizeof(struct acpi_subtable_header) > table_hdr->length)
		return NULL;

	entry = (struct acpi_subtable_header *)((u8 *)table_hdr + pptt_ref);

	if (pptt_ref + entry->length > table_hdr->length)
		return NULL;

	return entry;
}

static struct acpi_pptt_processor *fetch_pptt_node(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	return (struct acpi_pptt_processor *)fetch_pptt_subtable(table_hdr, pptt_ref);
}

static struct acpi_pptt_cache *fetch_pptt_cache(
	struct acpi_table_header *table_hdr, u32 pptt_ref)
{
	return (struct acpi_pptt_cache *)fetch_pptt_subtable(table_hdr, pptt_ref);
}

static struct acpi_subtable_header *acpi_get_pptt_resource(
	struct acpi_table_header *table_hdr,
	struct acpi_pptt_processor *node, int resource)
{
	u32 ref;

	if (resource >= node->number_of_priv_resources)
		return NULL;

	ref = *(u32 *)((u8 *)node + sizeof(struct acpi_pptt_processor) +
		      sizeof(u32) * resource);

	return fetch_pptt_subtable(table_hdr, ref);
}

/*
 * given a pptt resource, verify that it is a cache node, then walk
 * down each level of caches, counting how many levels are found
 * as well as checking the cache type (icache, dcache, unified). If a
 * level & type match, then we set found, and continue the search.
 * Once the entire cache branch has been walked return its max
 * depth.
 */
static int acpi_pptt_walk_cache(struct acpi_table_header *table_hdr,
				int local_level,
				struct acpi_subtable_header *res,
				struct acpi_pptt_cache **found,
				int level, int type)
{
	struct acpi_pptt_cache *cache;

	if (res->type != ACPI_PPTT_TYPE_CACHE)
		return 0;

	cache = (struct acpi_pptt_cache *) res;
	while (cache) {
		local_level++;

		if ((local_level == level) &&
		    (cache->flags & ACPI_PPTT_CACHE_TYPE_VALID) &&
		    ((cache->attributes & ACPI_PPTT_MASK_CACHE_TYPE) == type)) {
			if (*found != NULL)
				pr_err("Found duplicate cache level/type unable to determine uniqueness\n");

			pr_debug("Found cache @ level %d\n", level);
			*found = cache;
			/*
			 * continue looking at this node's resource list
			 * to verify that we don't find a duplicate
			 * cache node.
			 */
		}
		cache = fetch_pptt_cache(table_hdr, cache->next_level_of_cache);
	}
	return local_level;
}

/*
 * Given a CPU node look for cache levels that exist at this level, and then
 * for each cache node, count how many levels exist below (logically above) it.
 * If a level and type are specified, and we find that level/type, abort
 * processing and return the acpi_pptt_cache structure.
 */
static struct acpi_pptt_cache *acpi_find_cache_level(
	struct acpi_table_header *table_hdr,
	struct acpi_pptt_processor *cpu_node,
	int *starting_level, int level, int type)
{
	struct acpi_subtable_header *res;
	int number_of_levels = *starting_level;
	int resource = 0;
	struct acpi_pptt_cache *ret = NULL;
	int local_level;

	/* walk down from processor node */
	while ((res = acpi_get_pptt_resource(table_hdr, cpu_node, resource))) {
		resource++;

		local_level = acpi_pptt_walk_cache(table_hdr, *starting_level,
						   res, &ret, level, type);
		/*
		 * we are looking for the max depth. Since its potentially
		 * possible for a given node to have resources with differing
		 * depths verify that the depth we have found is the largest.
		 */
		if (number_of_levels < local_level)
			number_of_levels = local_level;
	}
	if (number_of_levels > *starting_level)
		*starting_level = number_of_levels;

	return ret;
}

/*
 * given a processor node containing a processing unit, walk into it and count
 * how many levels exist solely for it, and then walk up each level until we hit
 * the root node (ignore the package level because it may be possible to have
 * caches that exist across packages. Count the number of cache levels that
 * exist at each level on the way up.
 */
static int acpi_process_node(struct acpi_table_header *table_hdr,
			     struct acpi_pptt_processor *cpu_node)
{
	int total_levels = 0;

	do {
		acpi_find_cache_level(table_hdr, cpu_node, &total_levels, 0, 0);
		cpu_node = fetch_pptt_node(table_hdr, cpu_node->parent);
	} while (cpu_node);

	return total_levels;
}

/*
 * Find the subtable entry describing the provided processor
 */
static struct acpi_pptt_processor *acpi_find_processor_node(
	struct acpi_table_header *table_hdr,
	u32 acpi_cpu_id)
{
	struct acpi_subtable_header *entry;
	unsigned long table_end;
	struct acpi_pptt_processor *cpu_node;

	table_end = (unsigned long)table_hdr + table_hdr->length;
	entry = (struct acpi_subtable_header *)((u8 *)table_hdr + sizeof(struct acpi_table_pptt));

	/* find the processor structure associated with this cpuid */
	while (((unsigned long)entry) + sizeof(struct acpi_subtable_header) < table_end) {
		cpu_node = (struct acpi_pptt_processor *)entry;

		if ((entry->type == ACPI_PPTT_TYPE_PROCESSOR) &&
		    (cpu_node->flags & ACPI_PPTT_ACPI_PROCESSOR_ID_VALID)) {
			pr_debug("checking phy_cpu_id %d against acpi id %d\n",
				 acpi_cpu_id, cpu_node->acpi_processor_id);
			if (acpi_cpu_id == cpu_node->acpi_processor_id) {
				/* found the correct entry */
				pr_debug("match found!\n");
				return (struct acpi_pptt_processor *)entry;
			}
		}

		if (entry->length == 0) {
			pr_err("Invalid zero length subtable\n");
			break;
		}
		entry = (struct acpi_subtable_header *)
			((u8 *)entry + entry->length);
	}
	return NULL;
}

static int acpi_parse_pptt(struct acpi_table_header *table_hdr, u32 acpi_cpu_id)
{
	int number_of_levels = 0;
	struct acpi_pptt_processor *cpu;

	cpu = acpi_find_processor_node(table_hdr, acpi_cpu_id);
	if (cpu)
		number_of_levels = acpi_process_node(table_hdr, cpu);

	return number_of_levels;
}

#define ACPI_6_2_CACHE_TYPE_DATA		      (0x0)
#define ACPI_6_2_CACHE_TYPE_INSTR		      (1<<2)
#define ACPI_6_2_CACHE_TYPE_UNIFIED		      (1<<3)
#define ACPI_6_2_CACHE_POLICY_WB		      (0x0)
#define ACPI_6_2_CACHE_POLICY_WT		      (1<<4)
#define ACPI_6_2_CACHE_READ_ALLOCATE		      (0x0)
#define ACPI_6_2_CACHE_WRITE_ALLOCATE		      (0x01)
#define ACPI_6_2_CACHE_RW_ALLOCATE		      (0x02)

static u8 acpi_cache_type(enum cache_type type)
{
	switch (type) {
	case CACHE_TYPE_DATA:
		pr_debug("Looking for data cache\n");
		return ACPI_6_2_CACHE_TYPE_DATA;
	case CACHE_TYPE_INST:
		pr_debug("Looking for instruction cache\n");
		return ACPI_6_2_CACHE_TYPE_INSTR;
	default:
		pr_err("Unknown cache type, assume unified\n");
	case CACHE_TYPE_UNIFIED:
		pr_debug("Looking for unified cache\n");
		return ACPI_6_2_CACHE_TYPE_UNIFIED;
	}
}

/* find the ACPI node describing the cache type/level for the given CPU */
static struct acpi_pptt_cache *acpi_find_cache_node(
	struct acpi_table_header *table_hdr, u32 acpi_cpu_id,
	enum cache_type type, unsigned int level)
{
	int total_levels = 0;
	struct acpi_pptt_cache *found = NULL;
	struct acpi_pptt_processor *cpu_node;
	u8 acpi_type = acpi_cache_type(type);

	pr_debug("Looking for CPU %d's level %d cache type %d\n",
		 acpi_cpu_id, level, acpi_type);

	cpu_node = acpi_find_processor_node(table_hdr, acpi_cpu_id);
	if (!cpu_node)
		return NULL;

	do {
		found = acpi_find_cache_level(table_hdr, cpu_node, &total_levels, level, acpi_type);
		cpu_node = fetch_pptt_node(table_hdr, cpu_node->parent);
	} while ((cpu_node) && (!found));

	return found;
}

int acpi_find_last_cache_level(unsigned int cpu)
{
	u32 acpi_cpu_id;
	struct acpi_table_header *table;
	int number_of_levels = 0;
	acpi_status status;

	pr_debug("Cache Setup find last level cpu=%d\n", cpu);

	acpi_cpu_id = acpi_cpu_get_madt_gicc(cpu)->uid;
	status = acpi_get_table(ACPI_SIG_PPTT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err_once("No PPTT table found, cache topology may be inaccurate\n");
	} else {
		number_of_levels = acpi_parse_pptt(table, acpi_cpu_id);
		acpi_put_table(table);
	}
	pr_debug("Cache Setup find last level level=%d\n", number_of_levels);

	return number_of_levels;
}

/*
 * The ACPI spec implies that the fields in the cache structures are used to
 * extend and correct the information probed from the hardware. In the case
 * of arm64 the CCSIDR probing has been removed because it might be incorrect.
 */
static void update_cache_properties(struct cacheinfo *this_leaf,
				    struct acpi_pptt_cache *found_cache)
{
	this_leaf->of_node = (struct device_node *)found_cache;
	if (found_cache->flags & ACPI_PPTT_SIZE_PROPERTY_VALID)
		this_leaf->size = found_cache->size;
	if (found_cache->flags & ACPI_PPTT_LINE_SIZE_VALID)
		this_leaf->coherency_line_size = found_cache->line_size;
	if (found_cache->flags & ACPI_PPTT_NUMBER_OF_SETS_VALID)
		this_leaf->number_of_sets = found_cache->number_of_sets;
	if (found_cache->flags & ACPI_PPTT_ASSOCIATIVITY_VALID)
		this_leaf->ways_of_associativity = found_cache->associativity;
	if (found_cache->flags & ACPI_PPTT_WRITE_POLICY_VALID)
		switch (found_cache->attributes & ACPI_PPTT_MASK_WRITE_POLICY) {
		case ACPI_6_2_CACHE_POLICY_WT:
			this_leaf->attributes = CACHE_WRITE_THROUGH;
			break;
		case ACPI_6_2_CACHE_POLICY_WB:
			this_leaf->attributes = CACHE_WRITE_BACK;
			break;
		default:
			pr_err("Unknown ACPI cache policy %d\n",
			      found_cache->attributes & ACPI_PPTT_MASK_WRITE_POLICY);
		}
	if (found_cache->flags & ACPI_PPTT_ALLOCATION_TYPE_VALID)
		switch (found_cache->attributes & ACPI_PPTT_MASK_ALLOCATION_TYPE) {
		case ACPI_6_2_CACHE_READ_ALLOCATE:
			this_leaf->attributes |= CACHE_READ_ALLOCATE;
			break;
		case ACPI_6_2_CACHE_WRITE_ALLOCATE:
			this_leaf->attributes |= CACHE_WRITE_ALLOCATE;
			break;
		case ACPI_6_2_CACHE_RW_ALLOCATE:
			this_leaf->attributes |=
				CACHE_READ_ALLOCATE|CACHE_WRITE_ALLOCATE;
			break;
		default:
			pr_err("Unknown ACPI cache allocation policy %d\n",
			   found_cache->attributes & ACPI_PPTT_MASK_ALLOCATION_TYPE);
		}
}

static void cache_setup_acpi_cpu(struct acpi_table_header *table,
				 unsigned int cpu)
{
	struct acpi_pptt_cache *found_cache;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	u32 acpi_cpu_id = acpi_cpu_get_madt_gicc(cpu)->uid;
	struct cacheinfo *this_leaf;
	unsigned int index = 0;

	while (index < get_cpu_cacheinfo(cpu)->num_leaves) {
		this_leaf = this_cpu_ci->info_list + index;
		found_cache = acpi_find_cache_node(table, acpi_cpu_id,
						   this_leaf->type,
						   this_leaf->level);
		pr_debug("found = %p\n", found_cache);
		if (found_cache)
			update_cache_properties(this_leaf, found_cache);

		index++;
	}
}

/*
 * simply assign a ACPI cache entry to each known CPU cache entry
 * determining which entries are shared is done later
 */
int cache_setup_acpi(unsigned int cpu)
{
	struct acpi_table_header *table;
	acpi_status status;

	pr_debug("Cache Setup ACPI cpu %d\n", cpu);

	status = acpi_get_table(ACPI_SIG_PPTT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err_once("No PPTT table found, cache topology may be inaccurate\n");
		return -ENOENT;
	}

	cache_setup_acpi_cpu(table, cpu);
	acpi_put_table(table);

	return 0;
}
