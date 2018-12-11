// SPDX-License-Identifier: GPL-2.0
/*
 * Heterogeneous Memory Attributes Table (HMAT) representation
 *
 * Copyright (c) 2018, Intel Corporation.
 */

#include <acpi/acpi_numa.h>
#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/node.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

static LIST_HEAD(targets);

struct memory_target {
	struct list_head node;
	unsigned int memory_pxm;
	unsigned long p_nodes[BITS_TO_LONGS(MAX_NUMNODES)];
	bool hmem_valid;
	struct node_hmem_attrs hmem;
};

static __init struct memory_target *find_mem_target(unsigned int m)
{
	struct memory_target *t;

	list_for_each_entry(t, &targets, node)
		if (t->memory_pxm == m)
			return t;
	return NULL;
}

static __init void alloc_memory_target(unsigned int mem_pxm)
{
	struct memory_target *t;

	if (pxm_to_node(mem_pxm) == NUMA_NO_NODE)
		return;

	t = find_mem_target(mem_pxm);
	if (t)
		return;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return;

	t->memory_pxm = mem_pxm;
	list_add_tail(&t->node, &targets);
}

static __init const char *hmat_data_type(u8 type)
{
	switch (type) {
	case ACPI_HMAT_ACCESS_LATENCY:
		return "Access Latency";
	case ACPI_HMAT_READ_LATENCY:
		return "Read Latency";
	case ACPI_HMAT_WRITE_LATENCY:
		return "Write Latency";
	case ACPI_HMAT_ACCESS_BANDWIDTH:
		return "Access Bandwidth";
	case ACPI_HMAT_READ_BANDWIDTH:
		return "Read Bandwidth";
	case ACPI_HMAT_WRITE_BANDWIDTH:
		return "Write Bandwidth";
	default:
		return "Reserved";
	};
}

static __init const char *hmat_data_type_suffix(u8 type)
{
	switch (type) {
	case ACPI_HMAT_ACCESS_LATENCY:
	case ACPI_HMAT_READ_LATENCY:
	case ACPI_HMAT_WRITE_LATENCY:
		return " nsec";
	case ACPI_HMAT_ACCESS_BANDWIDTH:
	case ACPI_HMAT_READ_BANDWIDTH:
	case ACPI_HMAT_WRITE_BANDWIDTH:
		return " MB/s";
	default:
		return "";
	};
}

static __init void hmat_update_access(u8 type, u32 value, u32 *best)
{
	switch (type) {
	case ACPI_HMAT_ACCESS_LATENCY:
	case ACPI_HMAT_READ_LATENCY:
	case ACPI_HMAT_WRITE_LATENCY:
		if (!*best || *best > value)
			*best = value;
		break;
	case ACPI_HMAT_ACCESS_BANDWIDTH:
	case ACPI_HMAT_READ_BANDWIDTH:
	case ACPI_HMAT_WRITE_BANDWIDTH:
		if (!*best || *best < value)
			*best = value;
		break;
	}
}

static __init void hmat_update_target(struct memory_target *t, u8 type,
				      u32 value)
{
	switch (type) {
	case ACPI_HMAT_ACCESS_LATENCY:
		t->hmem.read_latency = value;
		t->hmem.write_latency = value;
		break;
	case ACPI_HMAT_READ_LATENCY:
		t->hmem.read_latency = value;
		break;
	case ACPI_HMAT_WRITE_LATENCY:
		t->hmem.write_latency = value;
		break;
	case ACPI_HMAT_ACCESS_BANDWIDTH:
		t->hmem.read_bandwidth = value;
		t->hmem.write_bandwidth = value;
		break;
	case ACPI_HMAT_READ_BANDWIDTH:
		t->hmem.read_bandwidth = value;
		break;
	case ACPI_HMAT_WRITE_BANDWIDTH:
		t->hmem.write_bandwidth = value;
		break;
	}
	t->hmem_valid = true;
}

