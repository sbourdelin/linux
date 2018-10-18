/*
 * Dynamic reconfiguration memory support
 *
 * Copyright 2017 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "drmem: " fmt

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/memblock.h>
#include <asm/prom.h>
#include <asm/drmem.h>

static struct drmem_lmb_info __drmem_info;
struct drmem_lmb_info *drmem_info = &__drmem_info;
static int n_root_addr_cells;

u64 drmem_lmb_memory_max(void)
{
	struct drmem_lmb *last_lmb;

	last_lmb = &drmem_info->lmbs[drmem_info->n_lmbs - 1];
	return last_lmb->base_addr + drmem_lmb_size();
}

static u32 drmem_lmb_flags(struct drmem_lmb *lmb)
{
	/*
	 * Return the value of the lmb flags field minus the reserved
	 * bit used internally for hotplug processing.
	 */
	return lmb->flags & ~DRMEM_LMB_RESERVED;
}

static struct property *clone_property(struct property *prop, u32 prop_sz)
{
	struct property *new_prop;

	new_prop = kzalloc(sizeof(*new_prop), GFP_KERNEL);
	if (!new_prop)
		return NULL;

	new_prop->name = kstrdup(prop->name, GFP_KERNEL);
	new_prop->value = kzalloc(prop_sz, GFP_KERNEL);
	if (!new_prop->name || !new_prop->value) {
		kfree(new_prop->name);
		kfree(new_prop->value);
		kfree(new_prop);
		return NULL;
	}

	new_prop->length = prop_sz;
#if defined(CONFIG_OF_DYNAMIC)
	of_property_set_flag(new_prop, OF_DYNAMIC);
#endif
	return new_prop;
}

static int drmem_update_dt_v1(struct device_node *memory,
			      struct property *prop)
{
	struct property *new_prop;
	struct of_drconf_cell_v1 *dr_cell;
	struct drmem_lmb *lmb;
	u32 *p;

	new_prop = clone_property(prop, prop->length);
	if (!new_prop)
		return -1;

	p = new_prop->value;
	*p++ = cpu_to_be32(drmem_info->n_lmbs);

	dr_cell = (struct of_drconf_cell_v1 *)p;

	for_each_drmem_lmb(lmb) {
		dr_cell->base_addr = cpu_to_be64(lmb->base_addr);
		dr_cell->drc_index = cpu_to_be32(lmb->drc_index);
		dr_cell->aa_index = cpu_to_be32(lmb->aa_index);
		dr_cell->flags = cpu_to_be32(drmem_lmb_flags(lmb));

		dr_cell++;
	}

	of_update_property(memory, new_prop);
	return 0;
}

static void init_drconf_v2_cell(struct of_drconf_cell_v2 *dr_cell,
				struct drmem_lmb *lmb)
{
	dr_cell->base_addr = cpu_to_be64(lmb->base_addr);
	dr_cell->drc_index = cpu_to_be32(lmb->drc_index);
	dr_cell->aa_index = cpu_to_be32(lmb->aa_index);
	dr_cell->flags = cpu_to_be32(drmem_lmb_flags(lmb));
}

static int drmem_update_dt_v2(struct device_node *memory,
			      struct property *prop)
{
	struct property *new_prop;
	struct of_drconf_cell_v2 *dr_cell;
	struct drmem_lmb *lmb, *prev_lmb;
	u32 lmb_sets, prop_sz, seq_lmbs;
	u32 *p;

	/* First pass, determine how many LMB sets are needed. */
	lmb_sets = 0;
	prev_lmb = NULL;
	for_each_drmem_lmb(lmb) {
		if (!prev_lmb) {
			prev_lmb = lmb;
			lmb_sets++;
			continue;
		}

		if (prev_lmb->aa_index != lmb->aa_index ||
		    drmem_lmb_flags(prev_lmb) != drmem_lmb_flags(lmb))
			lmb_sets++;

		prev_lmb = lmb;
	}

