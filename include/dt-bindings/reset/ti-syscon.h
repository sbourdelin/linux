/*
 * TI Syscon Reset definitions
 *
 * Copyright (C) 2015-2016 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __DT_BINDINGS_RESET_TI_SYSCON_H__
#define __DT_BINDINGS_RESET_TI_SYSCON_H__

/* The reset is asserted by setting (vs clearing) the described bit */
#define RESET_SET		(1 << 0)
/* This reset does not have a readable status bit */
#define RESET_TRIGGER		(1 << 1)

#define RESET_ASSERT_CLEAR	0
#define RESET_ASSERT_SET	RESET_SET
#define RESET_TRIGGER_CLEAR	RESET_TRIGGER
#define RESET_TRIGGER_SET	(RESET_TRIGGER | RESET_SET)

#endif