static __init int hmat_parse_locality(union acpi_subtable_headers *header,
				      const unsigned long end)
{
	struct memory_target *t;
	struct acpi_hmat_locality *loc = (void *)header;
	unsigned int init, targ, pass, p_node, total_size, ipds, tpds;
	u32 *inits, *targs, value;
	u16 *entries;
	u8 type;

	if (loc->header.length < sizeof(*loc)) {
		pr_err("HMAT: Unexpected locality header length: %d\n",
			loc->header.length);
		return -EINVAL;
	}

	type = loc->data_type;
	ipds = loc->number_of_initiator_Pds;
	tpds = loc->number_of_target_Pds;
	total_size = sizeof(*loc) + sizeof(*entries) * ipds * tpds +
		     sizeof(*inits) * ipds + sizeof(*targs) * tpds;
	if (loc->header.length < total_size) {
		pr_err("HMAT: Unexpected locality header length:%d, minimum required:%d\n",
			loc->header.length, total_size);
		return -EINVAL;
	}

	pr_info("HMAT: Locality: Flags:%02x Type:%s Initiator Domains:%d Target Domains:%d Base:%lld\n",
		loc->flags, hmat_data_type(type), ipds, tpds,
		loc->entry_base_unit);

	inits = (u32 *)(loc + 1);
	targs = &inits[ipds];
	entries = (u16 *)(&targs[tpds]);
	for (targ = 0; targ < tpds; targ++) {
		u32 best = 0;

		t = find_mem_target(targs[targ]);
		for (pass = 0; pass < 2; pass++) {
			for (init = 0; init < ipds; init++) {
				value = entries[init * tpds + targ];
				value = (value * loc->entry_base_unit) / 10;

				if (!pass) {
					hmat_update_access(type, value, &best);
					pr_info("  Initiator-Target[%d-%d]:%d%s\n",
						inits[init], targs[targ], value,
						hmat_data_type_suffix(type));
					continue;
				}

				if (!t)
					continue;
				p_node = pxm_to_node(inits[init]);
				if (p_node != NUMA_NO_NODE && value == best)
					set_bit(p_node, t->p_nodes);
			}
		}
		if (t && best)
			hmat_update_target(t, type, best);
	}
	return 0;
}

static __init int hmat_parse_cache(union acpi_subtable_headers *header,
				   const unsigned long end)
{
	struct acpi_hmat_cache *cache = (void *)header;
	struct node_cache_attrs cache_attrs;
	u32 attrs;

	if (cache->header.length < sizeof(*cache)) {
		pr_err("HMAT: Unexpected cache header length: %d\n",
			cache->header.length);
		return -EINVAL;
	}

	attrs = cache->cache_attributes;
	pr_info("HMAT: Cache: Domain:%d Size:%llu Attrs:%08x SMBIOS Handles:%d\n",
		cache->memory_PD, cache->cache_size, attrs,
		cache->number_of_SMBIOShandles);

	cache_attrs.size = cache->cache_size;
	cache_attrs.level = (attrs & ACPI_HMAT_CACHE_LEVEL) >> 4;
	cache_attrs.line_size = (attrs & ACPI_HMAT_CACHE_LINE_SIZE) >> 16;

	switch ((attrs & ACPI_HMAT_CACHE_ASSOCIATIVITY) >> 8) {
	case ACPI_HMAT_CA_DIRECT_MAPPED:
		cache_attrs.associativity = NODE_CACHE_DIRECT_MAP;
		break;
	case ACPI_HMAT_CA_COMPLEX_CACHE_INDEXING:
		cache_attrs.associativity = NODE_CACHE_INDEXED;
		break;
	case ACPI_HMAT_CA_NONE:
	default:
		cache_attrs.associativity = NODE_CACHE_OTHER;
		break;
	}

	switch ((attrs & ACPI_HMAT_WRITE_POLICY) >> 12) {
	case ACPI_HMAT_CP_WB:
		cache_attrs.write_policy = NODE_CACHE_WRITE_BACK;
		break;
	case ACPI_HMAT_CP_WT:
		cache_attrs.write_policy = NODE_CACHE_WRITE_THROUGH;
		break;
	case ACPI_HMAT_CP_NONE:
	default:
		cache_attrs.write_policy = NODE_CACHE_WRITE_OTHER;
		break;
	}

	node_add_cache(pxm_to_node(cache->memory_PD), &cache_attrs);
	return 0;
}

