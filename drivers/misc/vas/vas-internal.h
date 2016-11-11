/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef VAS_INTERNAL_H
#define VAS_INTERNAL_H
#include <linux/atomic.h>
#include <linux/idr.h>
#include <asm/vas.h>

/*
 * HVWC and UWC BAR.
 *
 * A Power node can have (upto?) 8 Power chips.
 *
 * There is one instance of VAS in each Power chip. Each instance of VAS
 * has 64K windows, which can be used to send/receive messages from
 * software threads and coprocessors.
 *
 * Each window is described by two types of window contexts:
 *
 *	Hypervisor Window Context (HVWC) of size VAS_HVWC_SIZE bytes
 *	OS/User Window Context (UWC) of size VAS_UWC_SIZE bytes.
 *
 * A window context can be viewed as a set of 64-bit registers. The settings
 * of these registers control/determine the behavior of the VAS hardware
 * when messages are sent/received through the window.
 *
 * Each Power chip i.e each instance of VAS, is assigned two distinct ranges
 * (one for each type of context) of Power-bus addresses (aka Base Address
 * Region or BAR) which can be used to access the window contexts in that
 * instance of VAS.
 *
 * From the Power9 MMIO Ranges Spreadsheet:
 *
 * The HVWC BAR is of size 0x40000000000 and for chip 0, the HVWC BAR begins
 * at 0x6019000000000ULL, for chip 1 at 0x0006059000000000 etc.
 *
 * i.e the HVWC for each of the 64K windows on chip 0 can be accessed at the
 * address 0x6019000000000ULL, and HVWC for the 64K windows on chip 1 can be
 * accessed at the address 0x0006059000000000 and so on.
 *
 * Similarly, the UWC BAR is also of size 0x40000000000 and for chip 0,
 * begins at 0x0006019100000000, for chip 1 at 0x0006059100000000 etc.
 *
 * Following macros describe the HVWC and UWC BARs for chip 0. The BARs for
 * the other chips are computed in get_hvwc_mmio_bar() / get_uwc_mmio_bar().
 */
#define VAS_HVWC_MMIO_BAR_BASE		0x0006019100000000ULL
#define VAS_HVWC_MMIO_BAR_SIZE		0x40000000000

#define VAS_UWC_MMIO_BAR_BASE		0x0006019000000000ULL
#define VAS_UWC_MMIO_BAR_SIZE		0x40000000000

/*
 * Hypervisor and OS/USer Window Context sizes
 */
#define VAS_HVWC_SIZE			512
#define VAS_UWC_SIZE			PAGE_SIZE

/*
 * TODO: Get nodes and chip info from device tree.
 */
#define VAS_MAX_NODES			1
#define VAS_MAX_CHIPS_PER_NODE		1

/* Initial per-process credits. We may need to tweak these later */
#define VAS_WCREDS_MIN			16
#define VAS_WCREDS_MAX			64
#define VAS_WCREDS_DEFAULT		64

/*
 * TODO:
 *	- Hardcoded for Power9 but should get from device tree (must
 *	  sync with Skiboot!)
 *	- Increase number of windows to 64K after initial development
 */
#define VAS_MAX_WINDOWS_PER_CHIP	64

#ifdef CONFIG_PPC_4K_PAGES
#	error "TODO: Compute RMA/Paste-address for 4K pages."
#else
#ifndef CONFIG_PPC_64K_PAGES
#	error "Unexpected Page size."
#endif
#endif

/*
 * TODO: Copied from nx-842.h. Move to a common header
 */
/* Get/Set bit fields */
#define MASK_LSH(m)             (__builtin_ffsl(m) - 1)

/* Sigh. nx-842 and skiboot have the parameters in opposite order */
#define GET_FIELD(m, v)          (((v) & (m)) >> MASK_LSH(m))
#define SET_FIELD(m, v, val)                             \
		(((v) & ~(m)) | ((((typeof(v))(val)) << MASK_LSH(m)) & (m)))

/*
 * VAS Window Context Register Offsets and bitmasks.
 * See Section 3.1.4 of VAS Work book
 */
#define VAS_LPID_OFFSET			0x010
#define VAS_LPID			PPC_BITMASK(0, 11)

#define VAS_PID_OFFSET			0x018
#define VAS_PID_ID			PPC_BITMASK(0, 19)

#define VAS_XLATE_MSR_OFFSET		0x020
#define VAS_XLATE_MSR_DR		PPC_BIT(0)
#define VAS_XLATE_MSR_TA		PPC_BIT(1)
#define VAS_XLATE_MSR_PR		PPC_BIT(2)
#define VAS_XLATE_MSR_US		PPC_BIT(3)
#define VAS_XLATE_MSR_HV		PPC_BIT(4)
#define VAS_XLATE_MSR_SF		PPC_BIT(5)
#define VAS_XLATE_MSR_UV		PPC_BIT(6)

