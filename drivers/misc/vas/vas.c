/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <asm/vas.h>
#include "vas-internal.h"
#include <asm/opal-api.h>
#include <asm/opal.h>

int vas_initialized;
struct vas_instance *vas_instances;

/*
 * Read the Fault Isolation Registers (FIR) from skiboot into @fir.
 */
static void read_fault_regs(int chip, uint64_t *fir)
{
	int i;
	int64_t rc;

	for (i = 0; i < 8; i++)
		rc = opal_vas_read_fir(chip, i, &fir[i]);
}

/*
 * Print the VAS Fault Isolation Registers (FIR) for the chip @chip.
 * Used when we encounter an error/exception in VAS.
 *
 * TODO: Find the chip id where the exception occurred. Hard coding to
 *	 chip 0 for now.
 */
void vas_print_regs(int chip)
{
	int i;
	uint64_t firs[8];

	/* TODO: Only dump FIRs for first chip for now */
	if (chip == -1)
		chip = 0;

	read_fault_regs(chip, firs);
	for (i = 0; i < 8; i += 4) {
		pr_err("FIR%d: 0x%llx    0x%llx    0x%llx    0x%llx\n", i,
				firs[i], firs[i+1], firs[i+2], firs[i+3]);
	}
}


static void init_vas_chip(struct vas_instance *vinst)
{
	int i;

	for (i = 0; i < VAS_MAX_WINDOWS_PER_CHIP; i++)
		vas_window_reset(vinst, i);
}

/*
 * Although this is read/used multiple times, it is written to only
 * during initialization.
 */
struct vas_instance *find_vas_instance(int node, int chip)
{
	int i = node * VAS_MAX_CHIPS_PER_NODE + chip;

	return &vas_instances[i];
}

static void init_vas_instance(int node, int chip)
{
	struct vas_instance *vinst;

	vinst = find_vas_instance(node, chip);

	ida_init(&vinst->ida);

	vinst->node = node;
	vinst->chip = chip;
	mutex_init(&vinst->mutex);

	init_vas_chip(vinst);
}

int vas_init(void)
{
	int n, c;

	vas_instances = kmalloc_array(VAS_MAX_NODES * VAS_MAX_CHIPS_PER_NODE,
				sizeof(struct vas_instance), GFP_KERNEL);
	if (!vas_instances)
		return -ENOMEM;

	/*
	 * TODO: Get node-id and chip id from device tree?
	 */
	for (n = 0; n < VAS_MAX_NODES; n++) {
		for (c = 0; c < VAS_MAX_CHIPS_PER_NODE; c++)
			init_vas_instance(n, c);
	}

	vas_initialized = 1;

	return 0;
}

void vas_exit(void)
{
	vas_initialized = 0;
	kfree(vas_instances);
}

/*
 * We will have a device driver for user space access to VAS.
 * But for now this is just a wrapper to vas_init()
 */
int __init vas_dev_init(void)
{
	int rc;

	rc = vas_init();
	if (rc)
		return rc;

	vas_initialized = 1;

	pr_err("VAS: initialized\n");

	return 0;
}

void __init vas_dev_exit(void)
{
	pr_err("VAS: exiting\n");
	vas_exit();
}

module_init(vas_dev_init);
module_exit(vas_dev_exit);
MODULE_DESCRIPTION("IBM Virtual Accelerator Switchboard");
MODULE_AUTHOR("Sukadev Bhattiprolu <sukadev@linux.vnet.ibm.com>");
MODULE_LICENSE("GPL");
