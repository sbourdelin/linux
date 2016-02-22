/*
 * Copyright (c) 2012, Intel Corporation
 * Copyright (c) 2015, 2016 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) "ACPI: DBG2: " fmt

#include <linux/acpi_dbg2.h>
#include <linux/acpi.h>
#include <linux/kernel.h>

static const char * __init type2string(u16 type)
{
	switch (type) {
	case ACPI_DBG2_SERIAL_PORT:
		return "SERIAL";
	case ACPI_DBG2_1394_PORT:
		return "1394";
	case ACPI_DBG2_USB_PORT:
		return "USB";
	case ACPI_DBG2_NET_PORT:
		return "NET";
	default:
		return "?";
	}
}

static const char * __init subtype2string(u16 subtype)
{
	switch (subtype) {
	case ACPI_DBG2_16550_COMPATIBLE:
		return "16550_COMPATIBLE";
	case ACPI_DBG2_16550_SUBSET:
		return "16550_SUBSET";
	case ACPI_DBG2_ARM_PL011:
		return "ARM_PL011";
	case ACPI_DBG2_ARM_SBSA_32BIT:
		return "ARM_SBSA_32BIT";
	case ACPI_DBG2_ARM_SBSA_GENERIC:
		return "ARM_SBSA_GENERIC";
	case ACPI_DBG2_ARM_DCC:
		return "ARM_DCC";
	case ACPI_DBG2_BCM2835:
		return "BCM2835";
	default:
		return "?";
	}
}

int __init acpi_dbg2_setup(struct acpi_table_header *table, const void *data)
{
	struct acpi_table_dbg2 *dbg2 = (struct acpi_table_dbg2 *)table;
	struct acpi_dbg2_data *dbg2_data = (struct acpi_dbg2_data *)data;
	struct acpi_dbg2_device *dbg2_device, *dbg2_end;
	int i;

	dbg2_device = ACPI_ADD_PTR(struct acpi_dbg2_device, dbg2,
				   dbg2->info_offset);
	dbg2_end = ACPI_ADD_PTR(struct acpi_dbg2_device, dbg2, table->length);

	for (i = 0; i < dbg2->info_count; i++) {
		if (dbg2_device + 1 > dbg2_end) {
			pr_err("device pointer overflows, bad table\n");
			return 0;
		}

		if (dbg2_device->port_type == dbg2_data->port_type &&
		    dbg2_device->port_subtype == dbg2_data->port_subtype) {
			pr_info("debug port type: %s subtype: %s\n",
				type2string(dbg2_device->port_type),
				subtype2string(dbg2_device->port_subtype));
			dbg2_data->setup(dbg2_device, dbg2_data->data);
		}

		dbg2_device = ACPI_ADD_PTR(struct acpi_dbg2_device, dbg2_device,
					   dbg2_device->length);
	}

	return 0;
}