#define VAS_XLATE_LPCR_OFFSET		0x028
#define VAS_XLATE_LPCR_PAGE_SIZE	PPC_BITMASK(0, 2)
#define VAS_XLATE_LPCR_ISL		PPC_BIT(3)
#define VAS_XLATE_LPCR_TC		PPC_BIT(4)
#define VAS_XLATE_LPCR_SC		PPC_BIT(5)

#define VAS_XLATE_CTL_OFFSET		0x030
#define VAS_XLATE_MODE			PPC_BITMASK(0, 1)

#define VAS_AMR_OFFSET			0x040
#define VAS_AMR				PPC_BITMASK(0, 63)

#define VAS_SEIDR_OFFSET		0x048
#define VAS_SEIDR			PPC_BITMASK(0, 63)

#define VAS_FAULT_TX_WIN_OFFSET		0x050
#define VAS_FAULT_TX_WIN		PPC_BITMASK(48, 63)

#define VAS_OSU_INTR_SRC_RA_OFFSET	0x060
#define VAS_OSU_INTR_SRC_RA		PPC_BITMASK(8, 63)

#define VAS_HV_INTR_SRC_RA_OFFSET	0x070
#define VAS_HV_INTR_SRC_RA		PPC_BITMASK(8, 63)

#define VAS_PSWID_OFFSET		0x078
#define VAS_PSWID_EA_HANDLE		PPC_BITMASK(0, 31)

#define VAS_SPARE1_OFFSET		0x080
#define VAS_SPARE2_OFFSET		0x088
#define VAS_SPARE3_OFFSET		0x090
#define VAS_SPARE4_OFFSET		0x130
#define VAS_SPARE5_OFFSET		0x160
#define VAS_SPARE6_OFFSET		0x188

#define VAS_LFIFO_BAR_OFFSET		0x0A0
#define VAS_LFIFO_BAR			PPC_BITMASK(8, 53)
#define VAS_PAGE_MIGRATION_SELECT	PPC_BITMASK(54, 56)

#define VAS_LDATA_STAMP_CTL_OFFSET	0x0A8
#define VAS_LDATA_STAMP			PPC_BITMASK(0, 1)
#define VAS_XTRA_WRITE			PPC_BIT(2)

#define VAS_LDMA_CACHE_CTL_OFFSET	0x0B0
#define VAS_LDMA_TYPE			PPC_BITMASK(0, 1)

#define VAS_LRFIFO_PUSH_OFFSET		0x0B8
#define VAS_LRFIFO_PUSH			PPC_BITMASK(0, 15)

#define VAS_CURR_MSG_COUNT_OFFSET	0x0C0
#define VAS_CURR_MSG_COUNT		PPC_BITMASK(0, 7)

#define VAS_LNOTIFY_AFTER_COUNT_OFFSET	0x0C8
#define VAS_LNOTIFY_AFTER_COUNT		PPC_BITMASK(0, 7)

#define VAS_LRX_WCRED_OFFSET		0x0E0
#define VAS_LRX_WCRED			PPC_BITMASK(0, 15)

#define VAS_LRX_WCRED_ADDER_OFFSET	0x190
#define VAS_LRX_WCRED_ADDER		PPC_BITMASK(0, 15)

#define VAS_TX_WCRED_OFFSET		0x0F0
#define VAS_TX_WCRED			PPC_BITMASK(4, 15)

#define VAS_TX_WCRED_ADDER_OFFSET	0x1A0
#define VAS_TX_WCRED_ADDER		PPC_BITMASK(4, 15)

#define VAS_LFIFO_SIZE_OFFSET		0x100
#define VAS_LFIFO_SIZE			PPC_BITMASK(0, 3)

#define VAS_WINCTL_OFFSET		0x108
#define VAS_WINCTL_OPEN			PPC_BIT(0)
#define VAS_WINCTL_REJ_NO_CREDIT	PPC_BIT(1)
#define VAS_WINCTL_PIN			PPC_BIT(2)
#define VAS_WINCTL_TX_WCRED_MODE	PPC_BIT(3)
#define VAS_WINCTL_RX_WCRED_MODE	PPC_BIT(4)
#define VAS_WINCTL_TXWIN_ORD_MODE	PPC_BIT(5)
#define VAS_WINCTL_RXWIN_ORD_MODE	PPC_BIT(6)
#define VAS_WINCTL_RSVD_TXBUF		PPC_BIT(7)
#define VAS_WINCTL_THRESH_CTL		PPC_BITMASK(8, 9)
#define VAS_WINCTL_FAULT_WIN		PPC_BIT(10)
#define VAS_WINCTL_NX_WIN		PPC_BIT(11)

#define VAS_WIN_STATUS_OFFSET		0x110
#define VAS_WIN_BUSY			PPC_BIT(1)

#define VAS_WIN_CTX_CACHING_CTL_OFFSET	0x118
#define VAS_CASTOUT_REQ			PPC_BIT(0)
#define VAS_PUSH_TO_MEM			PPC_BIT(1)
#define VAS_WIN_CACHE_STATUS		PPC_BIT(4)

#define VAS_TX_RSVD_BUF_COUNT_OFFSET	0x120
#define VAS_RXVD_BUF_COUNT		PPC_BITMASK(58, 63)

