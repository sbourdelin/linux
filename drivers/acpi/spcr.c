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

#include <linux/acpi.h>
#include <linux/console.h>
#include <linux/kernel.h>

struct spcr_table_handler_match_data {
	struct console *console;
	char **options;
};

static int spcr_table_handler_match(struct acpi_table_header *t, void *d)
{
	struct acpi_table_spcr *table = (struct acpi_table_spcr *)t;
	struct spcr_table_handler_match_data *data = d;
	int err;

	if (table->header.revision < 2)
		return -EOPNOTSUPP;

	err = data->console->acpi_match(data->console, table);
	if (err < 0)
		return err;

	if (data->options) {
		switch (table->baud_rate) {
		case 3:
			*data->options = "9600";
			break;
		case 4:
			*data->options = "19200";
			break;
		case 6:
			*data->options = "57600";
			break;
		case 7:
			*data->options = "115200";
			break;
		default:
			*data->options = "";
			break;
		}
	}

	return err;
}

/**
 * acpi_console_match - Check if console matches one specified by SPCR.
 *
 * @console:	console to match
 * @options:	if the console matches, this will return options for the console
 *		as in kernel command line
 *
 * Return: a non-error value if the console matches.
 */
int acpi_console_match(struct console *console, char **options)
{
	struct spcr_table_handler_match_data d = {
		.console = console,
		.options = options,
	};

	if (acpi_disabled || !console->acpi_match || console_set_on_cmdline)
		return -ENODEV;

	return acpi_table_parse2(ACPI_SIG_SPCR, spcr_table_handler_match, &d);
}

static int spcr_table_handler_32_bit(struct acpi_table_header *t,
				     void *data)
{
	struct acpi_table_spcr *table = (struct acpi_table_spcr *)t;

	return table->interface_type == ACPI_DBG2_ARM_SBSA_32BIT;
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
	if (acpi_disabled)
		return false;

	return acpi_table_parse2(ACPI_SIG_SPCR,
				 spcr_table_handler_32_bit, NULL) > 0;
}
EXPORT_SYMBOL(acpi_console_sbsa_32bit);
