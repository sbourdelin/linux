/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <asm/vas.h>
#include "vas-internal.h"

static int fault_winid;

/*
 * Using the node, chip and window id for the send winow identified by
 * @window, compute and return the Power Bus address to which a sender
 * could issue a paste instruction for this window.
 *
 * Refer to Tables 1.1 through 1.4 in Section 1.3.3.1 (Send Message w/Paste
 * Commands (cl_rma_w)) of VAS P9 Workbook for the PowerBus Address usage
 * in VAS.
 *
 * With 64K mode and Large SMP Mode the bits are used as follows:
 *
 *	Bits	Values		Comments
 *	--------------------------------------
 *	0:7     0b 0000_0000    Reserved
 *	8:12    0b 0000_1       System id/Foreign Index 0:4
 *	13:14   0b 00           Foreign Index 5:6
 *
 *	15:18   0 throuh 15     Node id (0 through 15)
 *	19:21   0 through 7     Chip id (0 throuh 7)
 *	22:23   0b 00           Unused, Foreign index 7:8
 *
 *	24:31   0b 0000_0000    RPN 0:7, Reserved
 *	32:47   0 through 64K   Send Window Id
 *	48:51   0b 0000         Spare
 *
 *	52      0b 0            Reserved
 *	53      0b 1            Report Enable (Set to 1 for NX).
 *	54      0b 0            Reserved
 *
 *	55:56   0b 00           Snoop Bus
 *	57:63   0b 0000_000     Reserved
 *
 * Except for a few bits, the small SMP mode computation is similar.
 *
 * TODO: Detect and compute address for small SMP mode.
 *
 * Example: For Node 0, Chip 0, Window id 4, Report Enable 1:
 *
 *     Byte0    Byte1    Byte2    Byte3    Byte4    Byte5    Byte6    Byte7
 *     00000000 00001000 00000000 00000000 00000000 00000100 00000100 00000000
 *                                         |               |      |
 *                                         +-------+-------+      v
 *                                                 |          Report Enable
 *                                                 v
 *                                             Window id 4
 *
 *     Thus, the paste address is 0x00080000_00040400.
 */
#define RMA_LSMP_64K_SYS_ID	PPC_BITMASK(8, 12)
#define RMA_LSMP_64K_NODE_ID	PPC_BITMASK(15, 18)
#define RMA_LSMP_64K_CHIP_ID	PPC_BITMASK(19, 21)
#define RMA_LSMP_64K_TX_WIN_ID	PPC_BITMASK(32, 47)
#define RMA_LSMP_REPORT_ENABLE	PPC_BIT(53)

uint64_t compute_paste_address(struct vas_window *window, int *size)
{
	int node, chip, winid;
	uint64_t val = 0ULL;

	node = window->vinst->node;
	chip = window->vinst->chip;
	winid = window->winid;

	*size = PAGE_SIZE;

	val = SET_FIELD(RMA_LSMP_64K_SYS_ID, val, 1);
	val = SET_FIELD(RMA_LSMP_64K_NODE_ID, val, node);
	val = SET_FIELD(RMA_LSMP_64K_CHIP_ID, val, chip);
	val = SET_FIELD(RMA_LSMP_64K_TX_WIN_ID, val, winid);
	val = SET_FIELD(RMA_LSMP_REPORT_ENABLE, val, 1);
	pr_debug("%swin #%d: Paste address 0x%llx\n",
			window->txwin ? "Tx" : "Rx",  winid, val);
	return val;
}

static void get_hvwc_mmio_bar(struct vas_window *window,
			uint64_t *start, int *len)
{
	uint64_t pbaddr;
	int instance;

	instance = window->vinst->node * 8 + window->vinst->chip;
	pbaddr = VAS_HVWC_MMIO_BAR_BASE + instance * VAS_HVWC_MMIO_BAR_SIZE;

	*start = pbaddr + window->winid * VAS_HVWC_SIZE;
	*len = VAS_HVWC_SIZE;
}

