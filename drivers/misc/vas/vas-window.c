/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/mutex.h>
#include <asm/vas.h>
#include "vas-internal.h"

/* stub for now */
int vas_window_reset(struct vas_instance *vinst, int winid)
{
	return 0;
}
