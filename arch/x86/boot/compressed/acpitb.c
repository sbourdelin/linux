// SPDX-License-Identifier: GPL-2.0
#define BOOT_CTYPE_H
#include "misc.h"

#include <linux/efi.h>
#include <asm/efi.h>
#include <linux/numa.h>
#include <linux/acpi.h>

extern unsigned long get_cmd_line_ptr(void);

#define STATIC
#include <linux/decompress/mm.h>

#ifdef CONFIG_MEMORY_HOTREMOVE
struct mem_vector {
	unsigned long long start;
	unsigned long long size;
};
/* Store the immovable memory regions */
struct mem_vector immovable_mem[MAX_NUMNODES*2];
#endif

#ifdef CONFIG_EFI
/* Search EFI table for rsdp table. */
static bool efi_get_rsdp_addr(acpi_physical_address *rsdp_addr)
{
	efi_system_table_t *systab;
	bool find_rsdp = false;
	bool efi_64 = false;
	void *config_tables;
	struct efi_info *e;
	char *sig;
	int size;
	int i;

	e = &boot_params->efi_info;
	sig = (char *)&e->efi_loader_signature;

	if (!strncmp(sig, EFI64_LOADER_SIGNATURE, 4))
		efi_64 = true;
	else if (!strncmp(sig, EFI32_LOADER_SIGNATURE, 4))
		efi_64 = false;
	else {
		debug_putstr("Wrong EFI loader signature.\n");
		return false;
	}

	/* Get systab from boot params. Based on efi_init(). */
#ifdef CONFIG_X86_32
	if (e->efi_systab_hi || e->efi_memmap_hi) {
		debug_putstr("Table located above 4GB, disabling EFI.\n");
		return false;
	}
	systab = (efi_system_table_t *)e->efi_systab;
#else
	systab = (efi_system_table_t *)(
			e->efi_systab | ((__u64)e->efi_systab_hi<<32));
#endif

	if (systab == NULL)
		return false;

	/*
	 * Get EFI tables from systab. Based on efi_config_init() and
	 * efi_config_parse_tables(). Only dig the useful tables but not
	 * do the initialization jobs.
	 */
	size = efi_64 ? sizeof(efi_config_table_64_t) :
			sizeof(efi_config_table_32_t);

	for (i = 0; i < systab->nr_tables; i++) {
		efi_guid_t guid;
		unsigned long table;

		config_tables = (void *)(systab->tables + size * i);
		if (efi_64) {
			efi_config_table_64_t *tmp_table;

			tmp_table = (efi_config_table_64_t *)config_tables;
			guid = tmp_table->guid;
			table = tmp_table->table;
#ifndef CONFIG_64BIT
			if (table >> 32) {
				debug_putstr("Table located above 4G, disabling EFI.\n");
				return false;
			}
#endif
		} else {
			efi_config_table_32_t *tmp_table;

			tmp_table = (efi_config_table_32_t *)config_tables;
			guid = tmp_table->guid;
			table = tmp_table->table;
		}

		/*
		 * Get rsdp from EFI tables.
		 * If ACPI20 table found, use it and return true.
		 * If ACPI20 table not found, but ACPI table found,
		 * use the ACPI table and return true.
		 * If neither ACPI table nor ACPI20 table found,
		 * return false.
		 */
		if (!(efi_guidcmp(guid, ACPI_TABLE_GUID))) {
			*rsdp_addr = (acpi_physical_address)table;
			find_rsdp = true;
		} else if (!(efi_guidcmp(guid, ACPI_20_TABLE_GUID))) {
			*rsdp_addr = (acpi_physical_address)table;
			return true;
		}
	}
	return find_rsdp;
}
#else
static bool efi_get_rsdp_addr(acpi_physical_address *rsdp_addr)
{
	return false;
}
#endif

static u8 checksum(u8 *buffer, u32 length)
{
	u8 sum = 0;
	u8 *end = buffer + length;

	while (buffer < end)
		sum = (u8)(sum + *(buffer++));

	return sum;
}

