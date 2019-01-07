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