	prop_sz = lmb_sets * sizeof(*dr_cell) + sizeof(__be32);
	new_prop = clone_property(prop, prop_sz);
	if (!new_prop)
		return -1;

	p = new_prop->value;
	*p++ = cpu_to_be32(lmb_sets);

	dr_cell = (struct of_drconf_cell_v2 *)p;

	/* Second pass, populate the LMB set data */
	prev_lmb = NULL;
	seq_lmbs = 0;
	for_each_drmem_lmb(lmb) {
		if (prev_lmb == NULL) {
			/* Start of first LMB set */
			prev_lmb = lmb;
			init_drconf_v2_cell(dr_cell, lmb);
			seq_lmbs++;
			continue;
		}

		if (prev_lmb->aa_index != lmb->aa_index ||
		    drmem_lmb_flags(prev_lmb) != drmem_lmb_flags(lmb)) {
			/* end of one set, start of another */
			dr_cell->seq_lmbs = cpu_to_be32(seq_lmbs);
			dr_cell++;

			init_drconf_v2_cell(dr_cell, lmb);
			seq_lmbs = 1;
		} else {
			seq_lmbs++;
		}

		prev_lmb = lmb;
	}

	/* close out last LMB set */
	dr_cell->seq_lmbs = cpu_to_be32(seq_lmbs);
	of_update_property(memory, new_prop);
	return 0;
}

int drmem_update_dt(void)
{
	struct device_node *memory;
	struct property *prop;
	int rc = -1;

	memory = of_find_node_by_path("/ibm,dynamic-reconfiguration-memory");
	if (!memory)
		return -1;

	prop = of_find_property(memory, "ibm,dynamic-memory", NULL);
	if (prop) {
		rc = drmem_update_dt_v1(memory, prop);
	} else {
		prop = of_find_property(memory, "ibm,dynamic-memory-v2", NULL);
		if (prop)
			rc = drmem_update_dt_v2(memory, prop);
	}

	of_node_put(memory);
	return rc;
}

static void read_drconf_v1_cell(struct drmem_lmb *lmb,
				       const __be32 **prop)
{
	const __be32 *p = *prop;

	lmb->base_addr = of_read_number(p, n_root_addr_cells);
	p += n_root_addr_cells;
	lmb->drc_index = of_read_number(p++, 1);

	p++; /* skip reserved field */

	lmb->aa_index = of_read_number(p++, 1);
	lmb->flags = of_read_number(p++, 1);
	lmb->internal_flags = 0;

	*prop = p;
}

static void __walk_drmem_v1_lmbs(const __be32 *prop, const __be32 *usm,
			void (*func)(struct drmem_lmb *, const __be32 **))
{
	struct drmem_lmb lmb;
	u32 i, n_lmbs;

	n_lmbs = of_read_number(prop++, 1);
	if (n_lmbs == 0)
		return;

	for (i = 0; i < n_lmbs; i++) {
		read_drconf_v1_cell(&lmb, &prop);
		func(&lmb, &usm);
	}
}

static void read_drconf_v2_cell(struct of_drconf_cell_v2 *dr_cell,
				       const __be32 **prop)
{
	const __be32 *p = *prop;

	dr_cell->seq_lmbs = of_read_number(p++, 1);
	dr_cell->base_addr = of_read_number(p, n_root_addr_cells);
	p += n_root_addr_cells;
	dr_cell->drc_index = of_read_number(p++, 1);
	dr_cell->aa_index = of_read_number(p++, 1);
	dr_cell->flags = of_read_number(p++, 1);

	*prop = p;
}

