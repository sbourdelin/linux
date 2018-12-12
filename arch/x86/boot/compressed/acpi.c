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
