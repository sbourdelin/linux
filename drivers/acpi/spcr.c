/*
 * Copyright (c) 2012, Intel Corporation
 * Copyright (c) 2015, Red Hat, Inc.
 * Copyright (c) 2015, 2016 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) "ACPI: SPCR: " fmt

#include <linux/acpi.h>
#include <linux/console.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>

static char *options;
static struct acpi_generic_address address;
static bool sbsa_32_bit;

static int __init parse_spcr_init(void)
{
	struct acpi_table_spcr *table;
	acpi_size table_size;
	acpi_status status;
	int err = 0;

	status = acpi_get_table_with_size(ACPI_SIG_SPCR, 0,
					  (struct acpi_table_header **)&table,
					  &table_size);

	if (ACPI_FAILURE(status)) {
		pr_err("could not get the table\n");
		return -ENOENT;
	}

	if (table->header.revision < 2) {
		err = -EINVAL;
		pr_err("wrong table version\n");
		goto done;
	}

	switch (table->baud_rate) {
	case 3:
		options = "9600";
		break;
	case 4:
		options = "19200";
		break;
	case 6:
		options = "57600";
		break;
	case 7:
		options = "115200";
		break;
	default:
		options = "";
		break;
	}

	address = table->serial_port;
	sbsa_32_bit = table->interface_type == ACPI_DBG2_ARM_SBSA_32BIT;

done:
	early_acpi_os_unmap_memory((void __iomem *)table, table_size);
	return err;
}

/*
 * This function calls __init parse_spcr_init() so it needs __ref.
 * It is referenced by the arch_inicall() macros so it will be called
 * at initialization and the 'parsed' variable will be set.
 * So it's safe to make it __ref.
 */
static int __ref parse_spcr(void)
{
	static bool parsed;
	static int parse_error;

	if (!parsed) {
		parse_error = parse_spcr_init();
		parsed = true;
	}

	return parse_error;
}

arch_initcall(parse_spcr);

/**
 * acpi_console_check - Check if uart matches the console specified by SPCR.
 *
 * @uport:	uart port to check
 *
 * This function checks if the ACPI SPCR table specifies @uport to be a console
 * and if so calls add_preferred_console()
 *
 * Return: a non-error value if the console matches.
 */
bool acpi_console_check(struct uart_port *uport)
{
	if (acpi_disabled || console_set_on_cmdline || parse_spcr() < 0)
		return false;

	if ((address.space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY &&
	     address.address == (u64)uport->mapbase) ||
	    (address.space_id == ACPI_ADR_SPACE_SYSTEM_IO &&
	     address.address == (u64)uport->iobase)) {
		pr_info("adding preferred console [%s%d]\n", uport->cons->name,
			uport->line);
		add_preferred_console(uport->cons->name, uport->line, options);
		return true;
	}

	return false;
}

/**
 * acpi_console_sbsa_32bit - Tell if SPCR specifies 32-bit SBSA.
 *
 * Some implementations of ARM SBSA serial port hardware require that access
 * to the registers should be 32-bit.  Unfortunately, the only way for
 * the driver to tell if it's the case is to use the data from ACPI SPCR/DBG2
 * tables.  In this case the value of the 'Interface Type' field of the SPCR
 * table is ACPI_DBG2_ARM_SBSA_32BIT.
 *
 * Return: true if access should be 32-bit wide.
 */
bool acpi_console_sbsa_32bit(void)
{
	if (acpi_disabled || parse_spcr() < 0)
		return false;

	return sbsa_32_bit;
}
EXPORT_SYMBOL(acpi_console_sbsa_32bit);