static void __walk_drmem_v2_lmbs(const __be32 *prop, const __be32 *usm,
			void (*func)(struct drmem_lmb *, const __be32 **))
{
	struct of_drconf_cell_v2 dr_cell;
	struct drmem_lmb lmb;
	u32 i, j, lmb_sets;

	lmb_sets = of_read_number(prop++, 1);
	if (lmb_sets == 0)
		return;

	for (i = 0; i < lmb_sets; i++) {
		read_drconf_v2_cell(&dr_cell, &prop);

		for (j = 0; j < dr_cell.seq_lmbs; j++) {
			lmb.base_addr = dr_cell.base_addr;
			dr_cell.base_addr += drmem_lmb_size();

			lmb.drc_index = dr_cell.drc_index;
			dr_cell.drc_index++;

			lmb.aa_index = dr_cell.aa_index;
			lmb.flags = dr_cell.flags;
			lmb.internal_flags = 0;

			func(&lmb, &usm);
		}
	}
}

#ifdef CONFIG_PPC_PSERIES
void __init walk_drmem_lmbs_early(unsigned long node,
			void (*func)(struct drmem_lmb *, const __be32 **))
{
	const __be32 *prop, *usm;
	int len;

	if (n_root_addr_cells == 0)
		n_root_addr_cells = dt_root_addr_cells;

	prop = of_get_flat_dt_prop(node, "ibm,lmb-size", &len);
	if (!prop || len < dt_root_size_cells * sizeof(__be32))
		return;

	drmem_info->lmb_size = dt_mem_next_cell(dt_root_size_cells, &prop);

	usm = of_get_flat_dt_prop(node, "linux,drconf-usable-memory", &len);

	prop = of_get_flat_dt_prop(node, "ibm,dynamic-memory", &len);
	if (prop) {
		__walk_drmem_v1_lmbs(prop, usm, func);
	} else {
		prop = of_get_flat_dt_prop(node, "ibm,dynamic-memory-v2",
					   &len);
		if (prop)
			__walk_drmem_v2_lmbs(prop, usm, func);
	}

	memblock_dump_all();
}

#endif

static int __init init_drmem_lmb_size(struct device_node *dn)
{
	const __be32 *prop;
	int len;

	if (drmem_info->lmb_size)
		return 0;

	prop = of_get_property(dn, "ibm,lmb-size", &len);
	if (!prop || len < dt_root_size_cells * sizeof(__be32)) {
		pr_info("Could not determine LMB size\n");
		return -1;
	}

	drmem_info->lmb_size = dt_mem_next_cell(dt_root_size_cells, &prop);
	return 0;
}

/*
 * Returns the property linux,drconf-usable-memory if
 * it exists (the property exists only in kexec/kdump kernels,
 * added by kexec-tools)
 */
static const __be32 *of_get_usable_memory(struct device_node *dn)
{
	const __be32 *prop;
	u32 len;

	prop = of_get_property(dn, "linux,drconf-usable-memory", &len);
	if (!prop || len < sizeof(unsigned int))
		return NULL;

	return prop;
}

void __init walk_drmem_lmbs(struct device_node *dn,
			    void (*func)(struct drmem_lmb *, const __be32 **))
{
	const __be32 *prop, *usm;

	if (init_drmem_lmb_size(dn))
		return;

	usm = of_get_usable_memory(dn);

	prop = of_get_property(dn, "ibm,dynamic-memory", NULL);
	if (prop) {
		__walk_drmem_v1_lmbs(prop, usm, func);
	} else {
		prop = of_get_property(dn, "ibm,dynamic-memory-v2", NULL);
		if (prop)
			__walk_drmem_v2_lmbs(prop, usm, func);
	}
}

int walk_drmem_lmbs_pairs(struct drmem_lmb_info *dinfo_oth,
			int (*func)(struct drmem_lmb *cnt,
				     struct drmem_lmb *oth,
				     void *data),
			void *data)
{
	struct drmem_lmb *lmb1, *lmb2;
	int rc = 0;

	for ((lmb1) = (&drmem_info->lmbs[0]), (lmb2) = (&dinfo_oth->lmbs[0]);
		((lmb1) <= (&drmem_info->lmbs[drmem_info->n_lmbs - 1])) &&
		((lmb2) <= (&dinfo_oth->lmbs[dinfo_oth->n_lmbs - 1]));
		(lmb1)++, (lmb2)++)
	{
		rc = func(lmb1, lmb2, data);
		if (rc <= 0)
			break;
	}
	return rc;
}

