/*
 * Copyright (C) IBM Corporation 2017
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERGCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef LINUX_OCC_H
#define LINUX_OCC_H

struct device;
struct occ_client;

extern struct occ_client *occ_drv_open(struct device *dev,
				       unsigned long flags);
extern int occ_drv_read(struct occ_client *client, char *buf, size_t len);
extern int occ_drv_write(struct occ_client *client, const char *buf,
			 size_t len);
extern void occ_drv_release(struct occ_client *client);

#endif /* LINUX_OCC_H */