static void get_uwc_mmio_bar(struct vas_window *window,
			uint64_t *start, int *len)
{
	uint64_t pbaddr;
	int instance;

	instance = window->vinst->node * 8 + window->vinst->chip;
	pbaddr = VAS_UWC_MMIO_BAR_BASE + instance * VAS_UWC_MMIO_BAR_SIZE;

	*start = pbaddr + window->winid * VAS_UWC_SIZE;
	*len = VAS_UWC_SIZE;
}

static void *map_mmio_region(char *name, uint64_t start, int len)
{
	void *map;

	if (!request_mem_region(start, len, name)) {
		pr_devel("%s(): request_mem_region(0x%llx, %d) failed\n",
				__func__, start, len);
		return NULL;
	}

	map = __ioremap(start, len, pgprot_val(pgprot_cached(__pgprot(0))));
	if (!map) {
		pr_devel("%s(): ioremap(0x%llx, %d) failed\n", __func__, start,
				len);
		return NULL;
	}

	return map;
}

/*
 * Unmap the MMIO regions for a window.
 */
void unmap_wc_mmio_bars(struct vas_window *window)
{
	int len;
	uint64_t busaddr_start;

	if (window->paste_kaddr) {
		iounmap(window->paste_kaddr);
		busaddr_start = compute_paste_address(window, &len);
		pr_debug("Releasing pbaddr region [0x%llx, %d]\n",
				busaddr_start, len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}

	if (window->hvwc_map) {
		iounmap(window->hvwc_map);
		get_hvwc_mmio_bar(window, &busaddr_start, &len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}

	if (window->uwc_map) {
		iounmap(window->uwc_map);
		get_uwc_mmio_bar(window, &busaddr_start, &len);
		release_mem_region((phys_addr_t)busaddr_start, len);
	}
}

/*
 * Find the Hypervisor Window Context (HVWC) MMIO Base Address Region and the
 * OS/User Window Context (UWC) MMIO Base Address Region for the given window.
 * Map these bus addresses and save the mapped kernel addresses in @window.
 */
int map_wc_mmio_bars(struct vas_window *window)
{
	int len;
	uint64_t start;

	window->hvwc_map = window->uwc_map = NULL;

	get_hvwc_mmio_bar(window, &start, &len);
	window->hvwc_map = map_mmio_region("HVWCM_Window", start, len);

	pr_debug("Win #%d: Map hvwc 0x%p -> [0x%llx,%d]\n", window->winid,
			window->hvwc_map, start, len);

	get_uwc_mmio_bar(window, &start, &len);
	window->uwc_map = map_mmio_region("UWCM_Window", start, len);

	pr_debug("Win #%d: Map uvwc 0x%p -> [0x%llx,%d]\n", window->winid,
			window->uwc_map, start, len);

	if (!window->hvwc_map || !window->uwc_map)
		return -1;

	return 0;
}

/*
 * Reset all valid registers in the HV and OS/User Window Contexts for
 * the window identified by @window.
 *
 * NOTE: We cannot really use a for loop to reset window context. Not all
 *	 offsets in a window context are valid registers and the valid
 *	 registers are not sequential. And, we can only write to offsets
 *	 with valid registers (or is that only in Simics?).
 */
void reset_window_regs(struct vas_window *window)
{
	write_hvwc_reg(window, VREG(LPID), 0ULL);
	write_hvwc_reg(window, VREG(PID), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_MSR), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_LPCR), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_CTL), 0ULL);
	write_hvwc_reg(window, VREG(AMR), 0ULL);
	write_hvwc_reg(window, VREG(SEIDR), 0ULL);
	write_hvwc_reg(window, VREG(FAULT_TX_WIN), 0ULL);
	write_hvwc_reg(window, VREG(OSU_INTR_SRC_RA), 0ULL);
	write_hvwc_reg(window, VREG(HV_INTR_SRC_RA), 0ULL);
	write_hvwc_reg(window, VREG(PSWID), 0ULL);
	write_hvwc_reg(window, VREG(SPARE1), 0ULL);
	write_hvwc_reg(window, VREG(SPARE2), 0ULL);
	write_hvwc_reg(window, VREG(SPARE3), 0ULL);
	write_hvwc_reg(window, VREG(SPARE4), 0ULL);
	write_hvwc_reg(window, VREG(SPARE5), 0ULL);
	write_hvwc_reg(window, VREG(SPARE6), 0ULL);
	write_hvwc_reg(window, VREG(LFIFO_BAR), 0ULL);
	write_hvwc_reg(window, VREG(LDATA_STAMP_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LDMA_CACHE_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LRFIFO_PUSH), 0ULL);
	write_hvwc_reg(window, VREG(CURR_MSG_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_AFTER_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LRX_WCRED), 0ULL);
	write_hvwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(LFIFO_SIZE), 0ULL);
	write_hvwc_reg(window, VREG(WINCTL), 0ULL);
	write_hvwc_reg(window, VREG(WIN_STATUS), 0ULL);
	write_hvwc_reg(window, VREG(WIN_CTX_CACHING_CTL), 0ULL);
	write_hvwc_reg(window, VREG(TX_RSVD_BUF_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LRFIFO_WIN_PTR), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_PID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_LPID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_TID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_SCOPE), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_SE), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_ADDER), 0ULL);

	/*
	 * TODO: The Send and receive window credit adder registers are
	 *	also accessible from HVWC and have been initialized above.
	 *	We probably don't need to initialize from the OS/User
	 *	Window Context? Initialize anyway for now.
	 */
	write_uwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);
	write_uwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
}

