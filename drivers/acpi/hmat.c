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

static __init int hmat_parse_locality(union acpi_subtable_headers *header,
				      const unsigned long end)
{
	struct acpi_hmat_locality *loc = (void *)header;
	unsigned int init, targ, total_size, ipds, tpds;
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
		for (init = 0; init < ipds; init++) {
			value = entries[init * tpds + targ];
			value = (value * loc->entry_base_unit) / 10;
			pr_info("  Initiator-Target[%d-%d]:%d%s\n",
				inits[init], targs[targ], value,
				hmat_data_type_suffix(type));
		}
	}
	return 0;
}

static __init int hmat_parse_cache(union acpi_subtable_headers *header,
				   const unsigned long end)
{
	struct acpi_hmat_cache *cache = (void *)header;
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

	return 0;
}

static int __init hmat_parse_address_range(union acpi_subtable_headers *header,
					   const unsigned long end)
{
	struct acpi_hmat_address_range *spa = (void *)header;

	if (spa->header.length != sizeof(*spa)) {
		pr_err("HMAT: Unexpected address range header length: %d\n",
			spa->header.length);
		return -EINVAL;
	}
	pr_info("HMAT: Memory (%#llx length %#llx) Flags:%04x Processor Domain:%d Memory Domain:%d\n",
		spa->physical_address_base, spa->physical_address_length,
		spa->flags, spa->processor_PD, spa->memory_PD);
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
 out_put:
	acpi_put_table(tbl);
	return 0;
}
subsys_initcall(hmat_init);