#define VAS_LRFIFO_WIN_PTR_OFFSET	0x128
#define VAS_LRX_WIN_ID			PPC_BITMASK(0, 15)

/*
 * Local Notification Control Register controls what happens in _response_
 * to a paste command and hence applies only to receive windows.
 */
#define VAS_LNOTIFY_CTL_OFFSET		0x138
#define VAS_NOTIFY_DISABLE		PPC_BIT(0)
#define VAS_INTR_DISABLE		PPC_BIT(1)
#define VAS_NOTIFY_EARLY		PPC_BIT(2)
#define VAS_NOTIFY_OSU_INTR		PPC_BIT(3)

#define VAS_LNOTIFY_PID_OFFSET		0x140
#define VAS_LNOTIFY_PID			PPC_BITMASK(0, 19)

#define VAS_LNOTIFY_LPID_OFFSET		0x148
#define VAS_LNOTIFY_LPID		PPC_BITMASK(0, 11)

#define VAS_LNOTIFY_TID_OFFSET		0x150
#define VAS_LNOTIFY_TID			PPC_BITMASK(0, 15)

#define VAS_LNOTIFY_SCOPE_OFFSET	0x158
#define VAS_LNOTIFY_MIN_SCOPE		PPC_BITMASK(0, 1)
#define VAS_LNOTIFY_MAX_SCOPE		PPC_BITMASK(2, 3)

#define VAS_NX_UTIL_OFFSET		0x1B0
#define VAS_NX_UTIL			PPC_BITMASK(0, 63)

/* SE: Side effects */
#define VAS_NX_UTIL_SE_OFFSET		0x1B8
#define VAS_NX_UTIL_SE			PPC_BITMASK(0, 63)

#define VAS_NX_UTIL_ADDER_OFFSET	0x180
#define VAS_NX_UTIL_ADDER		PPC_BITMASK(32, 63)

/*
 * Local Notify Scope Control Register. (Receive windows only).
 */
enum vas_notify_scope {
	VAS_SCOPE_LOCAL,
	VAS_SCOPE_GROUP,
	VAS_SCOPE_VECTORED_GROUP,
	VAS_SCOPE_UNUSED,
};

/*
 * Local DMA Cache Control Register (Receive windows only).
 */
enum vas_dma_type {
	VAS_DMA_TYPE_INJECT,
	VAS_DMA_TYPE_WRITE,
};

/*
 * Local Notify Scope Control Register. (Receive windows only).
 * Not applicable to NX receive windows.
 */
enum vas_notify_after_count {
	VAS_NOTIFY_AFTER_256 = 0,
	VAS_NOTIFY_NONE,
	VAS_NOTIFY_AFTER_2
};

/*
 * One per instance of VAS (i.e one per chip).
 * Each instance will have a separate set of receive windows, one per
 * coprocessor type.
 */
struct vas_instance {
	int node;
	int chip;
	struct ida ida;
	struct mutex mutex;
	struct vas_window *rxwin[VAS_COP_TYPE_MAX];
};

/*
 * In-kernel data structure for a VAS window. One per window.
 */
struct vas_window {
	/* Fields common to Send and receive windows */
	struct vas_instance *vinst;
	int winid;
	bool txwin;		/* True if send window */
	bool nx_win;		/* True if NX window */
	void *hvwc_map;		/* HV window context */
	void *uwc_map;		/* OS/User window context */

	/* Fields applicable only to send windows */
	void *paste_kaddr;
	char *paste_addr_name;
	struct vas_window *rxwin;

	/* Feilds applicable only to receive windows */
	enum vas_cop_type cop;
	atomic_t num_txwins;

	int32_t hwirq;
	uint64_t irq_port;
};

/*
 * A VAS Window context is a 512-byte area in the hardware that contains
 * a set of 64-bit registers. Individual bit-fields in these registers
 * determine the configuration/operation of the hardware. struct vas_winctx
 * is a container for the register fields in the window context.
 * One per window.
 */
struct vas_winctx {
	void *rx_fifo;
	int rx_fifo_size;
	int wcreds_max;
	int rsvd_txbuf_count;

	bool user_win;
	bool nx_win;
	bool fault_win;
	bool rsvd_txbuf_enable;
	bool pin_win;
	bool rej_no_credit;
	bool tx_wcred_mode;
	bool rx_wcred_mode;
	bool tx_win_ord_mode;
	bool rx_win_ord_mode;
	bool data_stamp;
	bool xtra_write;
	bool notify_disable;
	bool intr_disable;
	bool notify_early;
	bool notify_os_intr_reg;

	int lpid;
	int pid;
	int lnotify_lpid;
	int lnotify_pid;
	int lnotify_tid;
	int pswid;
	int rx_win_id;
	int fault_win_id;
	uint64_t irq_port;

	enum vas_dma_type dma_type;
	enum vas_thresh_ctl tc_mode;
	enum vas_notify_scope min_scope;
	enum vas_notify_scope max_scope;
	enum vas_notify_after_count notify_after_count;
};

#endif
