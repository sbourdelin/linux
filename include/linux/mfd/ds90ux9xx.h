/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TI DS90Ux9xx MFD driver
 *
 * Copyright (c) 2017-2018 Mentor Graphics Inc.
 */

#ifndef __LINUX_MFD_DS90UX9XX_H
#define __LINUX_MFD_DS90UX9XX_H

#include <linux/types.h>

enum ds90ux9xx_device_id {
	/* Supported serializers */
	TI_DS90UB925,
	TI_DS90UH925,
	TI_DS90UB927,
	TI_DS90UH927,

	/* Supported deserializers */
	TI_DS90UB926,
	TI_DS90UH926,
	TI_DS90UB928,
	TI_DS90UH928,
	TI_DS90UB940,
	TI_DS90UH940,
};

struct device;

bool ds90ux9xx_is_serializer(struct device *dev);
enum ds90ux9xx_device_id ds90ux9xx_get_ic_type(struct device *dev);
unsigned int ds90ux9xx_num_fpd_links(struct device *dev);

int ds90ux9xx_get_link_status(struct device *dev, unsigned int *link,
			      bool *locked);

int ds90ux9xx_update_bits_indirect(struct device *dev, u8 reg, u8 mask, u8 val);
int ds90ux9xx_write_indirect(struct device *dev, unsigned char reg, u8 val);
int ds90ux9xx_read_indirect(struct device *dev, u8 reg, u8 *val);

#endif /*__LINUX_MFD_DS90UX9XX_H */
