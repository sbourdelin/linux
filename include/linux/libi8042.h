#ifndef _LINUX_LIBI8042_H
#define _LINUX_LIBI8042_H

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/types.h>

/*
 * Number of AUX ports on controllers supporting active multiplexing
 * specification
 */

#define I8042_NUM_MUX_PORTS	4
#define I8042_NUM_PORTS		(I8042_NUM_MUX_PORTS + 2)

struct serio;

struct i8042_port {
	struct serio *serio;
	int irq;
	bool exists;
	bool driver_bound;
	signed char mux;
};

#if defined(CONFIG_SERIO_I8042) || defined(CONFIG_SERIO_I8042_MODULE)

extern struct i8042_port i8042_ports[I8042_NUM_PORTS];

void i8042_lock_chip(void);
void i8042_unlock_chip(void);
bool i8042_check_port_owner(const struct serio *);

#else

static inline void i8042_lock_chip(void)
{
}

static inline void i8042_unlock_chip(void)
{
}

static inline bool i8042_check_port_owner(const struct serio *serio)
{
	return false;
}

#endif

#endif
