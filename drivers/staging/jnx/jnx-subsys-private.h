/*
 * Juniper Generic APIs for providing chassis and card information
 * Private API
 *
 * Copyright (C) 2012, 2013, 2014 Juniper Networks. All rights reserved.
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

#ifndef _JNX_SUBSYS_PRIVATE_H
#define _JNX_SUBSYS_PRIVATE_H

#include <linux/jnx/jnx-subsys.h>

/* mastership related */
int register_mastership_notifier(struct notifier_block *nb);
int unregister_mastership_notifier(struct notifier_block *nb);

/* returns true is running on master */
bool jnx_is_master(void);

/* register and unregister a non local juniper board */
int jnx_register_board(struct device *edev, struct device *ideeprom,
		       struct jnx_card_info *cinfo, int id);
int jnx_unregister_board(struct device *edev);

#endif /* _JNX_SUBSYS_H */
