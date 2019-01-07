// SPDX-License-Identifier: GPL-2.0
#define BOOT_CTYPE_H
#include "misc.h"
#include "error.h"
#include "../string.h"

#include <linux/acpi.h>

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
