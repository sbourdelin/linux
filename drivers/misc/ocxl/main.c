/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include "ocxl_internal.h"

static int __init init_ocxl(void)
{
	int rc = 0;

	rc = ocxl_file_init();
	if (rc)
		return rc;

	rc = pci_register_driver(&ocxl_pci_driver);
	if (rc) {
		ocxl_file_exit();
		return rc;
	}
	return 0;
}

static void exit_ocxl(void)
{
	pci_unregister_driver(&ocxl_pci_driver);
	ocxl_file_exit();
}

module_init(init_ocxl);
module_exit(exit_ocxl);

MODULE_DESCRIPTION("Open Coherent Accelerator");
MODULE_LICENSE("GPL");
