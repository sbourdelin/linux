/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef __ACPI_NFIT_H
#define __ACPI_NFIT_H

#if IS_ENABLED(CONFIG_ACPI_NFIT)
int nfit_get_smbios_id(u32 device_handle, u16 *flags);
#else
static inline int nfit_get_smbios_id(u32 device_handle, u16 *flags)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __ACPI_NFIT_H */
