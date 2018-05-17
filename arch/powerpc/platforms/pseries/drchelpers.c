/*
 * Copyright (C) 2018 Michael Bringmann <mbringm@us.ibm.com>, IBM
 *
 * pSeries specific routines for device-tree properties.
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
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <asm/prom.h>
#include "pseries.h"

#define	MAX_DRC_NAME_LEN 64

int drc_info_parser(struct device_node *dn,
		int (*usercb)(struct of_drc_info *drc,
				void *data,
				void *optional_data,
				int *ret_code),
		char *opt_drc_type,
		void *data)
{
	struct property *info;
	unsigned int entries;
	struct of_drc_info drc;
	const __be32 *value;
	int j, done = 0, ret_code = -EINVAL;

	info = of_find_property(dn, "ibm,drc-info", NULL);
	if (info == NULL)
		return -EINVAL;

	value = of_prop_next_u32(info, NULL, &entries);
	if (!value)
		return -EINVAL;
	value++;

	for (j = 0, done = 0; (j < entries) && (!done); j++) {
		of_read_drc_info_cell(&info, &value, &drc);

		if (opt_drc_type && strcmp(opt_drc_type, drc.drc_type))
			continue;

		done = usercb(&drc, data, NULL, &ret_code);
	}

	return ret_code;
}
EXPORT_SYMBOL(drc_info_parser);
