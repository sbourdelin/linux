/*
 * scom.h - hwmon OCC driver
 *
 * This file contains data structures for scom operations to the OCC
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __SCOM_H__
#define __SCOM_H__

/*
 * occ_bus_ops - represent the low-level transfer methods to communicate with
 * the OCC.
 *
 * getscom - OCC scom read 8 bytes
 * @bus: handle to slave device
 * @address: address
 * @data: where to store data read from slave; buffer size must be at least
 * eight bytes.
 *
 * Returns 0 on success or a negative errno on error
 *
 * putscom - OCC scom write
 * @bus: handle to slave device
 * @address: address
 * @data0: first data word to write
 * @data1: second data word to write
 *
 * Returns 0 on success or a negative errno on error
 */
struct occ_bus_ops {
	int (*getscom)(void *bus, u32 address, u64 *data);
	int (*putscom)(void *bus, u32 address, u32 data0, u32 data1);
};

#endif /* __SCOM_H__ */
