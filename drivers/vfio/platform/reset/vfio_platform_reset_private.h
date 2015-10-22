/*
 * Interface used by VFIO platform reset modules to register/unregister
 * their reset function
 *
 * Copyright (c) 2015 Linaro Ltd.
 *              www.linaro.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef VFIO_PLATFORM_RESET_PRIVATE_H
#define VFIO_PLATFORM_RESET_PRIVATE_H

#include <linux/module.h>
#include "vfio_platform_private.h"

static int reset_module_register(struct module *module,
				    const char *compat,
				    vfio_platform_reset_fn_t reset)
{
	int (*register_reset)(struct module *, const char*,
				vfio_platform_reset_fn_t);
	int ret;

	register_reset = symbol_get(vfio_platform_register_reset);
	if (!register_reset)
		return -EINVAL;
	ret = register_reset(module, compat, reset);
	symbol_put(vfio_platform_register_reset);
	return ret;
}

static void reset_module_unregister(const char *compat)
{
	int (*unregister_reset)(const char *);

	unregister_reset = symbol_get(vfio_platform_unregister_reset);
	if (!unregister_reset)
		return;

	unregister_reset(compat);

	symbol_put(vfio_platform_unregister_reset);
}

#define module_vfio_reset_handler(compat, reset)			\
MODULE_ALIAS("vfio-reset:" compat);					\
static int __init reset ## _module_init(void)				\
{									\
	return reset_module_register(THIS_MODULE, compat, &reset);	\
};									\
static void __exit reset ## _module_exit(void)				\
{                                                                       \
	reset_module_unregister(compat);				\
};									\
module_init(reset ## _module_init);					\
module_exit(reset ## _module_exit)

#endif /* VFIO_PLATFORM_RESET_PRIVATE_H */