static void init_drmem_v1_lmbs(const __be32 *prop,
				struct drmem_lmb_info *dinfo)
{
	struct drmem_lmb *lmb;

	dinfo->n_lmbs = of_read_number(prop++, 1);
	if (dinfo->n_lmbs == 0)
		return;

	dinfo->lmbs = kcalloc(dinfo->n_lmbs, sizeof(*lmb),
				   GFP_KERNEL);
	if (!dinfo->lmbs)
		return;

	for_each_dinfo_lmb(dinfo, lmb)
		read_drconf_v1_cell(lmb, &prop);
}

static void init_drmem_v2_lmbs(const __be32 *prop,
				struct drmem_lmb_info *dinfo)
{
	struct drmem_lmb *lmb;
	struct of_drconf_cell_v2 dr_cell;
	const __be32 *p;
	u32 i, j, lmb_sets;
	int lmb_index;

	lmb_sets = of_read_number(prop++, 1);
	if (lmb_sets == 0)
		return;

	/* first pass, calculate the number of LMBs */
	p = prop;
	for (i = 0; i < lmb_sets; i++) {
		read_drconf_v2_cell(&dr_cell, &p);
		dinfo->n_lmbs += dr_cell.seq_lmbs;
	}

	dinfo->lmbs = kcalloc(dinfo->n_lmbs, sizeof(*lmb),
				   GFP_KERNEL);
	if (!dinfo->lmbs)
		return;

	/* second pass, read in the LMB information */
	lmb_index = 0;
	p = prop;

	for (i = 0; i < lmb_sets; i++) {
		read_drconf_v2_cell(&dr_cell, &p);

		for (j = 0; j < dr_cell.seq_lmbs; j++) {
			lmb = &dinfo->lmbs[lmb_index++];

			lmb->base_addr = dr_cell.base_addr;
			dr_cell.base_addr += dinfo->lmb_size;

			lmb->drc_index = dr_cell.drc_index;
			dr_cell.drc_index++;

			lmb->aa_index = dr_cell.aa_index;
			lmb->flags = dr_cell.flags;
			lmb->internal_flags = 0;
		}
	}
}

void drmem_lmbs_free(struct drmem_lmb_info *dinfo)
{
	if (dinfo) {
		kfree(dinfo->lmbs);
		kfree(dinfo);
	}
}

struct drmem_lmb_info *drmem_lmbs_init(struct property *prop)
{
	struct drmem_lmb_info *dinfo;

	dinfo = kzalloc(sizeof(*dinfo), GFP_KERNEL);
	if (!dinfo)
		return NULL;

	if (!strcmp("ibm,dynamic-memory", prop->name))
		init_drmem_v1_lmbs(prop->value, dinfo);
	else if (!strcmp("ibm,dynamic-memory-v2", prop->name))
		init_drmem_v2_lmbs(prop->value, dinfo);

	return dinfo;
}

static int __init drmem_init(void)
{
	struct device_node *dn;
	const __be32 *prop;

	if (n_root_addr_cells == 0)
		n_root_addr_cells = dt_root_addr_cells;

	dn = of_find_node_by_path("/ibm,dynamic-reconfiguration-memory");
	if (!dn) {
		pr_info("No dynamic reconfiguration memory found\n");
		return 0;
	}

	if (init_drmem_lmb_size(dn)) {
		of_node_put(dn);
		return 0;
	}

	prop = of_get_property(dn, "ibm,dynamic-memory", NULL);
	if (prop) {
		init_drmem_v1_lmbs(prop, drmem_info);
	} else {
		prop = of_get_property(dn, "ibm,dynamic-memory-v2", NULL);
		if (prop)
			init_drmem_v2_lmbs(prop, drmem_info);
	}

	of_node_put(dn);
	return 0;
}
late_initcall(drmem_init);