/*
 * Initialize window context registers related to Address Translation.
 * These registers are common to send/receive windows although they
 * differ for user/kernel windows. As we resolve the TODOs we may
 * want to add fields to vas_winctx and move the intialization to
 * init_vas_winctx_regs().
 */
static void init_xlate_regs(struct vas_window *window, bool user_win)
{
	uint64_t lpcr, msr, val;

	reset_window_regs(window);

	msr = mfmsr();
	WARN_ON_ONCE(!(msr & MSR_SF));
	val = 0ULL;
	if (user_win) {
		val = SET_FIELD(VAS_XLATE_MSR_DR, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_TA, val, false);
		val = SET_FIELD(VAS_XLATE_MSR_PR, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_US, val, false);
		val = SET_FIELD(VAS_XLATE_MSR_HV, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_SF, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_UV, val, false);
	} else {
		val = SET_FIELD(VAS_XLATE_MSR_DR, val, false);
		val = SET_FIELD(VAS_XLATE_MSR_TA, val, false);
		val = SET_FIELD(VAS_XLATE_MSR_PR, val, msr & MSR_PR);
		val = SET_FIELD(VAS_XLATE_MSR_US, val, false);
		val = SET_FIELD(VAS_XLATE_MSR_HV, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_SF, val, true);
		val = SET_FIELD(VAS_XLATE_MSR_UV, val, false);
	}
	write_hvwc_reg(window, VREG(XLATE_MSR), val);

	lpcr = mfspr(SPRN_LPCR);
	val = 0ULL;
	/*
	 * NOTE: From Section 5.7.6.1 Segment Lookaside Buffer of the
	 *	 Power ISA, v2.07, Page size encoding is 0 = 4KB, 5 = 64KB.
	 *
	 * NOTE: From Section 1.3.1, Address Translation Context of the
	 *	 Nest MMU Workbook, LPCR_SC should be 0 for Power9.
	 */
	val = SET_FIELD(VAS_XLATE_LPCR_PAGE_SIZE, val, 5);
	val = SET_FIELD(VAS_XLATE_LPCR_ISL, val, lpcr & LPCR_ISL);
	val = SET_FIELD(VAS_XLATE_LPCR_TC, val, lpcr & LPCR_TC);
	val = SET_FIELD(VAS_XLATE_LPCR_SC, val, 0);
	write_hvwc_reg(window, VREG(XLATE_LPCR), val);

	/*
	 * Section 1.3.1 (Address translation Context) of NMMU workbook.
	 *	0b00	Hashed Page Table mode
	 *	0b01	Reserved
	 *	0b10	Radix on HPT - not supported in P9
	 *	0b11	Radix on Radix (only mode supported in Linux on P9).
	 */
	val = 0ULL;
	val = SET_FIELD(VAS_XLATE_MODE, val, 0x11);
	write_hvwc_reg(window, VREG(XLATE_CTL), val);

	/*
	 * TODO: Can we mfspr(AMR) even for user windows?
	 */
	val = 0ULL;
	val = SET_FIELD(VAS_AMR, val, mfspr(SPRN_AMR));
	write_hvwc_reg(window, VREG(AMR), val);

	/*
	 * TODO: Assuming Secure Executable ID Register (SEIDR) is only used
	 *	 in the ultravisor mode. Since MSR(UV) is 0 for now, set SEIDR
	 *	 to 0 as well, although we should 'mfspr(SEIDR)' at some point.
	 */
	val = 0ULL;
	val = SET_FIELD(VAS_SEIDR, val, 0);
	write_hvwc_reg(window, VREG(SEIDR), val);
}

