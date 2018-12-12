// SPDX-License-Identifier: GPL-2.0
#define BOOT_CTYPE_H
#include "misc.h"
#include "error.h"

#include <linux/efi.h>
#include <linux/numa.h>
#include <linux/acpi.h>
#include <asm/efi.h>

#define STATIC
#include <linux/decompress/mm.h>

#include "../string.h"

static acpi_physical_address get_acpi_rsdp(void)
{
#ifdef CONFIG_KEXEC
	unsigned long long res;
	int len = 0;
	char val[MAX_ADDRESS_LENGTH+1];

	len = cmdline_find_option("acpi_rsdp", val, MAX_ADDRESS_LENGTH+1);
	if (len > 0) {
		val[len] = 0;
		return (acpi_physical_address)kstrtoull(val, 16, &res);
	}
	return 0;
#endif
}

/* Search EFI table for RSDP. */
static acpi_physical_address efi_get_rsdp_addr(void)
{
#ifdef CONFIG_EFI
	acpi_physical_address rsdp_addr = 0;
	efi_system_table_t *systab;
	struct efi_info *e;
	bool efi_64;
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
		return 0;
	}

	/* Get systab from boot params. Based on efi_init(). */
#ifdef CONFIG_X86_64
	systab = (efi_system_table_t *)(e->efi_systab | ((__u64)e->efi_systab_hi<<32));
#else
	if (e->efi_systab_hi || e->efi_memmap_hi) {
		debug_putstr("Error getting RSDP address: EFI system table located above 4GB.\n");
		return 0;
	}
	systab = (efi_system_table_t *)e->efi_systab;
#endif

	if (!systab)
		return 0;

	/*
	 * Get EFI tables from systab. Based on efi_config_init() and
	 * efi_config_parse_tables().
	 */
	size = efi_64 ? sizeof(efi_config_table_64_t) :
			sizeof(efi_config_table_32_t);

	for (i = 0; i < systab->nr_tables; i++) {
		void *config_tables;
		unsigned long table;
		efi_guid_t guid;

		config_tables = (void *)(systab->tables + size * i);
		if (efi_64) {
			efi_config_table_64_t *tmp_table;

			tmp_table = (efi_config_table_64_t *)config_tables;
			guid = tmp_table->guid;
			table = tmp_table->table;

			if (!IS_ENABLED(CONFIG_X86_64) && table >> 32) {
				debug_putstr("Error getting RSDP address: EFI system table located above 4GB.\n");
				return 0;
			}
		} else {
			efi_config_table_32_t *tmp_table;

			tmp_table = (efi_config_table_32_t *)config_tables;
			guid = tmp_table->guid;
			table = tmp_table->table;
		}

		if (!(efi_guidcmp(guid, ACPI_TABLE_GUID)))
			rsdp_addr = (acpi_physical_address)table;
		else if (!(efi_guidcmp(guid, ACPI_20_TABLE_GUID)))
			return (acpi_physical_address)table;
	}
	return rsdp_addr;
#endif
}

static u8 compute_checksum(u8 *buffer, u32 length)
{
	u8 *end = buffer + length;
	u8 sum = 0;

	while (buffer < end)
		sum += *(buffer++);

	return sum;
}

/* Search a block of memory for the RSDP signature. */
static u8 *scan_mem_for_rsdp(u8 *start, u32 length)
{
	struct acpi_table_rsdp *rsdp;
	u8 *address;
	u8 *end;

	end = start + length;

	/* Search from given start address for the requested length */
	for (address = start; address < end; address += ACPI_RSDP_SCAN_STEP) {
		/*
		 * Both RSDP signature and checksum must be correct.
		 * Note: Sometimes there exists more than one RSDP in memory;
		 * the valid RSDP has a valid checksum, all others have an
		 * invalid checksum.
		 */
		rsdp = (struct acpi_table_rsdp *)address;

		/* BAD Signature */
		if (!ACPI_VALIDATE_RSDP_SIG(rsdp->signature))
			continue;

		/* Check the standard checksum */
		if (compute_checksum((u8 *) rsdp, ACPI_RSDP_CHECKSUM_LENGTH))
			continue;

		/* Check extended checksum if table version >= 2 */
		if ((rsdp->revision >= 2) &&
		    (compute_checksum((u8 *) rsdp, ACPI_RSDP_XCHECKSUM_LENGTH)))
			continue;

		/* Signature and checksum valid, we have found a real RSDP */
		return address;
	}
	return NULL;
}

/* Search RSDP address, based on acpi_find_root_pointer(). */
static acpi_physical_address bios_get_rsdp_addr(void)
{
	u8 *table_ptr;
	u32 address;
	u8 *rsdp;

	/* Get the location of the Extended BIOS Data Area (EBDA) */
	table_ptr = (u8 *)ACPI_EBDA_PTR_LOCATION;
	*(u32 *)(void *)&address = *(u16 *)(void *)table_ptr;
	address <<= 4;
	table_ptr = (u8 *)(long)address;

	/*
	 * Search EBDA paragraphs (EBDA is required to be a minimum of
	 * 1K length)
	 */
	if (address > 0x400) {
		rsdp = scan_mem_for_rsdp(table_ptr, ACPI_EBDA_WINDOW_SIZE);
		if (rsdp) {
			address += (u32)ACPI_PTR_DIFF(rsdp, table_ptr);
			return (acpi_physical_address)address;
		}
	}

	table_ptr = (u8 *)ACPI_HI_RSDP_WINDOW_BASE;
	rsdp = scan_mem_for_rsdp(table_ptr, ACPI_HI_RSDP_WINDOW_SIZE);

	/* Search upper memory: 16-byte boundaries in E0000h-FFFFFh */
	if (rsdp) {
		address = (u32)(ACPI_HI_RSDP_WINDOW_BASE +
				ACPI_PTR_DIFF(rsdp, table_ptr));
		return (acpi_physical_address)address;
	}
}
