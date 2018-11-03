// SPDX-License-Identifier: GPL-2.0

/*
 * do_mounts_dm.c
 * Copyright (C) 2017 The Chromium OS Authors <chromium-os-dev@chromium.org>
 *
 * This file is released under the GPLv2.
 */
#include <linux/device-mapper.h>

#include "do_mounts.h"

#define DM_MSG_PREFIX "init"

static struct {
	char *str;
	int early_setup;
} dm_setup_args __initdata;

/*
 * Parse the command-line parameters given to our kernel, but do not
 * actually try to invoke the DM device now; that is handled by
 * dm_boot_setup_drives after the low-level disk drivers have initialised.
 */
static int __init dm_setup(char *str)
{
	if (!str) {
		DMERR("Invalid arguments supplied to dm=.");
		return 0;
	}
	DMDEBUG("Want to parse \"%s\"", str);
	dm_setup_args.str = str;
	dm_setup_args.early_setup = 1;

	return 1;
}

__setup("dm=", dm_setup);

void __init dm_run_setup(void)
{
	if (!dm_setup_args.early_setup)
		return;
	DMINFO("attempting early device configuration.");
	dm_boot_setup_drives(dm_setup_args.str);
}