/*
 * Used to search a block of memory for the RSDP signature.
 * Return Pointer to the RSDP if found, otherwise NULL.
 * Based on acpi_tb_scan_memory_for_rsdp().
 */
static u8 *scan_mem_for_rsdp(u8 *start_address, u32 length)
{
	struct acpi_table_rsdp *rsdp;
	u8 *end_address;
	u8 *mem_rover;

	end_address = start_address + length;

	/* Search from given start address for the requested length */
	for (mem_rover = start_address; mem_rover < end_address;
	     mem_rover += ACPI_RSDP_SCAN_STEP) {
		/* The RSDP signature and checksum must both be correct */
		rsdp = (struct acpi_table_rsdp *)mem_rover;
		if (!ACPI_VALIDATE_RSDP_SIG(rsdp->signature))
			continue;
		if (checksum((u8 *) rsdp, ACPI_RSDP_CHECKSUM_LENGTH) != 0)
			continue;
		if ((rsdp->revision >= 2) &&
		    (checksum((u8 *) rsdp, ACPI_RSDP_XCHECKSUM_LENGTH) != 0))
			continue;

		/* Sig and checksum valid, we have found a real RSDP */
		return mem_rover;
	}
	return NULL;
}

/*
 * Used to search RSDP physical address.
 * Based on acpi_find_root_pointer(). Since only use physical address
 * in this period, so there is no need to do the memory map jobs.
 */
static void bios_get_rsdp_addr(acpi_physical_address *rsdp_addr)
{
	struct acpi_table_rsdp *rsdp;
	u8 *table_ptr;
	u8 *mem_rover;
	u32 address;

	/* Get the location of the Extended BIOS Data Area (EBDA) */
	table_ptr = (u8 *)ACPI_EBDA_PTR_LOCATION;
	*(u32 *)(void *)&address = *(u16 *)(void *)table_ptr;
	address <<= 4;
	table_ptr = (u8 *)(acpi_physical_address)address;

	/*
	 * Search EBDA paragraphs (EBDA is required to be a minimum of
	 * 1K length)
	 */
	if (address > 0x400) {
		mem_rover = scan_mem_for_rsdp(table_ptr, ACPI_EBDA_WINDOW_SIZE);

		if (mem_rover) {
			address += (u32)ACPI_PTR_DIFF(mem_rover, table_ptr);
			*rsdp_addr = (acpi_physical_address)address;
			return;
		}
	}

	table_ptr = (u8 *)ACPI_HI_RSDP_WINDOW_BASE;
	mem_rover = scan_mem_for_rsdp(table_ptr, ACPI_HI_RSDP_WINDOW_SIZE);

	/*
	 * Search upper memory: 16-byte boundaries in E0000h-FFFFFh
	 */
	if (mem_rover) {
		address = (u32)(ACPI_HI_RSDP_WINDOW_BASE +
				ACPI_PTR_DIFF(mem_rover, table_ptr));
		*rsdp_addr = (acpi_physical_address)address;
		return;
	}
}

#ifdef CONFIG_KEXEC
static bool get_acpi_rsdp(acpi_physical_address *rsdp_addr)
{
	char *args = (char *)get_cmd_line_ptr();
	size_t len = strlen((char *)args);
	char *tmp_cmdline, *param, *val;
	unsigned long long addr = 0;
	char *endptr;

	if (!strstr(args, "acpi_rsdp="))
		return false;

	tmp_cmdline = malloc(len+1);
	if (!tmp_cmdline)
		error("Failed to allocate space for tmp_cmdline");

	memcpy(tmp_cmdline, args, len);
	tmp_cmdline[len] = 0;
	args = tmp_cmdline;

	args = skip_spaces(args);

	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val && strcmp(param, "--") == 0) {
			warn("Only '--' specified in cmdline");
			free(tmp_cmdline);
			return false;
		}

		if (!strcmp(param, "acpi_rsdp")) {
			addr = simple_strtoull(val, &endptr, 0);

			if (addr == 0)
				return false;

			*rsdp_addr = (acpi_physical_address)addr;
			return true;
		}
	}
	return false;
}
#else
static bool get_acpi_rsdp(acpi_physical_address *rsdp_addr)
{
	return false;
}
#endif

