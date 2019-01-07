// SPDX-License-Identifier: GPL-2.0
#define BOOT_CTYPE_H
#include "misc.h"
#include "error.h"
#include "../string.h"

#include <linux/acpi.h>
#include <linux/efi.h>
#include <asm/efi.h>

/*
 * Max length of 64-bit hex address string is 19, prefix "0x" + 16 hex
 * digits, and '\0' for termination.
 */
#define MAX_HEX_ADDRESS_STRING_LEN 19

static acpi_physical_address get_acpi_rsdp(void)
{
#ifdef CONFIG_KEXEC
	char val[MAX_HEX_ADDRESS_STRING_LEN];
	unsigned long long res;
	int len = 0;

	len = cmdline_find_option("acpi_rsdp", val, MAX_HEX_ADDRESS_STRING_LEN);
	if (len > 0) {
		val[len] = '\0';
		if (!kstrtoull(val, 16, &res))
			return res;
	}
#endif
	return 0;
}

/* Search EFI table for RSDP. */
static acpi_physical_address efi_get_rsdp_addr(void)
{
	acpi_physical_address rsdp_addr = 0;
#ifdef CONFIG_EFI
	efi_system_table_t *systab;
	struct efi_info *ei;
	bool efi_64;
	char *sig;
	int size;
	int i;

	ei = &boot_params->efi_info;
	sig = (char *)&ei->efi_loader_signature;

	if (!strncmp(sig, EFI64_LOADER_SIGNATURE, 4)) {
		efi_64 = true;
	} else if (!strncmp(sig, EFI32_LOADER_SIGNATURE, 4)) {
		efi_64 = false;
	} else {
		debug_putstr("Wrong EFI loader signature.\n");
		return 0;
	}

	/* Get systab from boot params. Based on efi_init(). */
#ifdef CONFIG_X86_64
	systab = (efi_system_table_t *)(ei->efi_systab | ((__u64)ei->efi_systab_hi<<32));
#else
	if (ei->efi_systab_hi || ei->efi_memmap_hi) {
		debug_putstr("Error getting RSDP address: EFI system table located above 4GB.\n");
		return 0;
	}
	systab = (efi_system_table_t *)ei->efi_systab;
#endif

	if (!systab)
		error("EFI system table is not found.");

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
			u64 table64;

			tmp_table = config_tables;
			guid = tmp_table->guid;
			table64 = tmp_table->table;
			table = table64;

			if (!IS_ENABLED(CONFIG_X86_64) && table64 >> 32) {
				debug_putstr("Error getting RSDP address: EFI config table located above 4GB.\n");
				return 0;
			}
		} else {
			efi_config_table_32_t *tmp_table;

			tmp_table = config_tables;
			guid = tmp_table->guid;
			table = tmp_table->table;
		}

		if (!(efi_guidcmp(guid, ACPI_TABLE_GUID)))
			rsdp_addr = table;
		else if (!(efi_guidcmp(guid, ACPI_20_TABLE_GUID)))
			return table;
	}
#endif
	return rsdp_addr;
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
		if (compute_checksum((u8 *)rsdp, ACPI_RSDP_CHECKSUM_LENGTH))
			continue;

		/* Check extended checksum if table version >= 2 */
		if ((rsdp->revision >= 2) &&
		    (compute_checksum((u8 *)rsdp, ACPI_RSDP_XCHECKSUM_LENGTH)))
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
	address = *(u16 *)table_ptr;
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
			return address;
		}
	}

	/* Search upper memory: 16-byte boundaries in E0000h-FFFFFh */
	table_ptr = (u8 *)ACPI_HI_RSDP_WINDOW_BASE;
	rsdp = scan_mem_for_rsdp(table_ptr, ACPI_HI_RSDP_WINDOW_SIZE);

	if (rsdp) {
		address = (u32)(ACPI_HI_RSDP_WINDOW_BASE +
				ACPI_PTR_DIFF(rsdp, table_ptr));
		return address;
	}
	return 0;
}
