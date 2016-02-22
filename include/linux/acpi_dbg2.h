#ifndef _ACPI_DBG2_H_
#define _ACPI_DBG2_H_

#ifdef CONFIG_ACPI_DBG2_TABLE

#include <linux/kernel.h>

struct acpi_dbg2_device;
struct acpi_table_header;

struct acpi_dbg2_data {
	u16 port_type;
	u16 port_subtype;
	int (*setup)(struct acpi_dbg2_device *, void *);
	void *data;
};

int acpi_dbg2_setup(struct acpi_table_header *header, const void *data);

/*
 * ACPI_DBG2_DECLARE() - Define handler for ACPI DBG2 port
 * @name:	Identifier to compose name of table data
 * @type:	Type of the port
 * @subtype:	Subtype of the port
 * @setup_fn:	Function to be called to setup the port
 *		(of type int (*)(struct acpi_dbg2_device *, void *);)
 * @data_ptr:	Sideband data provided back to the driver
 */
#define ACPI_DBG2_DECLARE(name, type, subtype, setup_fn, data_ptr)	\
	static const struct acpi_dbg2_data				\
		__acpi_dbg2_data_##name __used = {			\
			.port_type = type,				\
			.port_subtype = subtype,			\
			.setup = setup_fn,				\
			.data = data_ptr,				\
		   };							\
	ACPI_DECLARE_PROBE_ENTRY(dbg2, name, ACPI_SIG_DBG2,		\
				 acpi_dbg2_setup, &__acpi_dbg2_data_##name)

int __init acpi_setup_earlycon(struct acpi_dbg2_device *device, void *d);

/*
 * ACPI_DBG2_EARLYCON_DECLARE() - Define handler for ACPI GDB2 serial port
 * @name:		Identifier to compose name of table data
 * @subtype:		Subtype of the port
 * @console_setup:	Function to be called to setup the port
 *
 * Type of the console_setup() callback is
 * int (*setup)(struct earlycon_device *, const char *)
 * It's the type of callback of of_setup_earlycon().
 */
#define ACPI_DBG2_EARLYCON_DECLARE(name, subtype, console_setup)	\
	ACPI_DBG2_DECLARE(name, ACPI_DBG2_SERIAL_PORT, subtype,		\
			  acpi_setup_earlycon, console_setup)

#else

#define ACPI_DBG2_DECLARE(name, type, subtype, setup_fn, data_ptr)	\
	static const void *__acpi_dbg_data_##name[]			\
		__used __initdata = { (void *)setup_fn,	(void *)data_ptr }

#define ACPI_DBG2_EARLYCON_DECLARE(name, subtype, console_setup)	\
	static const void *__acpi_dbg_data_serial_##name[]		\
		__used __initdata = { (void *)console_setup }

#endif

#endif
