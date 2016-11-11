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
#include <linux/kthread.h>
#include <linux/io.h>
#include <asm/vas.h>
#include "vas-internal.h"
#include <asm/opal-api.h>
#include <asm/opal.h>

#define VAS_FAULT_WIN_FIFO_SIZE		(64 << 10)
#define VAS_FAULT_WIN_WCREDS		64

int vas_initialized;
struct vas_instance *vas_instances;

struct fault_win_thread_arg {
	int notified;
	wait_queue_head_t wq;
	void *rx_fifo;
	int rx_fifo_size;
} fwta;

struct task_struct *fwt_thr;		/* Fault window thread */
struct vas_window *fault_win;

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

void vas_wakeup_fault_win_thread(void)
{
	fwta.notified = 1;
	wake_up(&fwta.wq);
}

/*
 * Process a CRB that we receive on the fault window.
 *
 * TODO: Since we only support in-kernel compression requests for now,
 *	 we should not get a fault. If we do, dump the CRB and the FIR
 *	 and return - VAS may enter a checkstop :-(
 */
static void process_fault_crb(struct fault_win_thread_arg *fwt)
{
	u64 buf[16];

	/* TODO: Dump FIRs for all chips for now. We should detect the
	 *	 current chip id and dump only for that chip?
	 */
	vas_print_regs(-1);

	memcpy(buf, fwt->rx_fifo, sizeof(buf));
	memset(fwt->rx_fifo, 0, sizeof(buf));
	pr_debug("VAS: FaultWin Rx-fifo: 0x%llx 0x%llx 0x%llx 0x%llx\n",
				buf[0], buf[1], buf[2], buf[3]);
}

static int fault_win_thread(void *arg)
{
	struct fault_win_thread_arg *fwta = arg;

	do {
		if (signal_pending(current))
			flush_signals(current);

		fwta->notified = 0;
		wait_event_interruptible(fwta->wq, fwta->notified ||
				kthread_should_stop());

		process_fault_crb(fwta);

	} while (!kthread_should_stop());

	return 0;
}

static int create_fault_win(void)
{
	char *name = "VAS-FaultWin-Thread";
	struct vas_rx_win_attr attr;

	init_waitqueue_head(&fwta.wq);
	fwta.notified = 0;
	fwta.rx_fifo_size = VAS_FAULT_WIN_FIFO_SIZE;
	fwta.rx_fifo = kmalloc(fwta.rx_fifo_size, GFP_KERNEL);
	if (!fwta.rx_fifo) {
		pr_err("VAS: Unable to alloc %d bytes for rx_fifo\n",
				fwta.rx_fifo_size);
		return -1;
	}

	/*
	 * Create a worker thread that processes the fault CRBs.
	 */
	fwt_thr = kthread_create_on_node(fault_win_thread, &fwta, 0, name, 0);
	if (IS_ERR(fwt_thr))
		goto free_mem;

	memset(&attr, 0, sizeof(attr));
	attr.rx_fifo_size = fwta.rx_fifo_size;
	attr.rx_fifo = fwta.rx_fifo;

	attr.wcreds_max = VAS_FAULT_WIN_WCREDS;
	attr.tc_mode = VAS_THRESH_DISABLED;
	attr.pin_win = true;
	attr.tx_win_ord_mode = true;
	attr.rx_win_ord_mode = true;
	attr.fault_win = true;

	/*
	 * 3.1.4.32: Local Notification Control Register. notify_disable is
	 * true and interrupt disable is false for Fault windows
	 */
	attr.notify_disable = true;

	attr.lnotify_lpid = 0;
	attr.lnotify_pid = task_pid_nr(fwt_thr);
	attr.lnotify_tid = task_pid_nr(fwt_thr);

	fault_win = vas_rx_win_open(0, 0, VAS_COP_TYPE_FAULT, &attr);
	if (IS_ERR(fault_win)) {
		pr_err("VAS: Error %ld opening fault window\n",
					PTR_ERR(fault_win));
		goto stop_thread;
	}

	/*
	 * Wakeup fault thread after fault rx window is opened.
	 */
	wake_up_process(fwt_thr);

	pr_err("VAS: Created fault window, %d, LPID/PID/TID [%d/%d/%d]\n",
			fault_win->winid, attr.lnotify_lpid, attr.lnotify_pid,
			attr.lnotify_tid);

	return 0;

stop_thread:
	kthread_stop(fwt_thr);

free_mem:
	kfree(attr.rx_fifo);
	return -1;
}

static void destroy_fault_win(void)
{
	if (vas_win_close(fault_win) < 0)
		pr_err("VAS: error closing fault window\n");

	/*
	 * TODO: fault_win_thread() does not exit unless stopped
	 *	 but check if there can be any race here.
	 */
	kthread_stop(fwt_thr);
	kfree(fwta.rx_fifo);
	pr_err("VAS: Fault thread stopped\n");
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
	int n, c, rc;

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

	/*
	 * Create fault handler thread and window.
	 */
	rc = create_fault_win();
	if (rc < 0)
		goto cleanup;

	return 0;

cleanup:
	kfree(vas_instances);
	return rc;
}

void vas_exit(void)
{
	vas_initialized = 0;
	destroy_fault_win();
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
