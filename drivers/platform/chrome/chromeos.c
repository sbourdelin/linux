/*
 *  ChromeOS platform support code. Glue layer between higher level functions
 *  and per-platform firmware interfaces.
 *
 *  Copyright (C) 2017 The Chromium OS Authors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/types.h>
#include <linux/module.h>
#include "chromeos.h"

static struct chromeos_vbc *chromeos_vbc_ptr;

int chromeos_vbc_register(struct chromeos_vbc *chromeos_vbc)
{
	chromeos_vbc_ptr = chromeos_vbc;
	return 0;
}