/*
 * Initialize Reserved Send Buffer Count for the send window. It involves
 * writing to the register, reading it back to confirm that the hardware
 * has enough buffers to reserve. See section 1.3.1.2.1 of VAS workbook.
 *
 * Since we can only make a best-effort attempt to fulfill the request,
 * we don't return any errors if we cannot.
 *
 * TODO: Reserved (aka dedicated) send buffers are not supported yet.
 */
static void init_rsvd_tx_buf_count(struct vas_window *txwin,
				struct vas_winctx *winctx)
{
	write_hvwc_reg(txwin, VREG(TX_RSVD_BUF_COUNT), 0ULL);
}

/*
 * Compute the log2() of the FIFO size expressed as kilobytes. It is intended
 * to be used to initialize the Local FIFO Size Register defined in Section
 * 3.14.25 of the VAS Workbook.
 */
static int map_fifo_size_to_reg(int fifo_size)
{
	int kb;
	int map;

	kb = fifo_size / 1024;
	if (!kb)
		kb = 1;

	map = -1;
	while (kb) {
		kb >>= 1;
		map++;
	}

	return map;
}

/*
 * init_winctx_regs()
 *	Initialize window context registers for a receive window.
 *	Except for caching control and marking window open, the registers
 *	are initialized in the order listed in Section 3.1.4 (Window Context
 *	Cache Register Details) of the VAS workbook although they don't need
 *	to be.
 *
 * Design note: For NX receive windows, NX allocates the FIFO buffer in OPAL
 *	(so that it can get a large contiguous area) and passes that buffer
 *	to kernel via device tree. We now write that buffer address to the
 *	FIFO BAR. Would it make sense to do this all in OPAL? i.e have OPAL
 *	write the per-chip RX FIFO addresses to the windows during boot-up
 *	as a one-time task? That could work for NX but what about other
 *	receivers?  Let the receivers tell us the rx-fifo buffers for now.
 */
