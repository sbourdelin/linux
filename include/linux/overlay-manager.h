/*
 * Copyright (C) 2016 - Antoine Tenart <antoine.tenart@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __OVERLAY_MGR_H__
#define __OVERLAY_MGR_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/sizes.h>

#define OVERLAY_MGR_DIP_MAX_SZ		SZ_128

struct overlay_mgr_format {
	struct list_head list;
	char *name;
	int (*parse)(struct device *dev, void *data, char ***candidates,
		     unsigned *n);
};

int overlay_mgr_register_format(struct overlay_mgr_format *candidate);
int overlay_mgr_parse(struct device *dev, void *data, char ***candidates,
		      unsigned *n);
int overlay_mgr_apply(struct device *dev, char **candidates, unsigned n);

#define dip_convert(field)                                      \
        (                                                       \
                (sizeof(field) == 1) ? field :                  \
                (sizeof(field) == 2) ? be16_to_cpu(field) :     \
                (sizeof(field) == 4) ? be32_to_cpu(field) :     \
                -1                                              \
        )

#endif /* __OVERLAY_MGR_H__ */
