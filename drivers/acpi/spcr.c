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

static int acpi_table_parse_spcr(int (*handler)(struct acpi_table_spcr *table,
						void *data), void *data)
{
	struct acpi_table_spcr *table = NULL;
	acpi_size table_size;
	acpi_status status;
	int err;

	status = acpi_get_table_with_size(ACPI_SIG_SPCR, 0,
					  (struct acpi_table_header **)&table,
					  &table_size);

	if (ACPI_FAILURE(status))
		return -ENODEV;

	err = handler(table, data);

	early_acpi_os_unmap_memory(table, table_size);

	return err;
}

static int spcr_table_handler_check(struct acpi_table_spcr *table, void *data)
{
	struct uart_port *uport = data;
	char *options;

	if (table->header.revision < 2)
		return -EOPNOTSUPP;

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

	if ((table->serial_port.space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY &&
	     table->serial_port.address == (u64)uport->mapbase) ||
	    (table->serial_port.space_id == ACPI_ADR_SPACE_SYSTEM_IO &&
	     table->serial_port.address == (u64)uport->iobase)) {
		pr_info("adding preferred console [%s%d]\n", uport->cons->name,
			uport->line);
		add_preferred_console(uport->cons->name, uport->line, options);
		return 1;

	}

	return 0;
}

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
	if (acpi_disabled || console_set_on_cmdline)
		return false;

	return acpi_table_parse_spcr(spcr_table_handler_check, uport) > 0;
}
