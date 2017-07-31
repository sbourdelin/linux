/*
 * pSeries LMB support
 *
 * ** Add (C)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "lmb: " fmt

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/bootmem.h>
#include <asm/prom.h>
#include <asm/lmb.h>

struct lmb_data __lmb_data;
struct lmb_data *lmb_array = &__lmb_data;
int n_mem_addr_cells, n_mem_size_cells;

unsigned long read_n_cells(int n, const __be32 **buf)
{
	unsigned long result = 0;

	while (n--) {
		result = (result << 32) | of_read_number(*buf, 1);
		(*buf)++;
	}
	return result;
}

void __init get_n_mem_cells(int *n_addr_cells, int *n_size_cells)
{
	struct device_node *memory = NULL;

	memory = of_find_node_by_type(memory, "memory");
	if (!memory)
		panic("numa.c: No memory nodes found!");

	*n_addr_cells = of_n_addr_cells(memory);
	*n_size_cells = of_n_size_cells(memory);
	of_node_put(memory);
}

u32 lmb_get_lmb_size(void)
{
	return lmb_array->lmb_size;
}

u64 lmb_get_max_memory(void)
{
	u32 last_index = lmb_array->num_lmbs - 1;

	return lmb_array->lmbs[last_index].base_address + lmb_array->lmb_size;
}

/*
 * Retrieve and validate the ibm,dynamic-memory property of the device tree.
 *
 * The layout of the ibm,dynamic-memory property is a number N of memblock
 * list entries followed by N memblock list entries.  Each memblock list entry
 * contains information as laid out in the of_drconf_cell struct above.
 */
static int of_get_drconf_memory(struct device_node *memory, const __be32 **dm)
{
	const __be32 *prop;
	u32 len, entries;

	prop = of_get_property(memory, "ibm,dynamic-memory", &len);
	if (!prop || len < sizeof(unsigned int))
		return 0;

	entries = of_read_number(prop++, 1);

	/* Now that we know the number of entries, revalidate the size
	 * of the property read in to ensure we have everything
	 */
	if (len < (entries * (n_mem_addr_cells + 4) + 1) * sizeof(unsigned int))
		return 0;

	*dm = prop;
	return entries;
}

static int lmb_init_drconf_memory_v1(struct device_node *memory)
{
	const __be32 *dm;
	struct lmb *lmb;
	size_t lmb_array_sz;

	lmb_array->num_lmbs = of_get_drconf_memory(memory, &dm);
	if (!lmb_array->num_lmbs)
		return -1;

	lmb_array_sz = lmb_array->num_lmbs * sizeof(*lmb);
	lmb_array->lmbs = alloc_bootmem(lmb_array_sz);
	if (!lmb_array->lmbs) {
		pr_err("lmb array allocation failed\n");
		return -1;
	}

	for_each_lmb(lmb) {
		lmb->base_address = read_n_cells(n_mem_addr_cells, &dm);
		lmb->drc_index = of_read_number(dm++, 1);

		/* skip past the reserved field */
		dm++;

		lmb->aa_index = of_read_number(dm++, 1);
		lmb->flags = of_read_number(dm++, 1);

		pr_err("Init %llx, %x\n", lmb->base_address, lmb->drc_index);
	}

	return 0;
}

int lmb_init(void)
{
	const __be32 *prop;
	struct device_node *memory;
	int len;
	int rc = -1;

	pr_err("get mem node\n");
	memory = of_find_node_by_path("/ibm,dynamic-reconfiguration-memory");
	if (memory)
		rc = lmb_init_drconf_memory_v1(memory);

	if (rc) {
		of_node_put(memory);
		return rc;
	}

	prop = of_get_property(memory, "ibm,lmb-size", &len);
	if (prop)
		lmb_array->lmb_size = read_n_cells(n_mem_size_cells, &prop);

	of_node_put(memory);
	return 0;
}

