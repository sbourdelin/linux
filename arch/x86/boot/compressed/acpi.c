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
