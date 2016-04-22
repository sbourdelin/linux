/*
 *  i8042 driver shared dependencies
 *
 *  Copyright (c) 1999-2004 Vojtech Pavlik
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/libi8042.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@suse.cz>");
MODULE_DESCRIPTION("i8042 driver shared dependencies");
MODULE_LICENSE("GPL");

/*
 * Writers to AUX and KBD ports as well as users issuing i8042_command
 * directly should acquire i8042_mutex (by means of calling
 * i8042_lock_chip() and i8042_unlock_ship() helpers) to ensure that
 * they do not disturb each other (unfortunately in many i8042
 * implementations write to one of the ports will immediately abort
 * command that is being processed by another port).
 */
static DEFINE_MUTEX(i8042_mutex);

struct i8042_port i8042_ports[I8042_NUM_PORTS];
EXPORT_SYMBOL(i8042_ports);

void i8042_lock_chip(void)
{
	mutex_lock(&i8042_mutex);
}
EXPORT_SYMBOL(i8042_lock_chip);

void i8042_unlock_chip(void)
{
	mutex_unlock(&i8042_mutex);
}
EXPORT_SYMBOL(i8042_unlock_chip);

/*
 * Checks whether port belongs to i8042 controller.
 */
bool i8042_check_port_owner(const struct serio *port)
{
	int i;

	for (i = 0; i < I8042_NUM_PORTS; i++)
		if (i8042_ports[i].serio == port)
			return true;

	return false;
}
EXPORT_SYMBOL(i8042_check_port_owner);
