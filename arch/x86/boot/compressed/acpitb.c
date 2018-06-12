#include "acpitb.h"

extern unsigned long get_cmd_line_ptr(void);

/* Search rsdp table from efi table. */
static bool efi_get_rsdp_addr(acpi_physical_address *rsdp_addr)
{
	efi_system_table_t *systab;
	bool find_rsdp = false;
	bool acpi_20 = false;
	bool efi_64 = false;
	void *config_tables;
	struct efi_info *e;
	char *sig;
	int size;
	int i;

#ifndef CONFIG_EFI
	return false;
#endif

	e = &boot_params->efi_info;
	sig = (char *)&e->efi_loader_signature;

	if (!strncmp(sig, EFI64_LOADER_SIGNATURE, 4))
		efi_64 = true;
	else if (!strncmp(sig, EFI32_LOADER_SIGNATURE, 4))
		efi_64 = false;
	else {
		debug_putstr("Wrong efi loader signature.\n");
		return false;
	}

	/* Get systab from boot params. */
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

	/* Get efi tables from systab. */
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

		/* Get rsdp from efi tables. */
		if (!(efi_guidcmp(guid, ACPI_TABLE_GUID)) && !acpi_20) {
			*rsdp_addr = (acpi_physical_address)table;
			acpi_20 = false;
			find_rsdp = true;
		} else if (!(efi_guidcmp(guid, ACPI_20_TABLE_GUID))) {
			*rsdp_addr = (acpi_physical_address)table;
			acpi_20 = true;
			return true;
		}
	}
	return find_rsdp;
}

static u8 checksum(u8 *buffer, u32 length)
{
	u8 sum = 0;
	u8 *end = buffer + length;

	while (buffer < end)
		sum = (u8)(sum + *(buffer++));

	return sum;
}

static u8 *scan_memory_for_rsdp(u8 *start_address, u32 length)
{
	struct acpi_table_rsdp *rsdp;
	u8 *end_address;
	u8 *mem_rover;

	end_address = start_address + length;

	for (mem_rover = start_address; mem_rover < end_address;
	     mem_rover += ACPI_RSDP_SCAN_STEP) {
		rsdp = (struct acpi_table_rsdp *)mem_rover;
		if (!ACPI_VALIDATE_RSDP_SIG(rsdp->signature))
			continue;
		if (checksum((u8 *) rsdp,
		    ACPI_RSDP_CHECKSUM_LENGTH) != 0)
			continue;
		if ((rsdp->revision >= 2) && (checksum((u8 *)
		    rsdp, ACPI_RSDP_XCHECKSUM_LENGTH) != 0))
			continue;
		return mem_rover;
	}
	return NULL;
}

static void bios_get_rsdp_addr(acpi_physical_address *rsdp_addr)
{
	struct acpi_table_rsdp *rsdp;
	u32 physical_address;
	u8 *table_ptr;
	u8 *mem_rover;

	table_ptr = (u8 *)ACPI_EBDA_PTR_LOCATION;
	ACPI_MOVE_16_TO_32(&physical_address, table_ptr);
	physical_address <<= 4;
	table_ptr = (u8 *)(acpi_physical_address)physical_address;

	if (physical_address > 0x400) {
		mem_rover = scan_memory_for_rsdp(table_ptr,
						 ACPI_EBDA_WINDOW_SIZE);

		if (mem_rover) {
			physical_address += (u32)ACPI_PTR_DIFF(mem_rover,
							       table_ptr);
			*rsdp_addr = (acpi_physical_address)physical_address;
			return;
		}
	}

	table_ptr = (u8 *)ACPI_HI_RSDP_WINDOW_BASE;
	mem_rover = scan_memory_for_rsdp(table_ptr, ACPI_HI_RSDP_WINDOW_SIZE);

	if (mem_rover) {
		physical_address = (u32)(ACPI_HI_RSDP_WINDOW_BASE +
					 ACPI_PTR_DIFF(mem_rover, table_ptr));
		*rsdp_addr = (acpi_physical_address)physical_address;
		return;
	}
}

/*
 * Used to dig rsdp table from efi table or bios.
 * If rsdp table found in efi table, use it. Or search bios.
 */
static acpi_physical_address get_rsdp_addr(void)
{
	acpi_physical_address pa = 0;
	bool status = false;

	status = efi_get_rsdp_addr(&pa);

	if (!status || pa == 0)
		bios_get_rsdp_addr(&pa);

	return pa;
}

struct acpi_table_header *get_acpi_srat_table(void)
{
	struct acpi_table_desc table_descs[ACPI_MAX_TABLES];
	char *args = (char *)get_cmd_line_ptr();
	acpi_physical_address acpi_table;
	acpi_physical_address root_table;
	struct acpi_table_rsdp *rsdp;
	struct acpi_table_header *th;
	bool use_rsdt = false;
	u32 table_entry_size;
	u8 *table_entry;
	u32 table_count;
	int i, j;
	u32 len;

	rsdp = (struct acpi_table_rsdp *)get_rsdp_addr();
	if (!rsdp)
		return NULL;

	/* Get rsdt or xsdt from rsdp. */
	if (strstr(args, "acpi=rsdt"))
		use_rsdt = true;

	if (!(use_rsdt) &&
	    (rsdp->xsdt_physical_address) && (rsdp->revision > 1)) {
		root_table = rsdp->xsdt_physical_address;
		table_entry_size = ACPI_XSDT_ENTRY_SIZE;
	} else {
		root_table = rsdp->rsdt_physical_address;
		table_entry_size = ACPI_RSDT_ENTRY_SIZE;
	}

	/* Get acpi root table from rsdt or xsdt. */
	th = (struct acpi_table_header *)root_table;
	len = th->length;
	table_count = (u32)((len - sizeof(struct acpi_table_header)) /
				table_entry_size);
	table_entry = ACPI_ADD_PTR(u8, th, sizeof(struct acpi_table_header));

	for (i = 0; i < table_count; i++) {
		u64 address64;

		memset(&table_descs[i], 0, sizeof(struct acpi_table_desc));
		if (table_entry_size == ACPI_RSDT_ENTRY_SIZE)
			acpi_table = ((acpi_physical_address)
					(*ACPI_CAST_PTR(u32, table_entry)));
		else {
			ACPI_MOVE_64_TO_64(&address64, table_entry);
			acpi_table = (acpi_physical_address) address64;
		}

		if (acpi_table) {
			table_descs[i].address = acpi_table;
			table_descs[i].length = sizeof(
						struct acpi_table_header);
			table_descs[i].pointer = (struct acpi_table_header *)
						  acpi_table;
			for (j = 0; j < 4; j++)
				table_descs[i].signature.ascii[j] =
					((struct acpi_table_header *)
					 acpi_table)->signature[j];
		}

		if (!strncmp(table_descs[i].signature.ascii, "SRAT", 4))
			return table_descs[i].pointer;

		table_entry += table_entry_size;
	}
	return NULL;
}