int init_winctx_regs(struct vas_window *window, struct vas_winctx *winctx)
{
	uint64_t val;
	int fifo_size;

	val = 0ULL;
	val = SET_FIELD(VAS_LPID, val, winctx->lpid);
	write_hvwc_reg(window, VREG(LPID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_PID_ID, val, winctx->pid);
	write_hvwc_reg(window, VREG(PID), val);

	init_xlate_regs(window, false);

	val = 0ULL;
	val = SET_FIELD(VAS_FAULT_TX_WIN, val, fault_winid);
	write_hvwc_reg(window, VREG(FAULT_TX_WIN), val);

	/* In PowerNV, interrupts go to HV. */
	write_hvwc_reg(window, VREG(OSU_INTR_SRC_RA), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_HV_INTR_SRC_RA, val, window->irq_port);
	write_hvwc_reg(window, VREG(HV_INTR_SRC_RA), val);

	val = 0ULL;
	val = SET_FIELD(VAS_PSWID_EA_HANDLE, val, winctx->pswid);
	write_hvwc_reg(window, VREG(PSWID), val);

	write_hvwc_reg(window, VREG(SPARE1), 0ULL);
	write_hvwc_reg(window, VREG(SPARE2), 0ULL);
	write_hvwc_reg(window, VREG(SPARE3), 0ULL);

	/* See also: Design note in function header */
	val = 0ULL;
	val = SET_FIELD(VAS_LFIFO_BAR, val, __pa(winctx->rx_fifo));
	val = SET_FIELD(VAS_PAGE_MIGRATION_SELECT, val, 0);
	write_hvwc_reg(window, VREG(LFIFO_BAR), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LDATA_STAMP, val, winctx->data_stamp);
	write_hvwc_reg(window, VREG(LDATA_STAMP_CTL), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LDMA_TYPE, val, winctx->dma_type);
	write_hvwc_reg(window, VREG(LDMA_CACHE_CTL), val);

	write_hvwc_reg(window, VREG(LRFIFO_PUSH), 0ULL);
	write_hvwc_reg(window, VREG(CURR_MSG_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_AFTER_COUNT), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_LRX_WCRED, val, winctx->wcreds_max);
	write_hvwc_reg(window, VREG(LRX_WCRED), val);

	write_hvwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);

	val = 0ULL;
	fifo_size = winctx->rx_fifo_size;
	val = SET_FIELD(VAS_LFIFO_SIZE, val, map_fifo_size_to_reg(fifo_size));
	write_hvwc_reg(window, VREG(LFIFO_SIZE), val);

	/* Update window control and caching control registers last so
	 * we mark the window open only after fully initializing it and
	 * pushing context to cache.
	 */

	write_hvwc_reg(window, VREG(WIN_STATUS), 0ULL);

	init_rsvd_tx_buf_count(window, winctx);

	/* for a send window, point to the matching receive window */
	val = 0ULL;
	val = SET_FIELD(VAS_LRX_WIN_ID, val, winctx->rx_win_id);
	write_hvwc_reg(window, VREG(LRFIFO_WIN_PTR), val);

	write_hvwc_reg(window, VREG(SPARE4), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_NOTIFY_DISABLE, val, winctx->notify_disable);
	val = SET_FIELD(VAS_INTR_DISABLE, val, winctx->intr_disable);
	val = SET_FIELD(VAS_NOTIFY_EARLY, val, winctx->notify_early);
	val = SET_FIELD(VAS_NOTIFY_OSU_INTR, val, winctx->notify_os_intr_reg);
	write_hvwc_reg(window, VREG(LNOTIFY_CTL), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_PID, val, winctx->lnotify_pid);
	write_hvwc_reg(window, VREG(LNOTIFY_PID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_LPID, val, winctx->lnotify_lpid);
	write_hvwc_reg(window, VREG(LNOTIFY_LPID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_TID, val, winctx->lnotify_tid);
	write_hvwc_reg(window, VREG(LNOTIFY_TID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_MIN_SCOPE, val, winctx->min_scope);
	val = SET_FIELD(VAS_LNOTIFY_MAX_SCOPE, val, winctx->max_scope);
	write_hvwc_reg(window, VREG(LNOTIFY_SCOPE), val);

	write_hvwc_reg(window, VREG(SPARE5), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_SE), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(SPARE6), 0ULL);

	/* Finally, push window context to memory and... */
	val = 0ULL;
	val = SET_FIELD(VAS_PUSH_TO_MEM, val, 1);
	write_hvwc_reg(window, VREG(WIN_CTX_CACHING_CTL), val);

	/* ... mark the window open for business */
	val = 0ULL;
	val = SET_FIELD(VAS_WINCTL_REJ_NO_CREDIT, val, winctx->rej_no_credit);
	val = SET_FIELD(VAS_WINCTL_PIN, val, winctx->pin_win);
	val = SET_FIELD(VAS_WINCTL_TX_WCRED_MODE, val, winctx->tx_wcred_mode);
	val = SET_FIELD(VAS_WINCTL_RX_WCRED_MODE, val, winctx->rx_wcred_mode);
	val = SET_FIELD(VAS_WINCTL_TXWIN_ORD_MODE, val, winctx->tx_win_ord_mode);
	val = SET_FIELD(VAS_WINCTL_RXWIN_ORD_MODE, val, winctx->rx_win_ord_mode);
	val = SET_FIELD(VAS_WINCTL_FAULT_WIN, val, winctx->fault_win);
	val = SET_FIELD(VAS_WINCTL_NX_WIN, val, winctx->nx_win);
	val = SET_FIELD(VAS_WINCTL_OPEN, val, 1);
	write_hvwc_reg(window, VREG(WINCTL), val);

	return 0;
}

/* stub for now */
int vas_window_reset(struct vas_instance *vinst, int winid)
{
	return 0;
}
