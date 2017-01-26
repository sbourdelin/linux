/*
 * occ_scom_i2c.h - hwmon OCC driver
 *
 * This file contains function protoypes for peforming SCOM operations over I2C
 * bus to access the OCC.
 *
 * Copyright 2016 IBM Corp.
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

#ifndef __OCC_SCOM_I2C_H__
#define __OCC_SCOM_I2C_H__

int occ_i2c_getscom(void *bus, u32 address, u64 *data);
int occ_i2c_putscom(void *bus, u32 address, u32 data0, u32 data1);

#endif /* __OCC_SCOM_I2C_H__ */