/*
 * Used to dig rsdp table from EFI table or BIOS.
 * If rsdp table found in EFI table, use it. Or search BIOS.
 * Based on acpi_os_get_root_pointer().
 */
static acpi_physical_address get_rsdp_addr(void)
{
	acpi_physical_address pa = 0;
	bool status = false;

	status = get_acpi_rsdp(&pa);

	if (!status || pa == 0)
		status = efi_get_rsdp_addr(&pa);

	if (!status || pa == 0)
		bios_get_rsdp_addr(&pa);

	return pa;
}

struct acpi_table_header *get_acpi_srat_table(void)
{
	char *args = (char *)get_cmd_line_ptr();
	acpi_physical_address acpi_table;
	acpi_physical_address root_table;
	struct acpi_table_header *header;
	struct acpi_table_rsdp *rsdp;
	char *signature;
	u8 *entry;
	u32 count;
	u32 size;
	int i, j;
	u32 len;

	rsdp = (struct acpi_table_rsdp *)get_rsdp_addr();
	if (!rsdp)
		return NULL;

	/* Get rsdt or xsdt from rsdp. */
	if (!strstr(args, "acpi=rsdt") &&
	    rsdp->xsdt_physical_address && rsdp->revision > 1) {
		root_table = rsdp->xsdt_physical_address;
		size = ACPI_XSDT_ENTRY_SIZE;
	} else {
		root_table = rsdp->rsdt_physical_address;
		size = ACPI_RSDT_ENTRY_SIZE;
	}

	/* Get ACPI root table from rsdt or xsdt.*/
	header = (struct acpi_table_header *)root_table;
	len = header->length;
	count = (u32)((len - sizeof(struct acpi_table_header)) / size);
	entry = ACPI_ADD_PTR(u8, header, sizeof(struct acpi_table_header));

	for (i = 0; i < count; i++) {
		u64 address64;

		if (size == ACPI_RSDT_ENTRY_SIZE)
			acpi_table = ((acpi_physical_address)
				      (*ACPI_CAST_PTR(u32, entry)));
		else {
			*(u64 *)(void *)&address64 = *(u64 *)(void *)entry;
			acpi_table = (acpi_physical_address) address64;
		}

		if (acpi_table) {
			header = (struct acpi_table_header *)acpi_table;
			signature = header->signature;

			if (!strncmp(signature, "SRAT", 4))
				return header;
		}
		entry += size;
	}
	return NULL;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * According to ACPI table, filter the immvoable memory regions
 * and store them in immovable_mem[].
 */
void get_immovable_mem(void)
{
	char *args = (char *)get_cmd_line_ptr();
	struct acpi_table_header *table_header;
	struct acpi_subtable_header *table;
	struct acpi_srat_mem_affinity *ma;
	unsigned long table_end;
	int i = 0;

	if (!strstr(args, "movable_node") || strstr(args, "acpi=off"))
		return;

	table_header = get_acpi_srat_table();
	if (!table_header)
		return;

	table_end = (unsigned long)table_header + table_header->length;

	table = (struct acpi_subtable_header *)
		((unsigned long)table_header + sizeof(struct acpi_table_srat));

	while (((unsigned long)table) + table->length < table_end) {
		if (table->type == 1) {
			ma = (struct acpi_srat_mem_affinity *)table;
			if (!(ma->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE)) {
				immovable_mem[i].start = ma->base_address;
				immovable_mem[i].size = ma->length;
				i++;
			}

			if (i >= MAX_NUMNODES*2)
				break;
		}
		table = (struct acpi_subtable_header *)
			((unsigned long)table + table->length);
	}
	num_immovable_mem = i;
}
#endif