static int __init hmat_parse_address_range(union acpi_subtable_headers *header,
					   const unsigned long end)
{
	struct acpi_hmat_address_range *spa = (void *)header;
	struct memory_target *t = NULL;

	if (spa->header.length != sizeof(*spa)) {
		pr_err("HMAT: Unexpected address range header length: %d\n",
			spa->header.length);
		return -EINVAL;
	}
	pr_info("HMAT: Memory (%#llx length %#llx) Flags:%04x Processor Domain:%d Memory Domain:%d\n",
		spa->physical_address_base, spa->physical_address_length,
		spa->flags, spa->processor_PD, spa->memory_PD);

	if (spa->flags & ACPI_HMAT_MEMORY_PD_VALID) {
		t = find_mem_target(spa->memory_PD);
		if (!t) {
			pr_warn("HMAT: Memory Domain missing from SRAT\n");
			return -EINVAL;
		}
	}
	if (t && spa->flags & ACPI_HMAT_PROCESSOR_PD_VALID) {
		int p_node = pxm_to_node(spa->processor_PD);

		if (p_node == NUMA_NO_NODE) {
			pr_warn("HMAT: Invalid Processor Domain\n");
			return -EINVAL;
		}
		set_bit(p_node, t->p_nodes);
	}
	return 0;
}

static int __init hmat_parse_subtable(union acpi_subtable_headers *header,
				      const unsigned long end)
{
	struct acpi_hmat_structure *hdr = (void *)header;

	if (!hdr)
		return -EINVAL;

	switch (hdr->type) {
	case ACPI_HMAT_TYPE_ADDRESS_RANGE:
		return hmat_parse_address_range(header, end);
	case ACPI_HMAT_TYPE_LOCALITY:
		return hmat_parse_locality(header, end);
	case ACPI_HMAT_TYPE_CACHE:
		return hmat_parse_cache(header, end);
	default:
		return -EINVAL;
	}
}

static __init int srat_parse_mem_affinity(union acpi_subtable_headers *header,
					  const unsigned long end)
{
	struct acpi_srat_mem_affinity *ma = (void *)header;

	if (!ma)
		return -EINVAL;
	if (!(ma->flags & ACPI_SRAT_MEM_ENABLED))
		return 0;
	alloc_memory_target(ma->proximity_domain);
	return 0;
}

static __init void hmat_register_targets(void)
{
	struct memory_target *t, *next;
	unsigned m, p;

	list_for_each_entry_safe(t, next, &targets, node) {
		list_del(&t->node);
		m = pxm_to_node(t->memory_pxm);
		for_each_set_bit(p, t->p_nodes, MAX_NUMNODES)
			register_memory_node_under_compute_node(m, p);
		if (t->hmem_valid)
			node_set_perf_attrs(m, &t->hmem);
		kfree(t);
	}
}

static __init int parse_noop(struct acpi_table_header *table)
{
	return 0;
}

static __init int hmat_init(void)
{
	struct acpi_subtable_proc subtable_proc;
	struct acpi_table_header *tbl;
	enum acpi_hmat_type i;
	acpi_status status;

	if (srat_disabled())
		return 0;

	status = acpi_get_table(ACPI_SIG_SRAT, 0, &tbl);
	if (ACPI_FAILURE(status))
		return 0;

	if (acpi_table_parse(ACPI_SIG_SRAT, parse_noop))
		goto out_put;

	memset(&subtable_proc, 0, sizeof(subtable_proc));
	subtable_proc.id = ACPI_SRAT_TYPE_MEMORY_AFFINITY;
	subtable_proc.handler = srat_parse_mem_affinity;

	if (acpi_table_parse_entries_array(ACPI_SIG_SRAT,
				sizeof(struct acpi_table_srat),
				&subtable_proc, 1, 0) < 0)
		goto out_put;
	acpi_put_table(tbl);

	status = acpi_get_table(ACPI_SIG_HMAT, 0, &tbl);
	if (ACPI_FAILURE(status))
		return 0;

	if (acpi_table_parse(ACPI_SIG_HMAT, parse_noop))
		goto out_put;

	memset(&subtable_proc, 0, sizeof(subtable_proc));
	subtable_proc.handler = hmat_parse_subtable;
	for (i = ACPI_HMAT_TYPE_ADDRESS_RANGE; i < ACPI_HMAT_TYPE_RESERVED; i++) {
		subtable_proc.id = i;
		if (acpi_table_parse_entries_array(ACPI_SIG_HMAT,
					sizeof(struct acpi_table_hmat),
					&subtable_proc, 1, 0) < 0)
			goto out_put;
	}
	hmat_register_targets();
 out_put:
	acpi_put_table(tbl);
	return 0;
}
subsys_initcall(hmat_init);
